#include "AvenorStripTerrainGenerator.h"

#include "AvenorTerrainData.h"

#include "ActorFactories/ActorFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Serialization/ArchiveLoadCompressedProxy.h"
#include "Serialization/ArchiveSaveCompressedProxy.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "Modules/ModuleManager.h"
#include "MeshPartition.h"
#include "MeshPartitionMeshView.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/DateTime.h"
#include "Misc/MessageDialog.h"
#include "Misc/SecureHash.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyOceanActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterSplineComponent.h"
#include "WaterSplineMetadata.h"
#include "WaterBodyHeightmapSettings.h"
#include "WaterBodyWeightmapSettings.h"
#include "WaterBrushEffects.h"
#include "WaterCurveSettings.h"
#include "WaterFalloffSettings.h"
#include "Components/SplineComponent.h"
#include "ProceduralMeshComponent.h"
#include "Modifiers/MeshPartitionSplineRemeshModifier.h"
#include "Modifiers/MeshPartitionRemeshModifier.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include <queue>
#include <vector>

namespace UE::Avenor::Strip
{
static const FName GeneratedWaterTag(TEXT("AvenorStripWater"));
static const FName GeneratedRefinementTag(TEXT("AvenorStripRefinement"));
static const FName ElevationChannel(TEXT("Elevation"));
static const FName SlopeChannel(TEXT("Slope"));
static const FName WetnessChannel(TEXT("Wetness"));
static const FName RiverChannel(TEXT("River"));
static const FName LakeChannel(TEXT("Lake"));
static const FName MountainChannel(TEXT("Mountain"));
static const FName HillChannel(TEXT("Hill"));
static const FName DesertChannel(TEXT("Desert"));
static const FName PlainsChannel(TEXT("Plains"));

static FName MakeWaterOwnerTag(const AAvenorStripTerrainGenerator& Generator)
{
    return FName(*FString::Printf(
        TEXT("AvenorStripOwner_%s"),
        *Generator.GetFName().ToString()
    ));
}

static bool IsWaterOwnerTag(const FName& Tag)
{
    return Tag.ToString().StartsWith(TEXT("AvenorStripOwner_"));
}

// SplineRadius has no public C++ setter in UE 5.8.1. Reflection is therefore
// the only available editor-code route, but it is deliberately validated so
// an engine rename produces a visible failure instead of a silent no-op.
static bool SetSplineRemeshRadius(UE::MeshPartition::USplineRemeshModifier& Modifier, float Radius)
{
    if (FFloatProperty* Property = FindFProperty<FFloatProperty>(
        UE::MeshPartition::USplineRemeshModifier::StaticClass(), TEXT("SplineRadius")
    ))
    {
        Property->SetPropertyValue_InContainer(&Modifier, Radius);
        return true;
    }

    UE_LOG(
        LogTemp,
        Error,
        TEXT("Avenor refinement: UE 5.8.1 USplineRemeshModifier no longer exposes the reflected SplineRadius property; refinement spline was not created.")
    );
    return false;
}

static double Smooth01(double Value)
{
    const double T = FMath::Clamp(Value, 0.0, 1.0);
    return T * T * (3.0 - 2.0 * T);
}

static double Quintic(double Value)
{
    const double T = FMath::Clamp(Value, 0.0, 1.0);
    return T * T * T * (T * (T * 6.0 - 15.0) + 10.0);
}

static double Noise(
    const FVector2D& Position,
    double Scale,
    const FVector2D& Offset
)
{
    return FMath::PerlinNoise2D(
        (Position + Offset) / FMath::Max(1.0, Scale)
    );
}

static double Fbm(
    const FVector2D& Position,
    double Scale,
    const FVector2D& Offset,
    int32 Octaves,
    double Gain = 0.5,
    double Lacunarity = 2.0
)
{
    double Sum = 0.0;
    double Weight = 1.0;
    double WeightSum = 0.0;
    double FrequencyScale = FMath::Max(1.0, Scale);
    for (int32 Octave = 0; Octave < Octaves; ++Octave)
    {
        Sum += Noise(Position, FrequencyScale, Offset) * Weight;
        WeightSum += Weight;
        Weight *= Gain;
        FrequencyScale /= Lacunarity;
    }
    return WeightSum > 0.0 ? Sum / WeightSum : 0.0;
}

static double RidgedFbm(
    const FVector2D& Position,
    double Scale,
    const FVector2D& Offset,
    int32 Octaves
)
{
    double Sum = 0.0;
    double Weight = 1.0;
    double WeightSum = 0.0;
    double FrequencyScale = FMath::Max(1.0, Scale);
    for (int32 Octave = 0; Octave < Octaves; ++Octave)
    {
        double Ridge = 1.0 - FMath::Abs(Noise(Position, FrequencyScale, Offset));
        Ridge *= Ridge;
        Sum += Ridge * Weight;
        WeightSum += Weight;
        Weight *= 0.52;
        FrequencyScale /= 2.05;
    }
    return WeightSum > 0.0 ? Sum / WeightSum : 0.0;
}

static FVector2D Rotate90(const FVector2D& Vector)
{
    return FVector2D(-Vector.Y, Vector.X);
}

static double SegmentDistance(
    const FVector2D& Point,
    const FVector2D& A,
    const FVector2D& B,
    double* OutAlpha = nullptr
)
{
    const FVector2D Segment = B - A;
    const double LengthSquared = Segment.SizeSquared();
    const double Alpha = LengthSquared > UE_DOUBLE_KINDA_SMALL_NUMBER
        ? FMath::Clamp(
            FVector2D::DotProduct(Point - A, Segment) / LengthSquared,
            0.0,
            1.0
        )
        : 0.0;
    if (OutAlpha)
    {
        *OutAlpha = Alpha;
    }
    return FVector2D::Distance(Point, A + Segment * Alpha);
}

static bool IsInsidePolygon(
    const FVector2D& Point,
    const TArray<FVector>& Polygon,
    double* OutEdgeDistance = nullptr
)
{
    bool bInside = false;
    double EdgeDistance = TNumericLimits<double>::Max();
    for (int32 Index = 0; Index < Polygon.Num(); ++Index)
    {
        const FVector2D A(Polygon[Index]);
        const FVector2D B(Polygon[(Index + 1) % Polygon.Num()]);
        EdgeDistance = FMath::Min(EdgeDistance, SegmentDistance(Point, A, B));
        if ((A.Y > Point.Y) != (B.Y > Point.Y))
        {
            const double CrossingX = A.X +
                (Point.Y - A.Y) * (B.X - A.X) /
                    FMath::Max(UE_DOUBLE_KINDA_SMALL_NUMBER, FMath::Abs(B.Y - A.Y)) *
                    FMath::Sign(B.Y - A.Y);
            if (Point.X < CrossingX)
            {
                bInside = !bInside;
            }
        }
    }
    if (OutEdgeDistance)
    {
        *OutEdgeDistance = EdgeDistance;
    }
    return bInside;
}

static TArray<FVector> ChaikinSmooth(
    const TArray<FVector>& Input,
    bool bClosed,
    int32 Iterations
)
{
    TArray<FVector> Points = Input;
    for (int32 Iteration = 0;
         Iteration < Iterations && Points.Num() >= (bClosed ? 3 : 2);
         ++Iteration)
    {
        TArray<FVector> Result;
        if (!bClosed)
        {
            Result.Add(Points[0]);
        }
        const int32 SegmentCount = bClosed ? Points.Num() : Points.Num() - 1;
        Result.Reserve(SegmentCount * 2 + 2);
        for (int32 Index = 0; Index < SegmentCount; ++Index)
        {
            const FVector& A = Points[Index];
            const FVector& B = Points[(Index + 1) % Points.Num()];
            Result.Add(FMath::Lerp(A, B, 0.25));
            Result.Add(FMath::Lerp(A, B, 0.75));
        }
        if (!bClosed)
        {
            Result.Add(Points.Last());
        }
        Points = MoveTemp(Result);
    }
    return Points;
}

static TArray<FVector> ResamplePolyline(
    const TArray<FVector>& Input,
    double Spacing,
    bool bClosed
)
{
    if (Input.Num() < 2)
    {
        return Input;
    }
    const int32 SegmentCount = bClosed ? Input.Num() : Input.Num() - 1;
    TArray<double> SegmentLengths;
    SegmentLengths.SetNum(SegmentCount);
    double TotalLength = 0.0;
    for (int32 Index = 0; Index < SegmentCount; ++Index)
    {
        SegmentLengths[Index] = FVector2D::Distance(
            FVector2D(Input[Index]), FVector2D(Input[(Index + 1) % Input.Num()])
        );
        TotalLength += SegmentLengths[Index];
    }
    if (TotalLength <= UE_DOUBLE_KINDA_SMALL_NUMBER)
    {
        return Input;
    }
    const int32 SampleCount = FMath::Max(
        bClosed ? 3 : 2,
        FMath::CeilToInt(TotalLength / FMath::Max(1.0, Spacing)) + (bClosed ? 0 : 1)
    );
    TArray<FVector> Result;
    Result.Reserve(SampleCount);
    int32 SegmentIndex = 0;
    double SegmentStart = 0.0;
    for (int32 Sample = 0; Sample < SampleCount; ++Sample)
    {
        const double Distance = bClosed
            ? TotalLength * static_cast<double>(Sample) / SampleCount
            : TotalLength * static_cast<double>(Sample) / FMath::Max(1, SampleCount - 1);
        while (SegmentIndex + 1 < SegmentCount &&
               SegmentStart + SegmentLengths[SegmentIndex] < Distance)
        {
            SegmentStart += SegmentLengths[SegmentIndex];
            ++SegmentIndex;
        }
        const double Alpha = FMath::Clamp(
            (Distance - SegmentStart) / FMath::Max(1.0, SegmentLengths[SegmentIndex]),
            0.0,
            1.0
        );
        Result.Add(FMath::Lerp(
            Input[SegmentIndex],
            Input[(SegmentIndex + 1) % Input.Num()],
            Alpha
        ));
    }
    return Result;
}

static TArray<FVector> SimplifyOpenPolyline(
    const TArray<FVector>& Input,
    double Tolerance
)
{
    if (Input.Num() <= 2 || Tolerance <= 0.0)
    {
        return Input;
    }

    TBitArray<> Keep(false, Input.Num());
    Keep[0] = true;
    Keep[Input.Num() - 1] = true;
    TArray<FIntPoint> PendingRanges;
    PendingRanges.Emplace(0, Input.Num() - 1);
    while (!PendingRanges.IsEmpty())
    {
        const FIntPoint Range = PendingRanges.Pop(EAllowShrinking::No);
        if (Range.Y <= Range.X + 1)
        {
            continue;
        }
        double MaximumDistance = 0.0;
        int32 MaximumIndex = INDEX_NONE;
        for (int32 Index = Range.X + 1; Index < Range.Y; ++Index)
        {
            const double Distance = FMath::PointDistToSegment(
                Input[Index], Input[Range.X], Input[Range.Y]
            );
            if (Distance > MaximumDistance)
            {
                MaximumDistance = Distance;
                MaximumIndex = Index;
            }
        }
        if (MaximumIndex != INDEX_NONE && MaximumDistance > Tolerance)
        {
            Keep[MaximumIndex] = true;
            PendingRanges.Emplace(Range.X, MaximumIndex);
            PendingRanges.Emplace(MaximumIndex, Range.Y);
        }
    }

    TArray<FVector> Result;
    Result.Reserve(Input.Num());
    for (int32 Index = 0; Index < Input.Num(); ++Index)
    {
        if (Keep[Index])
        {
            Result.Add(Input[Index]);
        }
    }
    return Result;
}

static TArray<FVector> SimplifyFeaturePolyline(
    const TArray<FVector>& Input,
    double Tolerance,
    bool bClosed
)
{
    if (!bClosed)
    {
        return SimplifyOpenPolyline(Input, Tolerance);
    }
    if (Input.Num() <= 4 || Tolerance <= 0.0)
    {
        return Input;
    }

    int32 OppositeIndex = 1;
    double MaximumDistanceSquared = 0.0;
    for (int32 Index = 1; Index < Input.Num(); ++Index)
    {
        const double DistanceSquared = FVector::DistSquared(Input[0], Input[Index]);
        if (DistanceSquared > MaximumDistanceSquared)
        {
            MaximumDistanceSquared = DistanceSquared;
            OppositeIndex = Index;
        }
    }

    TArray<FVector> FirstArc;
    FirstArc.Append(Input.GetData(), OppositeIndex + 1);
    TArray<FVector> SecondArc;
    for (int32 Index = OppositeIndex; Index < Input.Num(); ++Index)
    {
        SecondArc.Add(Input[Index]);
    }
    SecondArc.Add(Input[0]);

    FirstArc = SimplifyOpenPolyline(FirstArc, Tolerance);
    SecondArc = SimplifyOpenPolyline(SecondArc, Tolerance);
    TArray<FVector> Result = FirstArc;
    for (int32 Index = 1; Index + 1 < SecondArc.Num(); ++Index)
    {
        Result.Add(SecondArc[Index]);
    }
    return Result.Num() >= 3 ? Result : Input;
}

static void AddBroadMeanders(
    TArray<FVector>& Points,
    double CellSize,
    double Strength,
    double LowlandFraction,
    double DischargeFraction,
    double ValleyFreedom,
    int32 Seed,
    const FBox& Bounds
)
{
    if (Points.Num() < 4 || Strength <= 0.0)
    {
        return;
    }
    TArray<double> Lengths;
    Lengths.SetNum(Points.Num());
    Lengths[0] = 0.0;
    for (int32 Index = 1; Index < Points.Num(); ++Index)
    {
        Lengths[Index] = Lengths[Index - 1] + FVector2D::Distance(
            FVector2D(Points[Index - 1]), FVector2D(Points[Index])
        );
    }
    const double TotalLength = Lengths.Last();
    if (TotalLength < CellSize * 4.0)
    {
        return;
    }

    const double Lowland = FMath::Pow(
        FMath::Clamp(LowlandFraction, 0.0, 1.0), 1.35
    );
    const double Discharge = FMath::Clamp(DischargeFraction, 0.0, 1.0);
    const double Freedom = FMath::Clamp(ValleyFreedom, 0.15, 1.0);
    if (Lowland < 0.04 || Freedom < 0.16)
    {
        return;
    }

    FRandomStream Random(Seed);
    const double Amplitude = FMath::Min(
        TotalLength * 0.075,
        CellSize * FMath::Lerp(2.2, 5.0, Discharge)
    ) * Strength * Lowland * FMath::Lerp(0.55, 1.0, Discharge) * Freedom;
    const double Wavelength = Random.FRandRange(0.88, 1.16)
        * FMath::Lerp(10.0, 34.0, Discharge) * CellSize;
    const double PhaseA = Random.FRandRange(-PI, PI);
    const double PhaseB = Random.FRandRange(-PI, PI);
    for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
    {
        const double Distance = Lengths[Index];
        const double Alpha = Distance / TotalLength;
        const FVector2D Tangent = FVector2D(
            Points[Index + 1].X - Points[Index - 1].X,
            Points[Index + 1].Y - Points[Index - 1].Y
        ).GetSafeNormal();
        const FVector2D Normal = Rotate90(Tangent);
        const double Fade = FMath::Sin(PI * Alpha);
        const double Wave =
            0.70 * FMath::Sin(2.0 * PI * Distance / Wavelength + PhaseA) +
            0.30 * FMath::Sin(
                2.0 * PI * Distance / (Wavelength * 2.43) + PhaseB
            );
        const FVector2D Offset = Normal * Amplitude * Fade * Wave;
        Points[Index].X = FMath::Clamp(
            Points[Index].X + Offset.X,
            Bounds.Min.X + CellSize,
            Bounds.Max.X - CellSize
        );
        Points[Index].Y = FMath::Clamp(
            Points[Index].Y + Offset.Y,
            Bounds.Min.Y + CellSize,
            Bounds.Max.Y - CellSize
        );
    }
}

struct FRiverReach
{
    TArray<FVector> Points;
    double Width = 500.0;
    double Depth = 250.0;
    double ValleyHalfWidth = 15000.0;
    double ValleyDepth = 1500.0;
    double CrossSectionExponent = 1.0;
    double ChannelSteepness = 2.2;
    double DrainageArea = 0.0;
    int32 StartLakeIndex = INDEX_NONE;
    int32 EndLakeIndex = INDEX_NONE;
    bool bIsCanyon = false;
    FBox2D Bounds = FBox2D(ForceInit);
};

struct FLakeBasin
{
    TArray<FVector> Shoreline;
    double ShorelineHeight = 0.0;
    double SurfaceHeight = 0.0;
    double MaximumDepth = 500.0;
    double ModifierBedDepth = 3000.0;
    double BankBlendWidth = 24000.0;
    double DepthRampWidth = 7500.0;
    FBox2D Bounds = FBox2D(ForceInit);
};

struct FPriorityCell
{
    double Height = 0.0;
    int32 Cell = INDEX_NONE;
    bool operator>(const FPriorityCell& Other) const { return Height > Other.Height; }
};

} // namespace UE::Avenor::Strip

struct FAvenorStripData
{
    FBox Bounds = FBox(ForceInit);
    int32 Columns = 0;
    int32 Rows = 0;
    double CellSize = 10000.0;
    TArray<double> Height;
    TArray<double> Resistance;
    TArray<double> MountainMask;
    TArray<double> HillMask;
    TArray<double> DesertMask;
    TArray<double> PlainsMask;
    TArray<double> FilledHeight;
    TArray<double> Accumulation;
    TArray<double> Slope;
    TArray<double> MacroTemperature;
    TArray<double> MacroMoisture;
    TArray<double> Temperature;
    TArray<double> Moisture;
    TArray<double> Runoff;
    TArray<uint8> Biome;
    TArray<int32> ReceiverA;
    TArray<int32> ReceiverB;
    TArray<double> ReceiverWeightA;
    TArray<int32> FillParent;
    TArray<int32> LakeIndex;
    TArray<UE::Avenor::Strip::FRiverReach> Rivers;
    TArray<UE::Avenor::Strip::FLakeBasin> Lakes;
    TArray<FVector> OceanBoundary;
    int32 RequestedMountainRanges = 0;
    int32 PlacedMountainRanges = 0;
    int32 AuthoritativeRiverCells = 0;
    int32 RiverSeedCells = 0;
    int32 RiverContinuationCells = 0;
    int32 RejectedShortRiverSystems = 0;
    int32 RiverTerminusLakeCandidates = 0;
    int32 AcceptedRiverTerminusLakes = 0;
    int32 AcceptedOptionalLakes = 0;

    int32 Index(int32 X, int32 Y) const { return Y * Columns + X; }
    bool IsValid(int32 X, int32 Y) const { return X >= 0 && X < Columns && Y >= 0 && Y < Rows; }
    bool IsBoundary(int32 X, int32 Y) const { return X == 0 || Y == 0 || X == Columns - 1 || Y == Rows - 1; }

    FVector2D CellPosition(int32 Cell) const
    {
        const int32 X = Cell % Columns;
        const int32 Y = Cell / Columns;
        return FVector2D(
            Bounds.Min.X + (static_cast<double>(X) + 0.5) * CellSize,
            Bounds.Min.Y + (static_cast<double>(Y) + 0.5) * CellSize
        );
    }

    double CellAreaSquareKilometres() const { return CellSize * CellSize / 10000000000.0; }

    double SampleGrid(const TArray<double>& Values, const FVector2D& Position) const
    {
        if (Values.Num() != Columns * Rows || Columns < 2 || Rows < 2)
        {
            return 0.0;
        }
        const double GX = (Position.X - Bounds.Min.X) / CellSize - 0.5;
        const double GY = (Position.Y - Bounds.Min.Y) / CellSize - 0.5;
        const int32 X0 = FMath::Clamp(FMath::FloorToInt(GX), 0, Columns - 1);
        const int32 Y0 = FMath::Clamp(FMath::FloorToInt(GY), 0, Rows - 1);
        const int32 X1 = FMath::Min(X0 + 1, Columns - 1);
        const int32 Y1 = FMath::Min(Y0 + 1, Rows - 1);
        const double AX = FMath::Clamp(GX - X0, 0.0, 1.0);
        const double AY = FMath::Clamp(GY - Y0, 0.0, 1.0);
        return FMath::Lerp(
            FMath::Lerp(Values[Index(X0, Y0)], Values[Index(X1, Y0)], AX),
            FMath::Lerp(Values[Index(X0, Y1)], Values[Index(X1, Y1)], AX),
            AY
        );
    }

    double SampleHeight(const FVector2D& Position) const;
    float SampleChannel(FName Channel, const FVector2D& Position) const;
};

namespace UE::Avenor::Strip::BakedData
{
static constexpr uint32 ChunkMagic = 0x41564431;
static constexpr int32 ChunkPayloadVersion = 5;
static constexpr int32 GeneratorAlgorithmVersion = 15;

// NOTE: file body continues unchanged from v14 except for the river routing/carving
// changes below. This marker is intentionally not used at runtime.

