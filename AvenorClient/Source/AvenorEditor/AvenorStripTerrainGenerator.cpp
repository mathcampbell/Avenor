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
    // A dendritic network's short headwater tributaries vastly outnumber
    // its main-stem reaches, so a 4-cell (400m) cutoff meant most of the
    // visible network got literally zero meander, not just a small one.
    if (TotalLength < CellSize * 2.0)
    {
        return;
    }

    const double Lowland = FMath::Clamp(LowlandFraction, 0.0, 1.0);
    const double Discharge = FMath::Clamp(DischargeFraction, 0.0, 1.0);
    // Floor kept above the gate threshold below: a clamped-to-floor Freedom
    // used to land at exactly the same value the gate rejected, so any
    // valley-constrained reach (common wherever resistance/mountain masks
    // are non-trivial, which is most of this terrain) silently got zero
    // meander instead of the reduced-but-nonzero wiggle the floor implies.
    const double Freedom = FMath::Clamp(ValleyFreedom, 0.20, 1.0);
    if (Lowland < 0.04 || Freedom < 0.16)
    {
        return;
    }

    FRandomStream Random(Seed);
    // This used to multiply five independent ~[0,1] factors together (the
    // length cap, Strength, Lowland raised to a 1.35 exponent, a Discharge
    // lerp bottoming out at 0.55, Freedom) - each looked reasonable in
    // isolation, but compounded they routinely reduced a short, moderately
    // steep headwater tributary's amplitude to 1-3m: "some" meander on
    // paper, invisible in practice and well under any simplification
    // tolerance, which is exactly why a dendritic network's many short
    // branches (the majority of any river system by count) were rendering
    // as straight lines even after the simplification-tolerance fix.
    // Loosened the length cap, dropped the extra Lowland exponent, and
    // raised the Discharge floor so a typical short stream keeps a real,
    // visible wiggle instead of vanishing under compounded multipliers.
    const double RawAmplitude = FMath::Min(
        TotalLength * 0.16,
        CellSize * FMath::Lerp(2.4, 5.5, Discharge)
    ) * Strength * Lowland * FMath::Lerp(0.75, 1.0, Discharge) * Freedom;
    const double Wavelength = Random.FRandRange(0.88, 1.16)
        * FMath::Lerp(10.0, 34.0, Discharge) * CellSize;
    // The active channel can form a tight meander neck, but it must never
    // curl into a synthetic full loop. An abandoned loop is an oxbow feature,
    // not a self-crossing Water spline.
    const double Amplitude = FMath::Min(
        RawAmplitude, Wavelength * 0.18
    );
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
    // Near-zero-epsilon depression fill, used only for lake/basin detection.
    // FilledHeight above is built with DrainageEpsilon for flow routing, and
    // that epsilon accumulates additively along any long, gently-sloped
    // stretch (an ordinary lowland valley, a floodplain) even where the
    // terrain has no real depression - which can make a ordinary valley many
    // cells long read as a "filled basin" and drag a lake's detected surface
    // height down to whatever the far end of that false blob happens to be.
    // This field is not part of the baked chunk format - transient, used
    // only during generation.
    TArray<double> DepressionFillHeight;
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
    // Single steepest-descent receiver/accumulation, transient and rebuilt
    // each BuildContinuousFlow call. Not part of the baked chunk format -
    // used only during generation to give river/lake network extraction a
    // single, stable channel topology instead of reducing the MFD split
    // above per-cell.
    TArray<int32> ReceiverD8;
    TArray<double> AccumulationD8;
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
static constexpr int32 GeneratorAlgorithmVersion = 20;

static void ExtractFloatChunk(
    const TArray<double>& Source,
    const FAvenorStripData& Data,
    int32 StartX,
    int32 StartY,
    int32 CountX,
    int32 CountY,
    TArray<float>& Output
)
{
    Output.SetNumUninitialized(CountX * CountY);
    const bool bHasSource = Source.Num() == Data.Columns * Data.Rows;
    for (int32 LocalY = 0; LocalY < CountY; ++LocalY)
    {
        for (int32 LocalX = 0; LocalX < CountX; ++LocalX)
        {
            const int32 LocalIndex = LocalY * CountX + LocalX;
            const int32 SourceIndex = Data.Index(StartX + LocalX, StartY + LocalY);
            Output[LocalIndex] = bHasSource ? static_cast<float>(Source[SourceIndex]) : 0.0f;
        }
    }
}

static void ExtractIntChunk(
    const TArray<int32>& Source,
    const FAvenorStripData& Data,
    int32 StartX,
    int32 StartY,
    int32 CountX,
    int32 CountY,
    TArray<int32>& Output
)
{
    Output.SetNumUninitialized(CountX * CountY);
    const bool bHasSource = Source.Num() == Data.Columns * Data.Rows;
    for (int32 LocalY = 0; LocalY < CountY; ++LocalY)
    {
        for (int32 LocalX = 0; LocalX < CountX; ++LocalX)
        {
            const int32 LocalIndex = LocalY * CountX + LocalX;
            const int32 SourceIndex = Data.Index(StartX + LocalX, StartY + LocalY);
            Output[LocalIndex] = bHasSource ? Source[SourceIndex] : INDEX_NONE;
        }
    }
}

static bool CompressChunk(
    const FAvenorStripData& Data,
    int32 StartX,
    int32 StartY,
    int32 CountX,
    int32 CountY,
    FAvenorTerrainDataChunk& Chunk
)
{
    Chunk.StartCell = FIntPoint(StartX, StartY);
    Chunk.CellCount = FIntPoint(CountX, CountY);
    Chunk.CompressedPayload.Reset();

    FArchiveSaveCompressedProxy Archive(
        Chunk.CompressedPayload,
        NAME_Zlib,
        ECompressionFlags::COMPRESS_BiasMemory
    );
    uint32 Magic = ChunkMagic;
    int32 Version = ChunkPayloadVersion;
    Archive << Magic;
    Archive << Version;

    TArray<float> FloatValues;
    TArray<int32> IntValues;
    auto WriteFloat = [&](const TArray<double>& Source)
    {
        ExtractFloatChunk(Source, Data, StartX, StartY, CountX, CountY, FloatValues);
        Archive << FloatValues;
    };
    auto WriteInt = [&](const TArray<int32>& Source)
    {
        ExtractIntChunk(Source, Data, StartX, StartY, CountX, CountY, IntValues);
        Archive << IntValues;
    };

    WriteFloat(Data.Height);
    WriteFloat(Data.Resistance);
    WriteFloat(Data.MountainMask);
    WriteFloat(Data.HillMask);
    WriteFloat(Data.DesertMask);
    WriteFloat(Data.PlainsMask);
    WriteFloat(Data.FilledHeight);
    WriteFloat(Data.Accumulation);
    WriteFloat(Data.Slope);
    WriteInt(Data.ReceiverA);
    WriteInt(Data.ReceiverB);
    WriteFloat(Data.ReceiverWeightA);
    WriteInt(Data.FillParent);
    WriteInt(Data.LakeIndex);

    Chunk.UncompressedBytes = static_cast<int64>(CountX) * CountY
        * 14 * sizeof(uint32);
    Archive.Flush();
    return !Archive.GetError() && !Chunk.CompressedPayload.IsEmpty();
}

static bool CopyFloatChunkToGrid(
    FArchive& Archive,
    TArray<double>& Destination,
    const FAvenorTerrainDataChunk& Chunk,
    const FAvenorStripData& Data
)
{
    TArray<float> Values;
    Archive << Values;
    const int32 Expected = Chunk.CellCount.X * Chunk.CellCount.Y;
    if (Values.Num() != Expected)
    {
        return false;
    }
    for (int32 LocalY = 0; LocalY < Chunk.CellCount.Y; ++LocalY)
    {
        for (int32 LocalX = 0; LocalX < Chunk.CellCount.X; ++LocalX)
        {
            Destination[Data.Index(
                Chunk.StartCell.X + LocalX,
                Chunk.StartCell.Y + LocalY
            )] = Values[LocalY * Chunk.CellCount.X + LocalX];
        }
    }
    return true;
}

static bool CopyIntChunkToGrid(
    FArchive& Archive,
    TArray<int32>& Destination,
    const FAvenorTerrainDataChunk& Chunk,
    const FAvenorStripData& Data
)
{
    TArray<int32> Values;
    Archive << Values;
    const int32 Expected = Chunk.CellCount.X * Chunk.CellCount.Y;
    if (Values.Num() != Expected)
    {
        return false;
    }
    for (int32 LocalY = 0; LocalY < Chunk.CellCount.Y; ++LocalY)
    {
        for (int32 LocalX = 0; LocalX < Chunk.CellCount.X; ++LocalX)
        {
            Destination[Data.Index(
                Chunk.StartCell.X + LocalX,
                Chunk.StartCell.Y + LocalY
            )] = Values[LocalY * Chunk.CellCount.X + LocalX];
        }
    }
    return true;
}

static bool DecompressChunk(
    const FAvenorTerrainDataChunk& Chunk,
    FAvenorStripData& Data
)
{
    if (Chunk.CompressedPayload.IsEmpty()
        || Chunk.StartCell.X < 0
        || Chunk.StartCell.Y < 0
        || Chunk.CellCount.X <= 0
        || Chunk.CellCount.Y <= 0
        || Chunk.StartCell.X + Chunk.CellCount.X > Data.Columns
        || Chunk.StartCell.Y + Chunk.CellCount.Y > Data.Rows)
    {
        return false;
    }

    FArchiveLoadCompressedProxy Archive(Chunk.CompressedPayload, NAME_Zlib);
    uint32 Magic = 0;
    int32 Version = 0;
    Archive << Magic;
    Archive << Version;
    if (Archive.GetError() || Magic != ChunkMagic || Version != ChunkPayloadVersion)
    {
        return false;
    }

    return CopyFloatChunkToGrid(Archive, Data.Height, Chunk, Data)
        && CopyFloatChunkToGrid(Archive, Data.Resistance, Chunk, Data)
        && CopyFloatChunkToGrid(Archive, Data.MountainMask, Chunk, Data)
        && CopyFloatChunkToGrid(Archive, Data.HillMask, Chunk, Data)
        && CopyFloatChunkToGrid(Archive, Data.DesertMask, Chunk, Data)
        && CopyFloatChunkToGrid(Archive, Data.PlainsMask, Chunk, Data)
        && CopyFloatChunkToGrid(Archive, Data.FilledHeight, Chunk, Data)
        && CopyFloatChunkToGrid(Archive, Data.Accumulation, Chunk, Data)
        && CopyFloatChunkToGrid(Archive, Data.Slope, Chunk, Data)
        && CopyIntChunkToGrid(Archive, Data.ReceiverA, Chunk, Data)
        && CopyIntChunkToGrid(Archive, Data.ReceiverB, Chunk, Data)
        && CopyFloatChunkToGrid(Archive, Data.ReceiverWeightA, Chunk, Data)
        && CopyIntChunkToGrid(Archive, Data.FillParent, Chunk, Data)
        && CopyIntChunkToGrid(Archive, Data.LakeIndex, Chunk, Data)
        && !Archive.GetError();
}

#if WITH_EDITOR
static UTexture2D* CreateOrUpdateClimateTexture(
    const FString& PackageName,
    const FString& AssetName,
    int32 Width,
    int32 Height,
    const TArray<FColor>& Pixels,
    TextureFilter Filter
)
{
    if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
    {
        return nullptr;
    }
    const FString ObjectPath = FString::Printf(
        TEXT("%s.%s"), *PackageName, *AssetName
    );
    UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *ObjectPath);
    bool bCreated = false;
    if (!Texture)
    {
        UPackage* Package = CreatePackage(*PackageName);
        Texture = NewObject<UTexture2D>(
            Package,
            *AssetName,
            RF_Public | RF_Standalone | RF_Transactional
        );
        bCreated = Texture != nullptr;
    }
    if (!Texture)
    {
        return nullptr;
    }

    Texture->Modify();
    Texture->Source.Init(
        Width,
        Height,
        1,
        1,
        TSF_BGRA8,
        reinterpret_cast<const uint8*>(Pixels.GetData())
    );
    Texture->SRGB = false;
    Texture->CompressionSettings = TC_VectorDisplacementmap;
    Texture->MipGenSettings = TMGS_NoMipmaps;
    Texture->Filter = Filter;
    Texture->NeverStream = false;
    Texture->UpdateResource();
    Texture->MarkPackageDirty();
    if (bCreated)
    {
        FAssetRegistryModule::AssetCreated(Texture);
    }
    return Texture;
}

static uint8 UnitToByte(double Value)
{
    return static_cast<uint8>(FMath::RoundToInt(
        FMath::Clamp(Value, 0.0, 1.0) * 255.0
    ));
}

static EAvenorBiomeClass ClassifyBaseBiome(
    double Temperature,
    double Moisture
)
{
    const bool bMoist = Moisture >= 0.5;
    if (Temperature < 0.25)
        return bMoist ? EAvenorBiomeClass::ColdMoist
                      : EAvenorBiomeClass::ColdDry;
    if (Temperature < 0.5)
        return bMoist ? EAvenorBiomeClass::TemperateMoist
                      : EAvenorBiomeClass::TemperateDry;
    if (Temperature < 0.75)
        return bMoist ? EAvenorBiomeClass::WarmMoist
                      : EAvenorBiomeClass::WarmDry;
    return bMoist ? EAvenorBiomeClass::HotWet
                  : EAvenorBiomeClass::HotDry;
}

struct FClimateSurfaceMasks
{
    TArray<double> Riverbed;
    TArray<double> Riverbank;
    TArray<double> Lakebed;
    TArray<double> Lakeshore;
};

static FIntRect BoundsToCellRect(
    const FAvenorStripData& Data,
    const FBox2D& Bounds
)
{
    const int32 MinX = FMath::Clamp(
        FMath::FloorToInt((Bounds.Min.X - Data.Bounds.Min.X) / Data.CellSize),
        0, Data.Columns - 1
    );
    const int32 MinY = FMath::Clamp(
        FMath::FloorToInt((Bounds.Min.Y - Data.Bounds.Min.Y) / Data.CellSize),
        0, Data.Rows - 1
    );
    const int32 MaxX = FMath::Clamp(
        FMath::CeilToInt((Bounds.Max.X - Data.Bounds.Min.X) / Data.CellSize),
        0, Data.Columns - 1
    );
    const int32 MaxY = FMath::Clamp(
        FMath::CeilToInt((Bounds.Max.Y - Data.Bounds.Min.Y) / Data.CellSize),
        0, Data.Rows - 1
    );
    return FIntRect(MinX, MinY, MaxX + 1, MaxY + 1);
}

static FClimateSurfaceMasks BuildClimateSurfaceMasks(
    const FAvenorStripData& Data,
    double RiverBankWidth
)
{
    const int32 CellCount = Data.Columns * Data.Rows;
    FClimateSurfaceMasks Masks;
    Masks.Riverbed.Init(0.0, CellCount);
    Masks.Riverbank.Init(0.0, CellCount);
    Masks.Lakebed.Init(0.0, CellCount);
    Masks.Lakeshore.Init(0.0, CellCount);

    const double EffectiveRiverBankWidth = FMath::Max(
        Data.CellSize * 0.35, RiverBankWidth
    );
    const double MaximumRiverBankWidth = EffectiveRiverBankWidth * 1.75;
    for (const FRiverReach& River : Data.Rivers)
    {
        if (River.Points.Num() < 2)
        {
            continue;
        }
        FBox2D Bounds(ForceInit);
        for (const FVector& Point : River.Points)
        {
            Bounds += FVector2D(Point);
        }
        const double HalfWidth = FMath::Max(
            Data.CellSize * 0.28,
            FMath::Max(100.0, River.Width * 0.5)
        );
        const FIntRect Cells = BoundsToCellRect(
            Data, Bounds.ExpandBy(HalfWidth + MaximumRiverBankWidth)
        );
        for (int32 Y = Cells.Min.Y; Y < Cells.Max.Y; ++Y)
        {
            for (int32 X = Cells.Min.X; X < Cells.Max.X; ++X)
            {
                const int32 Cell = Data.Index(X, Y);
                const FVector2D Position = Data.CellPosition(Cell);
                double Distance = TNumericLimits<double>::Max();
                for (int32 PointIndex = 0;
                     PointIndex + 1 < River.Points.Num(); ++PointIndex)
                {
                    Distance = FMath::Min(
                        Distance,
                        SegmentDistance(
                            Position,
                            FVector2D(River.Points[PointIndex]),
                            FVector2D(River.Points[PointIndex + 1])
                        )
                    );
                }
                if (Distance <= HalfWidth)
                {
                    Masks.Riverbed[Cell] = FMath::Max(
                        Masks.Riverbed[Cell],
                        Smooth01(1.0 - Distance / HalfWidth)
                    );
                    continue;
                }
                const double SlopeAlpha = Smooth01(FMath::Clamp(
                    Data.Slope.IsValidIndex(Cell)
                        ? Data.Slope[Cell] / 0.22 : 0.0,
                    0.0,
                    1.0
                ));
                const double LocalBankWidth = FMath::Max(
                    Data.CellSize * 0.30,
                    EffectiveRiverBankWidth
                        * FMath::Lerp(1.65, 0.32, SlopeAlpha)
                );
                if (Distance <= HalfWidth + LocalBankWidth)
                {
                    Masks.Riverbank[Cell] = FMath::Max(
                        Masks.Riverbank[Cell],
                        Smooth01(
                            1.0 - (Distance - HalfWidth) / LocalBankWidth
                        )
                    );
                }
            }
        }
    }

    for (const FLakeBasin& Lake : Data.Lakes)
    {
        if (Lake.Shoreline.Num() < 3)
        {
            continue;
        }
        FBox2D Bounds(ForceInit);
        for (const FVector& Point : Lake.Shoreline)
        {
            Bounds += FVector2D(Point);
        }
        const double ShoreWidth = FMath::Max(
            Data.CellSize * 0.35, Lake.BankBlendWidth
        );
        const double MaximumShoreWidth = ShoreWidth * 1.8;
        const double BedRamp = FMath::Max(
            Data.CellSize * 0.35, Lake.DepthRampWidth
        );
        const FIntRect Cells = BoundsToCellRect(
            Data, Bounds.ExpandBy(MaximumShoreWidth)
        );
        for (int32 Y = Cells.Min.Y; Y < Cells.Max.Y; ++Y)
        {
            for (int32 X = Cells.Min.X; X < Cells.Max.X; ++X)
            {
                const int32 Cell = Data.Index(X, Y);
                double EdgeDistance = 0.0;
                const bool bInside = IsInsidePolygon(
                    Data.CellPosition(Cell), Lake.Shoreline, &EdgeDistance
                );
                if (bInside)
                {
                    Masks.Lakebed[Cell] = FMath::Max(
                        Masks.Lakebed[Cell], Smooth01(EdgeDistance / BedRamp)
                    );
                }
                const double SlopeAlpha = Smooth01(FMath::Clamp(
                    Data.Slope.IsValidIndex(Cell)
                        ? Data.Slope[Cell] / 0.22 : 0.0,
                    0.0,
                    1.0
                ));
                const double LocalShoreWidth = FMath::Max(
                    Data.CellSize * 0.30,
                    ShoreWidth * FMath::Lerp(1.75, 0.28, SlopeAlpha)
                );
                if (EdgeDistance <= LocalShoreWidth)
                {
                    Masks.Lakeshore[Cell] = FMath::Max(
                        Masks.Lakeshore[Cell],
                        Smooth01(1.0 - EdgeDistance / LocalShoreWidth)
                    );
                }
            }
        }
    }
    return Masks;
}

static bool BuildClimateTextureTiles(
    UAvenorTerrainData& Asset,
    const FAvenorStripData& Data,
    const FString& OwnerName,
    EAvenorStripLongAxis LongAxis,
    double TileLength,
    double RiverBankWidth
)
{
    Asset.ClimateTiles.Reset();
    Asset.WorldClimateMaps = FAvenorWorldClimateMapReference();
    const int32 CellCount = Data.Columns * Data.Rows;
    if (Data.MacroTemperature.Num() != CellCount
        || Data.MacroMoisture.Num() != CellCount
        || Data.Temperature.Num() != CellCount
        || Data.Moisture.Num() != CellCount
        || Data.Biome.Num() != CellCount
        || Data.Height.Num() != CellCount
        || Data.Slope.Num() != CellCount
        || Data.Accumulation.Num() != CellCount
        || Data.MountainMask.Num() != CellCount
        || Data.DesertMask.Num() != CellCount)
    {
        return false;
    }

    UEditorAssetSubsystem* AssetSubsystem = GEditor
        ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
    if (!AssetSubsystem)
    {
        return false;
    }

    const bool bLongX = LongAxis == EAvenorStripLongAxis::X;
    const int32 LongCellCount = bLongX ? Data.Columns : Data.Rows;
    const int32 CellsPerTile = FMath::Max(
        1, FMath::RoundToInt(FMath::Max(Data.CellSize, TileLength)
            / Data.CellSize)
    );
    const int32 TileCount = FMath::DivideAndRoundUp(
        LongCellCount, CellsPerTile
    );
    Asset.ClimateTiles.Reserve(TileCount);

    const FClimateSurfaceMasks SurfaceMasks = BuildClimateSurfaceMasks(
        Data, RiverBankWidth
    );
    double MinimumHeight = TNumericLimits<double>::Max();
    double MaximumHeight = -TNumericLimits<double>::Max();
    double MaximumFlow = 0.0;
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        MinimumHeight = FMath::Min(MinimumHeight, Data.Height[Cell]);
        MaximumHeight = FMath::Max(MaximumHeight, Data.Height[Cell]);
        MaximumFlow = FMath::Max(MaximumFlow, Data.Accumulation[Cell]);
    }
    const double HeightRange = FMath::Max(1.0, MaximumHeight - MinimumHeight);
    const double FlowLogRange = FMath::Max(1.0, FMath::Loge(1.0 + MaximumFlow));

    TArray<FColor> WorldBaseBiomePixels;
    TArray<FColor> WorldLocalBiomePixels;
    TArray<FColor> WorldClimatePixels;
    TArray<FColor> WorldTerrainPixels;
    TArray<FColor> WorldWaterPixels;
    WorldBaseBiomePixels.SetNumUninitialized(CellCount);
    WorldLocalBiomePixels.SetNumUninitialized(CellCount);
    WorldClimatePixels.SetNumUninitialized(CellCount);
    WorldTerrainPixels.SetNumUninitialized(CellCount);
    WorldWaterPixels.SetNumUninitialized(CellCount);
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        WorldBaseBiomePixels[Cell] = UAvenorTerrainData::GetBiomeColour(
            ClassifyBaseBiome(
                Data.MacroTemperature[Cell],
                Data.MacroMoisture[Cell]
            )
        );
        EAvenorBiomeClass LocalBiome =
            static_cast<EAvenorBiomeClass>(Data.Biome[Cell]);
        bool bHasLocalBiome = LocalBiome == EAvenorBiomeClass::SnowIce
            || LocalBiome == EAvenorBiomeClass::AlpineTundra
            || LocalBiome == EAvenorBiomeClass::Wetland
            || LocalBiome == EAvenorBiomeClass::Oasis;
        if (SurfaceMasks.Lakebed[Cell] >= 0.5)
        {
            LocalBiome = EAvenorBiomeClass::Lakebed;
            bHasLocalBiome = true;
        }
        else if (SurfaceMasks.Riverbed[Cell] >= 0.5)
        {
            LocalBiome = EAvenorBiomeClass::Riverbed;
            bHasLocalBiome = true;
        }
        else if (LocalBiome != EAvenorBiomeClass::SnowIce
            && LocalBiome != EAvenorBiomeClass::AlpineTundra
            && SurfaceMasks.Lakeshore[Cell] >= 0.25)
        {
            LocalBiome = EAvenorBiomeClass::Lakeshore;
            bHasLocalBiome = true;
        }
        else if (LocalBiome != EAvenorBiomeClass::SnowIce
            && LocalBiome != EAvenorBiomeClass::AlpineTundra
            && SurfaceMasks.Riverbank[Cell] >= 0.25)
        {
            LocalBiome = EAvenorBiomeClass::Riverbank;
            bHasLocalBiome = true;
        }
        WorldLocalBiomePixels[Cell] = bHasLocalBiome
            ? UAvenorTerrainData::GetBiomeColour(LocalBiome)
            : FColor(0, 0, 0, 0);
        WorldClimatePixels[Cell] = FColor(
            UnitToByte(Data.MacroTemperature[Cell]),
            UnitToByte(Data.MacroMoisture[Cell]),
            UnitToByte(Data.Temperature[Cell]),
            UnitToByte(Data.Moisture[Cell])
        );
        const double NormalizedSlope = FMath::Clamp(
            Data.Slope[Cell] / 0.35, 0.0, 1.0
        );
        const double ExposedRock = FMath::Clamp(
            FMath::Max(
                NormalizedSlope,
                Data.MountainMask[Cell] * 0.68
            ) * (1.0 - Data.Moisture[Cell] * 0.42)
                + Data.DesertMask[Cell] * 0.24,
            0.0, 1.0
        );
        WorldTerrainPixels[Cell] = FColor(
            UnitToByte((Data.Height[Cell] - MinimumHeight) / HeightRange),
            UnitToByte(NormalizedSlope),
            UnitToByte(FMath::Loge(1.0 + FMath::Max(
                0.0, Data.Accumulation[Cell]
            )) / FlowLogRange),
            UnitToByte(ExposedRock)
        );
        WorldWaterPixels[Cell] = FColor(
            UnitToByte(SurfaceMasks.Riverbed[Cell]),
            UnitToByte(SurfaceMasks.Riverbank[Cell]),
            UnitToByte(SurfaceMasks.Lakebed[Cell]),
            UnitToByte(SurfaceMasks.Lakeshore[Cell])
        );
    }

    for (int32 TileIndex = 0; TileIndex < TileCount; ++TileIndex)
    {
        const int32 LongStart = TileIndex * CellsPerTile;
        const int32 LongCount = FMath::Min(
            CellsPerTile, LongCellCount - LongStart
        );
        const int32 StartX = bLongX ? LongStart : 0;
        const int32 StartY = bLongX ? 0 : LongStart;
        const int32 CountX = bLongX ? LongCount : Data.Columns;
        const int32 CountY = bLongX ? Data.Rows : LongCount;
        TArray<FColor> BaseBiomePixels;
        TArray<FColor> LocalBiomePixels;
        TArray<FColor> ClimatePixels;
        TArray<FColor> TerrainPixels;
        TArray<FColor> WaterPixels;
        BaseBiomePixels.SetNumUninitialized(CountX * CountY);
        LocalBiomePixels.SetNumUninitialized(CountX * CountY);
        ClimatePixels.SetNumUninitialized(CountX * CountY);
        TerrainPixels.SetNumUninitialized(CountX * CountY);
        WaterPixels.SetNumUninitialized(CountX * CountY);
        for (int32 LocalY = 0; LocalY < CountY; ++LocalY)
        {
            for (int32 LocalX = 0; LocalX < CountX; ++LocalX)
            {
                const int32 SourceCell = Data.Index(
                    StartX + LocalX, StartY + LocalY
                );
                const int32 Pixel = LocalY * CountX + LocalX;
                BaseBiomePixels[Pixel] = WorldBaseBiomePixels[SourceCell];
                LocalBiomePixels[Pixel] = WorldLocalBiomePixels[SourceCell];
                ClimatePixels[Pixel] = WorldClimatePixels[SourceCell];
                TerrainPixels[Pixel] = WorldTerrainPixels[SourceCell];
                WaterPixels[Pixel] = WorldWaterPixels[SourceCell];
            }
        }

        const FString Suffix = FString::Printf(TEXT("%03d"), TileIndex);
        const FString BiomeName = FString::Printf(
            TEXT("T_AvenorBiome_%s_%s"), *OwnerName, *Suffix
        );
        const FString LocalBiomeName = FString::Printf(
            TEXT("T_AvenorLocalBiome_%s_%s"), *OwnerName, *Suffix
        );
        const FString ClimateName = FString::Printf(
            TEXT("T_AvenorClimate_%s_%s"), *OwnerName, *Suffix
        );
        const FString TerrainName = FString::Printf(
            TEXT("T_AvenorTerrainFilter_%s_%s"), *OwnerName, *Suffix
        );
        const FString WaterName = FString::Printf(
            TEXT("T_AvenorWaterSurface_%s_%s"), *OwnerName, *Suffix
        );
        UTexture2D* BaseBiomeTexture = CreateOrUpdateClimateTexture(
            FString::Printf(
                TEXT("/Game/Avenor/Generated/Climate/%s"), *BiomeName
            ),
            BiomeName,
            CountX,
            CountY,
            BaseBiomePixels,
            TF_Nearest
        );
        UTexture2D* LocalBiomeTexture = CreateOrUpdateClimateTexture(
            FString::Printf(
                TEXT("/Game/Avenor/Generated/Climate/%s"), *LocalBiomeName
            ),
            LocalBiomeName,
            CountX,
            CountY,
            LocalBiomePixels,
            TF_Nearest
        );
        UTexture2D* ClimateTexture = CreateOrUpdateClimateTexture(
            FString::Printf(
                TEXT("/Game/Avenor/Generated/Climate/%s"), *ClimateName
            ),
            ClimateName,
            CountX,
            CountY,
            ClimatePixels,
            TF_Bilinear
        );
        UTexture2D* TerrainTexture = CreateOrUpdateClimateTexture(
            FString::Printf(
                TEXT("/Game/Avenor/Generated/Climate/%s"), *TerrainName
            ),
            TerrainName,
            CountX,
            CountY,
            TerrainPixels,
            TF_Bilinear
        );
        UTexture2D* WaterTexture = CreateOrUpdateClimateTexture(
            FString::Printf(
                TEXT("/Game/Avenor/Generated/Climate/%s"), *WaterName
            ),
            WaterName,
            CountX,
            CountY,
            WaterPixels,
            TF_Bilinear
        );
        if (!BaseBiomeTexture || !LocalBiomeTexture || !ClimateTexture
            || !TerrainTexture || !WaterTexture
            || !AssetSubsystem->SaveLoadedAsset(BaseBiomeTexture, false)
            || !AssetSubsystem->SaveLoadedAsset(LocalBiomeTexture, false)
            || !AssetSubsystem->SaveLoadedAsset(ClimateTexture, false)
            || !AssetSubsystem->SaveLoadedAsset(TerrainTexture, false)
            || !AssetSubsystem->SaveLoadedAsset(WaterTexture, false))
        {
            Asset.ClimateTiles.Reset();
            Asset.WorldClimateMaps = FAvenorWorldClimateMapReference();
            return false;
        }

        FAvenorClimateTileReference& Tile =
            Asset.ClimateTiles.AddDefaulted_GetRef();
        Tile.TileIndex = TileIndex;
        Tile.StartCell = FIntPoint(StartX, StartY);
        Tile.CellCount = FIntPoint(CountX, CountY);
        const FVector2D TileMin(
            Data.Bounds.Min.X + StartX * Data.CellSize,
            Data.Bounds.Min.Y + StartY * Data.CellSize
        );
        const FVector2D TileMax(
            FMath::Min(
                Data.Bounds.Max.X,
                Data.Bounds.Min.X + (StartX + CountX) * Data.CellSize
            ),
            FMath::Min(
                Data.Bounds.Max.Y,
                Data.Bounds.Min.Y + (StartY + CountY) * Data.CellSize
            )
        );
        Tile.WorldBounds = FBox2D(TileMin, TileMax);
        Tile.BaseBiomeTexture = BaseBiomeTexture;
        Tile.LocalBiomeTexture = LocalBiomeTexture;
        Tile.ClimateFilterTexture = ClimateTexture;
        Tile.TerrainFilterTexture = TerrainTexture;
        Tile.WaterSurfaceTexture = WaterTexture;
    }

    const FString WorldBaseBiomeName = FString::Printf(
        TEXT("T_AvenorWorldBiome_%s"), *OwnerName
    );
    const FString WorldLocalBiomeName = FString::Printf(
        TEXT("T_AvenorWorldLocalBiome_%s"), *OwnerName
    );
    const FString WorldClimateName = FString::Printf(
        TEXT("T_AvenorWorldClimate_%s"), *OwnerName
    );
    const FString WorldTerrainName = FString::Printf(
        TEXT("T_AvenorWorldTerrainFilter_%s"), *OwnerName
    );
    const FString WorldWaterName = FString::Printf(
        TEXT("T_AvenorWorldWaterSurface_%s"), *OwnerName
    );
    const FString WorldTextureFolder = TEXT("/Game/Avenor/Generated/Climate/World");
    UTexture2D* WorldBaseBiomeTexture = CreateOrUpdateClimateTexture(
        FString::Printf(TEXT("%s/%s"), *WorldTextureFolder, *WorldBaseBiomeName),
        WorldBaseBiomeName,
        Data.Columns,
        Data.Rows,
        WorldBaseBiomePixels,
        TF_Nearest
    );
    UTexture2D* WorldLocalBiomeTexture = CreateOrUpdateClimateTexture(
        FString::Printf(TEXT("%s/%s"), *WorldTextureFolder, *WorldLocalBiomeName),
        WorldLocalBiomeName,
        Data.Columns,
        Data.Rows,
        WorldLocalBiomePixels,
        TF_Nearest
    );
    UTexture2D* WorldClimateTexture = CreateOrUpdateClimateTexture(
        FString::Printf(TEXT("%s/%s"), *WorldTextureFolder, *WorldClimateName),
        WorldClimateName,
        Data.Columns,
        Data.Rows,
        WorldClimatePixels,
        TF_Bilinear
    );
    UTexture2D* WorldTerrainTexture = CreateOrUpdateClimateTexture(
        FString::Printf(TEXT("%s/%s"), *WorldTextureFolder, *WorldTerrainName),
        WorldTerrainName,
        Data.Columns,
        Data.Rows,
        WorldTerrainPixels,
        TF_Bilinear
    );
    UTexture2D* WorldWaterTexture = CreateOrUpdateClimateTexture(
        FString::Printf(TEXT("%s/%s"), *WorldTextureFolder, *WorldWaterName),
        WorldWaterName,
        Data.Columns,
        Data.Rows,
        WorldWaterPixels,
        TF_Bilinear
    );
    if (!WorldBaseBiomeTexture || !WorldLocalBiomeTexture
        || !WorldClimateTexture || !WorldTerrainTexture || !WorldWaterTexture
        || !AssetSubsystem->SaveLoadedAsset(WorldBaseBiomeTexture, false)
        || !AssetSubsystem->SaveLoadedAsset(WorldLocalBiomeTexture, false)
        || !AssetSubsystem->SaveLoadedAsset(WorldClimateTexture, false)
        || !AssetSubsystem->SaveLoadedAsset(WorldTerrainTexture, false)
        || !AssetSubsystem->SaveLoadedAsset(WorldWaterTexture, false))
    {
        Asset.ClimateTiles.Reset();
        Asset.WorldClimateMaps = FAvenorWorldClimateMapReference();
        return false;
    }

    Asset.WorldClimateMaps.WorldBounds = FBox2D(
        FVector2D(Data.Bounds.Min.X, Data.Bounds.Min.Y),
        FVector2D(Data.Bounds.Max.X, Data.Bounds.Max.Y));
    Asset.WorldClimateMaps.CellCount = FIntPoint(Data.Columns, Data.Rows);
    Asset.WorldClimateMaps.CellSize = Data.CellSize;
    Asset.WorldClimateMaps.BaseBiomeTexture = WorldBaseBiomeTexture;
    Asset.WorldClimateMaps.LocalBiomeTexture = WorldLocalBiomeTexture;
    Asset.WorldClimateMaps.ClimateFilterTexture = WorldClimateTexture;
    Asset.WorldClimateMaps.TerrainFilterTexture = WorldTerrainTexture;
    Asset.WorldClimateMaps.WaterSurfaceTexture = WorldWaterTexture;
    return true;
}
#endif
} // namespace UE::Avenor::Strip::BakedData

namespace UE::Avenor::Strip
{
static constexpr int32 NeighborX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
static constexpr int32 NeighborY[8] = {0, 1, 1, 1, 0, -1, -1, -1};

// Remove analysis-grid-scale chatter from terrain classes whose identity is
// broad form. Plains and rolling hills are low-passed, while true mountains
// and resistant desert relief retain their sharper structure. Erosion runs
// afterward and supplies coherent drainage detail.
static void SmoothLowReliefTerrain(FAvenorStripData& Data, int32 Iterations)
{
    if (Iterations <= 0 || Data.Height.Num() == 0)
    {
        return;
    }
    TArray<double> Smoothed = Data.Height;
    for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
    {
        Smoothed = Data.Height;
        for (int32 Y = 1; Y + 1 < Data.Rows; ++Y)
        {
            for (int32 X = 1; X + 1 < Data.Columns; ++X)
            {
                const int32 Cell = Data.Index(X, Y);
                const double Mountain = FMath::Clamp(
                    Data.MountainMask[Cell], 0.0, 1.0
                );
                const double Hill = FMath::Clamp(Data.HillMask[Cell], 0.0, 1.0);
                const double Plains = FMath::Clamp(
                    Data.PlainsMask[Cell], 0.0, 1.0
                );
                const double Desert = FMath::Clamp(
                    Data.DesertMask[Cell], 0.0, 1.0
                );
                const double ProtectedRelief = FMath::Clamp(
                    FMath::Max(Mountain, Desert * 0.62), 0.0, 1.0
                );
                const double Smoothing = FMath::Clamp(
                    (Plains * 0.48 + Hill * 0.30)
                        * (1.0 - ProtectedRelief * 0.90),
                    0.0, 0.52
                );
                if (Smoothing <= 0.01)
                {
                    continue;
                }
                double NeighborSum = 0.0;
                for (int32 Direction = 0; Direction < 8; ++Direction)
                {
                    NeighborSum += Data.Height[Data.Index(
                        X + NeighborX[Direction], Y + NeighborY[Direction]
                    )];
                }
                const double Target = NeighborSum / 8.0;
                const double Change = FMath::Clamp(
                    (Target - Data.Height[Cell]) * Smoothing,
                    -Data.CellSize * 0.20,
                    Data.CellSize * 0.20
                );
                Smoothed[Cell] = Data.Height[Cell] + Change;
            }
        }
        Data.Height = Smoothed;
    }
}

static void ApplyThermalErosion(
    FAvenorStripData& Data,
    int32 Iterations,
    double Strength,
    double TalusAngleDegrees,
    double ResistanceStrength
)
{
    if (Iterations <= 0 || Strength <= 0.0)
    {
        return;
    }
    const bool bUseResistance = ResistanceStrength > 0.0 &&
        Data.Resistance.Num() == Data.Height.Num();
    const double TalusDrop = FMath::Tan(FMath::DegreesToRadians(TalusAngleDegrees)) * Data.CellSize;
    TArray<double> Delta;
    Delta.SetNumZeroed(Data.Height.Num());
    for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
    {
        FMemory::Memzero(Delta.GetData(), Delta.Num() * sizeof(double));
        for (int32 Y = 1; Y + 1 < Data.Rows; ++Y)
        {
            for (int32 X = 1; X + 1 < Data.Columns; ++X)
            {
                const int32 Cell = Data.Index(X, Y);
                double TotalExcess = 0.0;
                double Excess[8] = {};
                for (int32 Direction = 0; Direction < 8; ++Direction)
                {
                    const int32 Neighbor = Data.Index(X + NeighborX[Direction], Y + NeighborY[Direction]);
                    const double Distance = (NeighborX[Direction] != 0 && NeighborY[Direction] != 0)
                        ? Data.CellSize * 1.4142135623730951 : Data.CellSize;
                    const double AllowedDrop = TalusDrop * Distance / Data.CellSize;
                    Excess[Direction] = FMath::Max(0.0, Data.Height[Cell] - Data.Height[Neighbor] - AllowedDrop);
                    TotalExcess += Excess[Direction];
                }
                if (TotalExcess <= 0.0)
                {
                    continue;
                }
                const double LocalResistance = bUseResistance
                    ? FMath::Clamp(Data.Resistance[Cell] * ResistanceStrength, 0.0, 0.95)
                    : 0.0;
                const double Transfer = FMath::Min(TotalExcess * Strength * 0.18, Data.CellSize * 0.25) *
                    (1.0 - LocalResistance);
                Delta[Cell] -= Transfer;
                for (int32 Direction = 0; Direction < 8; ++Direction)
                {
                    if (Excess[Direction] <= 0.0)
                    {
                        continue;
                    }
                    const int32 Neighbor = Data.Index(X + NeighborX[Direction], Y + NeighborY[Direction]);
                    Delta[Neighbor] += Transfer * Excess[Direction] / TotalExcess;
                }
            }
        }
        for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
        {
            Data.Height[Cell] += Delta[Cell];
        }
    }
}

static void PriorityFlood(FAvenorStripData& Data, double Epsilon)
{
    const int32 CellCount = Data.Height.Num();
    Data.FilledHeight = Data.Height;
    Data.FillParent.Init(INDEX_NONE, CellCount);
    TArray<bool> Visited;
    Visited.Init(false, CellCount);
    std::priority_queue<FPriorityCell, std::vector<FPriorityCell>, std::greater<FPriorityCell>> Queue;
    auto AddBoundary = [&](int32 X, int32 Y)
    {
        const int32 Cell = Data.Index(X, Y);
        if (!Visited[Cell])
        {
            Visited[Cell] = true;
            Queue.push({Data.FilledHeight[Cell], Cell});
        }
    };
    for (int32 X = 0; X < Data.Columns; ++X)
    {
        AddBoundary(X, 0);
        AddBoundary(X, Data.Rows - 1);
    }
    for (int32 Y = 1; Y + 1 < Data.Rows; ++Y)
    {
        AddBoundary(0, Y);
        AddBoundary(Data.Columns - 1, Y);
    }
    while (!Queue.empty())
    {
        const FPriorityCell Current = Queue.top();
        Queue.pop();
        const int32 X = Current.Cell % Data.Columns;
        const int32 Y = Current.Cell / Data.Columns;
        for (int32 Direction = 0; Direction < 8; ++Direction)
        {
            const int32 NX = X + NeighborX[Direction];
            const int32 NY = Y + NeighborY[Direction];
            if (!Data.IsValid(NX, NY))
            {
                continue;
            }
            const int32 Neighbor = Data.Index(NX, NY);
            if (Visited[Neighbor])
            {
                continue;
            }
            Visited[Neighbor] = true;
            Data.FillParent[Neighbor] = Current.Cell;
            Data.FilledHeight[Neighbor] = FMath::Max(Data.Height[Neighbor], Current.Height + FMath::Max(0.0001, Epsilon));
            Queue.push({Data.FilledHeight[Neighbor], Neighbor});
        }
    }
}

// Same priority-flood algorithm as above, but always at the smallest
// representable step (not DrainageEpsilon) and writing to a separate field.
// FilledHeight above needs a real epsilon to disambiguate flow direction
// across flats; that same epsilon then accumulates additively along any
// long, gently-sloped stretch, which can make an ordinary valley many cells
// long falsely read as a filled depression. This field is for measuring
// genuine depression depth only - lake/basin detection - not routing.
static void ComputeDepressionFillHeight(FAvenorStripData& Data)
{
    const int32 CellCount = Data.Height.Num();
    Data.DepressionFillHeight = Data.Height;
    TArray<bool> Visited;
    Visited.Init(false, CellCount);
    std::priority_queue<FPriorityCell, std::vector<FPriorityCell>, std::greater<FPriorityCell>> Queue;
    auto AddBoundary = [&](int32 X, int32 Y)
    {
        const int32 Cell = Data.Index(X, Y);
        if (!Visited[Cell])
        {
            Visited[Cell] = true;
            Queue.push({Data.DepressionFillHeight[Cell], Cell});
        }
    };
    for (int32 X = 0; X < Data.Columns; ++X)
    {
        AddBoundary(X, 0);
        AddBoundary(X, Data.Rows - 1);
    }
    for (int32 Y = 1; Y + 1 < Data.Rows; ++Y)
    {
        AddBoundary(0, Y);
        AddBoundary(Data.Columns - 1, Y);
    }
    while (!Queue.empty())
    {
        const FPriorityCell Current = Queue.top();
        Queue.pop();
        const int32 X = Current.Cell % Data.Columns;
        const int32 Y = Current.Cell / Data.Columns;
        for (int32 Direction = 0; Direction < 8; ++Direction)
        {
            const int32 NX = X + NeighborX[Direction];
            const int32 NY = Y + NeighborY[Direction];
            if (!Data.IsValid(NX, NY))
            {
                continue;
            }
            const int32 Neighbor = Data.Index(NX, NY);
            if (Visited[Neighbor])
            {
                continue;
            }
            Visited[Neighbor] = true;
            Data.DepressionFillHeight[Neighbor] = FMath::Max(Data.Height[Neighbor], Current.Height + 0.0001);
            Queue.push({Data.DepressionFillHeight[Neighbor], Neighbor});
        }
    }
}

static int32 SteepestReceiver(const FAvenorStripData& Data, int32 X, int32 Y)
{
    const int32 Cell = Data.Index(X, Y);
    double BestSlope = 0.0;
    int32 Best = INDEX_NONE;
    for (int32 Direction = 0; Direction < 8; ++Direction)
    {
        const int32 NX = X + NeighborX[Direction];
        const int32 NY = Y + NeighborY[Direction];
        if (!Data.IsValid(NX, NY))
        {
            continue;
        }
        const int32 Neighbor = Data.Index(NX, NY);
        const double Distance = (NeighborX[Direction] != 0 && NeighborY[Direction] != 0)
            ? Data.CellSize * 1.4142135623730951 : Data.CellSize;
        const double CandidateSlope = (Data.FilledHeight[Cell] - Data.FilledHeight[Neighbor]) / Distance;
        if (CandidateSlope > BestSlope)
        {
            BestSlope = CandidateSlope;
            Best = Neighbor;
        }
    }
    return Best != INDEX_NONE ? Best : Data.FillParent[Cell];
}

static void BuildContinuousFlow(FAvenorStripData& Data)
{
    const int32 CellCount = Data.Height.Num();
    Data.ReceiverA.Init(INDEX_NONE, CellCount);
    Data.ReceiverB.Init(INDEX_NONE, CellCount);
    Data.ReceiverWeightA.Init(1.0, CellCount);
    Data.ReceiverD8.Init(INDEX_NONE, CellCount);
    Data.Slope.Init(0.0, CellCount);
    Data.Accumulation.SetNumUninitialized(CellCount);
    const bool bHasRunoff = Data.Runoff.Num() == CellCount;
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        const double Runoff = bHasRunoff
            ? FMath::Clamp(Data.Runoff[Cell], 0.05, 2.0)
            : 1.0;
        Data.Accumulation[Cell] = Data.CellAreaSquareKilometres() * Runoff;
    }
    // AccumulationD8 starts from the same per-cell contribution as the MFD
    // Accumulation above and only diverges in how it is routed downstream.
    Data.AccumulationD8 = Data.Accumulation;

    for (int32 Y = 0; Y < Data.Rows; ++Y)
    {
        for (int32 X = 0; X < Data.Columns; ++X)
        {
            const int32 Cell = Data.Index(X, Y);
            if (Data.IsBoundary(X, Y))
            {
                continue;
            }
            const double DX = (Data.FilledHeight[Data.Index(X + 1, Y)] - Data.FilledHeight[Data.Index(X - 1, Y)]) / (2.0 * Data.CellSize);
            const double DY = (Data.FilledHeight[Data.Index(X, Y + 1)] - Data.FilledHeight[Data.Index(X, Y - 1)]) / (2.0 * Data.CellSize);
            Data.Slope[Cell] = FMath::Sqrt(DX * DX + DY * DY);
            double Angle = FMath::Atan2(-DY, -DX);
            if (Angle < 0.0)
            {
                Angle += 2.0 * PI;
            }
            const double Sector = Angle / (PI * 0.25);
            const int32 LowerDirection = FMath::FloorToInt(Sector) & 7;
            const int32 UpperDirection = (LowerDirection + 1) & 7;
            const double UpperWeight = Sector - static_cast<double>(FMath::FloorToInt(Sector));
            const int32 AX = X + NeighborX[LowerDirection];
            const int32 AY = Y + NeighborY[LowerDirection];
            const int32 BX = X + NeighborX[UpperDirection];
            const int32 BY = Y + NeighborY[UpperDirection];
            int32 A = Data.IsValid(AX, AY) ? Data.Index(AX, AY) : INDEX_NONE;
            int32 B = Data.IsValid(BX, BY) ? Data.Index(BX, BY) : INDEX_NONE;
            if (A != INDEX_NONE && Data.FilledHeight[A] >= Data.FilledHeight[Cell])
            {
                A = INDEX_NONE;
            }
            if (B != INDEX_NONE && Data.FilledHeight[B] >= Data.FilledHeight[Cell])
            {
                B = INDEX_NONE;
            }
            if (A == INDEX_NONE && B == INDEX_NONE)
            {
                A = SteepestReceiver(Data, X, Y);
                B = INDEX_NONE;
            }
            else if (A == INDEX_NONE)
            {
                A = B;
                B = INDEX_NONE;
            }
            Data.ReceiverA[Cell] = A;
            Data.ReceiverB[Cell] = B;
            Data.ReceiverWeightA[Cell] = B == INDEX_NONE ? 1.0 : 1.0 - UpperWeight;
            // Single steepest-descent receiver, independent of the MFD split
            // above. Network topology (channel extraction, confluences, lake
            // termini) reduces the MFD field back to one direction per cell
            // anyway; deriving that reduction independently as true D8 avoids
            // per-cell weight tie-breaks producing braided/unstable channels
            // and diluted accumulation at basin outlets.
            Data.ReceiverD8[Cell] = SteepestReceiver(Data, X, Y);
        }
    }

    TArray<int32> Order;
    Order.SetNumUninitialized(CellCount);
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        Order[Cell] = Cell;
    }
    Order.Sort([&](int32 A, int32 B) { return Data.FilledHeight[A] > Data.FilledHeight[B]; });
    for (int32 Cell : Order)
    {
        const int32 A = Data.ReceiverA[Cell];
        const int32 B = Data.ReceiverB[Cell];
        const double WeightA = Data.ReceiverWeightA[Cell];
        if (A != INDEX_NONE)
        {
            Data.Accumulation[A] += Data.Accumulation[Cell] * WeightA;
        }
        if (B != INDEX_NONE)
        {
            Data.Accumulation[B] += Data.Accumulation[Cell] * (1.0 - WeightA);
        }
        const int32 D8Receiver = Data.ReceiverD8[Cell];
        if (D8Receiver != INDEX_NONE)
        {
            Data.AccumulationD8[D8Receiver] += Data.AccumulationD8[Cell];
        }
    }
}

static double DrainageSourceMultiplier(
    const FAvenorStripData& Data,
    int32 Cell
)
{
    const double MountainSupport = Data.MountainMask.IsValidIndex(Cell)
        ? Data.MountainMask[Cell] : 0.0;
    const double HillSupport = Data.HillMask.IsValidIndex(Cell)
        ? Data.HillMask[Cell] : 0.0;
    const double ReliefSupport = FMath::Max(
        FMath::Max(MountainSupport, HillSupport * 0.75),
        Smooth01(FMath::Clamp(Data.Slope[Cell] / 0.055, 0.0, 1.0))
    );
    const double SourceMoisture = Data.MacroMoisture.IsValidIndex(Cell)
        ? Data.MacroMoisture[Cell]
        : (Data.Moisture.IsValidIndex(Cell) ? Data.Moisture[Cell] : 0.5);
    const double WetLowlandSupport = Smooth01(FMath::Clamp(
        (SourceMoisture - 0.62) / 0.26, 0.0, 1.0
    ));
    const double DesertInfluence = Data.DesertMask.IsValidIndex(Cell)
        ? Data.DesertMask[Cell] : 0.0;
    const double AridSourcePenalty = DesertInfluence * Smooth01(
        FMath::Clamp((0.52 - SourceMoisture) / 0.42, 0.0, 1.0)
    );
    const double SourceSupport = FMath::Max(
        ReliefSupport, WetLowlandSupport
    );
    return 1.0
        + (1.0 - SourceSupport) * 1.45
        + AridSourcePenalty * 1.65;
}

// Geomorphic erosion preserves palaeodrainage more readily than the modern
// river selector. Dry climates can therefore retain old canyons and valleys
// after permanent surface flow has disappeared.
static double ErosionDrainageSourceMultiplier(
    const FAvenorStripData& Data,
    int32 Cell
)
{
    const double Active = DrainageSourceMultiplier(Data, Cell);
    const double Desert = Data.DesertMask.IsValidIndex(Cell)
        ? FMath::Clamp(Data.DesertMask[Cell], 0.0, 1.0) : 0.0;
    return FMath::Lerp(
        FMath::Min(Active, 1.85),
        FMath::Min(Active, 1.20),
        Desert
    );
}

static void ApplyStreamPowerErosion(
    FAvenorStripData& Data,
    int32 Iterations,
    double Strength,
    double MountainStartArea,
    double LowlandStartArea,
    double Epsilon,
    double ResistanceStrength
)
{
    const bool bUseResistance = ResistanceStrength > 0.0 &&
        Data.Resistance.Num() == Data.Height.Num();
    for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
    {
        PriorityFlood(Data, Epsilon);
        BuildContinuousFlow(Data);
        TArray<double> Delta;
        Delta.Init(0.0, Data.Height.Num());
        for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
        {
            const double MountainFraction = FMath::Clamp(Data.Slope[Cell] / 0.18, 0.0, 1.0);
            const double StreamStartArea = FMath::Lerp(
                LowlandStartArea, MountainStartArea, MountainFraction
            ) * ErosionDrainageSourceMultiplier(Data, Cell);
            if (Data.Accumulation[Cell] < StreamStartArea || Data.ReceiverA[Cell] == INDEX_NONE)
            {
                continue;
            }
            const double AreaFactor = FMath::Pow(Data.Accumulation[Cell] / FMath::Max(0.01, StreamStartArea), 0.42);
            const double SlopeFactor = FMath::Pow(FMath::Max(0.00001, Data.Slope[Cell]), 0.72);
            const double LocalResistance = bUseResistance
                ? FMath::Clamp(Data.Resistance[Cell] * ResistanceStrength, 0.0, 0.92)
                : 0.0;
            // Suppress incision on cells that are not the dominant D8 flow
            // path. The MFD accumulation above disperses across two
            // receivers at every cell, so two adjacent cells can each
            // independently accumulate enough area to qualify for incision
            // even though only one is really "the" channel - carving two
            // parallel troughs a cell apart that get permanently entrenched,
            // instead of one channel. D8 accumulation concentrates all of a
            // path's flow onto a single thread, so a genuine secondary
            // candidate shows much less D8 accumulation than MFD
            // accumulation even where the two look similar under MFD alone.
            // Damping only (clamped to 1.0), not boosting the winner, to
            // keep this a minimal change to already-tuned incision strength.
            const double D8Dominance = Data.AccumulationD8.IsValidIndex(Cell)
                ? FMath::Clamp(
                    Data.AccumulationD8[Cell] / FMath::Max(0.01, Data.Accumulation[Cell]), 0.0, 1.0
                ) : 1.0;
            Delta[Cell] = FMath::Min(Data.CellSize * 2.5, Strength * 520.0 * AreaFactor * SlopeFactor) *
                (1.0 - LocalResistance) * D8Dominance;
        }
        for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
        {
            Data.Height[Cell] -= Delta[Cell];
        }
    }
    PriorityFlood(Data, Epsilon);
    BuildContinuousFlow(Data);
}

// Evolve structural terrain through its drainage network before final water
// features are selected. Weak terrain develops broad valleys; resistant arid
// uplands concentrate incision and survive as canyon rims and residual mesas.
static void EvolveTerrainFromDrainage(
    FAvenorStripData& Data,
    int32 Passes,
    double Strength,
    double MountainStartArea,
    double LowlandStartArea,
    double Epsilon,
    double ResistanceStrength
)
{
    if (Passes <= 0 || Strength <= 0.0 || Data.Height.Num() == 0)
    {
        return;
    }
    const bool bUseResistance = Data.Resistance.Num() == Data.Height.Num();
    const double EffectiveResistance = FMath::Clamp(ResistanceStrength, 0.0, 1.5);
    for (int32 Pass = 0; Pass < Passes; ++Pass)
    {
        PriorityFlood(Data, Epsilon);
        BuildContinuousFlow(Data);
        TArray<double> Delta;
        Delta.Init(0.0, Data.Height.Num());

        for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
        {
            const int32 X = Cell % Data.Columns;
            const int32 Y = Cell / Data.Columns;
            if (Data.IsBoundary(X, Y))
            {
                continue;
            }

            const double Mountain = Data.MountainMask.IsValidIndex(Cell)
                ? Data.MountainMask[Cell] : 0.0;
            const double Hill = Data.HillMask.IsValidIndex(Cell)
                ? Data.HillMask[Cell] : 0.0;
            const double Desert = Data.DesertMask.IsValidIndex(Cell)
                ? Data.DesertMask[Cell] : 0.0;
            const double Resistance = bUseResistance
                ? FMath::Clamp(Data.Resistance[Cell] * EffectiveResistance, 0.0, 0.94)
                : 0.0;
            const double ReliefClass = FMath::Clamp(
                FMath::Max(Mountain, Hill * 0.72), 0.0, 1.0
            );
            const double StartArea = FMath::Lerp(
                LowlandStartArea, MountainStartArea, ReliefClass
            ) * ErosionDrainageSourceMultiplier(Data, Cell);
            const double AreaRatio = Data.Accumulation[Cell]
                / FMath::Max(0.01, StartArea);
            const double ChannelPower = Smooth01(FMath::Clamp(
                FMath::Loge(1.0 + FMath::Max(0.0, AreaRatio)) / FMath::Loge(9.0),
                0.0, 1.0
            ));
            const double Slope = FMath::Clamp(Data.Slope[Cell], 0.0, 0.8);

            // Differential weathering creates residual mesa/escarpment terrain.
            const double Upland = FMath::Clamp(Hill + Mountain * 0.35, 0.0, 1.0);
            Delta[Cell] -= Desert * Upland * (1.0 - Resistance)
                * Strength * Data.CellSize * 0.006
                * (0.35 + FMath::Clamp(Slope / 0.10, 0.0, 1.0));

            if (ChannelPower <= 0.0 || Data.ReceiverA[Cell] == INDEX_NONE)
            {
                continue;
            }

            const double CanyonSuitability = Desert
                * Smooth01(FMath::Clamp((Resistance - 0.22) / 0.55, 0.0, 1.0))
                * FMath::Clamp(Hill + Mountain * 0.55, 0.0, 1.0);
            // Same D8-dominance damping as ApplyStreamPowerErosion, and for
            // the same reason: this channel-centred incision term (not the
            // deliberate BroadValley widening below it) can otherwise
            // entrench a second parallel candidate a cell away from the real
            // channel, since MFD lets both accumulate enough area.
            const double D8Dominance = Data.AccumulationD8.IsValidIndex(Cell)
                ? FMath::Clamp(
                    Data.AccumulationD8[Cell] / FMath::Max(0.01, Data.Accumulation[Cell]), 0.0, 1.0
                ) : 1.0;
            const double Incision = Strength * Data.CellSize
                * FMath::Lerp(0.022, 0.065, CanyonSuitability)
                * ChannelPower
                * (0.45 + FMath::Clamp(Slope / 0.09, 0.0, 1.0) * 0.55)
                * FMath::Lerp(1.0 - Resistance * 0.72, 0.78, CanyonSuitability)
                * D8Dominance;
            Delta[Cell] -= Incision;

            // Large streams in weak/wet terrain broaden valleys. Resistant arid
            // terrain suppresses that widening, producing narrower canyon forms.
            const double BroadValley = ChannelPower * (1.0 - CanyonSuitability)
                * (1.0 - Resistance * 0.78);
            if (BroadValley > 0.02)
            {
                const int32 Radius = ChannelPower > 0.72
                    ? 3 : (ChannelPower > 0.42 ? 2 : 1);
                for (int32 DY = -Radius; DY <= Radius; ++DY)
                {
                    for (int32 DX = -Radius; DX <= Radius; ++DX)
                    {
                        if (DX == 0 && DY == 0)
                        {
                            continue;
                        }
                        const int32 NX = X + DX;
                        const int32 NY = Y + DY;
                        if (!Data.IsValid(NX, NY) || Data.IsBoundary(NX, NY))
                        {
                            continue;
                        }
                        const double Distance = FMath::Sqrt(
                            static_cast<double>(DX * DX + DY * DY)
                        );
                        if (Distance > static_cast<double>(Radius) + 0.15)
                        {
                            continue;
                        }
                        const int32 Neighbor = Data.Index(NX, NY);
                        const double NeighborResistance = bUseResistance
                            ? FMath::Clamp(
                                Data.Resistance[Neighbor] * EffectiveResistance,
                                0.0, 0.94
                            ) : 0.0;
                        const double Falloff = 1.0
                            - Distance / (static_cast<double>(Radius) + 0.75);
                        Delta[Neighbor] -= FMath::Max(
                            0.0,
                            Incision * BroadValley * Falloff
                                * (1.0 - NeighborResistance * 0.82) * 0.34
                        );
                    }
                }
            }
        }

        const double MaximumPassChange = Data.CellSize * 0.12;
        for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
        {
            Data.Height[Cell] += FMath::Clamp(
                Delta[Cell], -MaximumPassChange, MaximumPassChange
            );
        }
    }

    PriorityFlood(Data, Epsilon);
    BuildContinuousFlow(Data);
}

// The authoritative single-direction receiver for river/lake network
// topology. This used to reduce the MFD split (ReceiverA/B) back to one
// direction per cell via a >=0.5 weight tie-break, but that reduction is
// independent at every cell along a flow path - wherever the MFD split sits
// near 50/50 (common in wide valley floors and, worst of all, right at
// basin spillways/saddles) it produces braided parallel "channels" and
// unstable outlet routing. ReceiverD8 is a true single steepest-descent
// tree computed directly from FilledHeight, so it stays a coherent,
// non-braided tree end to end.
static int32 PrimaryReceiver(const FAvenorStripData& Data, int32 Cell)
{
    if (Cell == INDEX_NONE || !Data.ReceiverD8.IsValidIndex(Cell))
    {
        return INDEX_NONE;
    }
    return Data.ReceiverD8[Cell];
}

static double DrainageScaleAlpha(double Area, double MainRiverArea)
{
    if (Area >= MainRiverArea)
    {
        return 1.0;
    }
    return Smooth01(FMath::Loge(1.0 + FMath::Max(0.0, Area)) / FMath::Loge(1.0 + FMath::Max(0.01, MainRiverArea)));
}
} // namespace UE::Avenor::Strip

namespace UE::Avenor::Strip
{
struct FLakeBoundaryEdge
{
    FIntPoint Start;
    FIntPoint End;
    FVector2D Position = FVector2D::ZeroVector;
    bool bUsed = false;
};

static TArray<FVector> TraceComponentBoundary(
    const FAvenorStripData& Data,
    const TArray<int32>& Cells,
    double SurfaceHeight,
    double FinalPointSpacing
)
{
    TSet<int32> Membership;
    Membership.Reserve(Cells.Num());
    for (int32 Cell : Cells)
    {
        Membership.Add(Cell);
    }
    TArray<FLakeBoundaryEdge> Edges;
    auto AddEdge = [&Data, SurfaceHeight, &Edges](
        int32 Cell, int32 OutsideX, int32 OutsideY,
        const FIntPoint& Start, const FIntPoint& End)
    {
        FVector2D Position;
        if (Data.IsValid(OutsideX, OutsideY))
        {
            const int32 Outside = Data.Index(OutsideX, OutsideY);
            const double InsideHeight = Data.Height[Cell];
            const double OutsideHeight = Data.Height[Outside];
            const double Denominator = OutsideHeight - InsideHeight;
            const double Alpha = FMath::Abs(Denominator) > 1.0
                ? FMath::Clamp((SurfaceHeight - InsideHeight) / Denominator, 0.05, 0.95)
                : 0.5;
            Position = FMath::Lerp(Data.CellPosition(Cell), Data.CellPosition(Outside), Alpha);
        }
        else
        {
            Position = FVector2D(
                Data.Bounds.Min.X + (Start.X + End.X) * 0.5 * Data.CellSize,
                Data.Bounds.Min.Y + (Start.Y + End.Y) * 0.5 * Data.CellSize
            );
        }
        Edges.Add({Start, End, Position, false});
    };
    for (int32 Cell : Cells)
    {
        const int32 X = Cell % Data.Columns;
        const int32 Y = Cell / Data.Columns;
        if (Y == 0 || !Membership.Contains(Data.Index(X, Y - 1)))
        {
            AddEdge(Cell, X, Y - 1, FIntPoint(X, Y), FIntPoint(X + 1, Y));
        }
        if (X + 1 >= Data.Columns || !Membership.Contains(Data.Index(X + 1, Y)))
        {
            AddEdge(Cell, X + 1, Y, FIntPoint(X + 1, Y), FIntPoint(X + 1, Y + 1));
        }
        if (Y + 1 >= Data.Rows || !Membership.Contains(Data.Index(X, Y + 1)))
        {
            AddEdge(Cell, X, Y + 1, FIntPoint(X + 1, Y + 1), FIntPoint(X, Y + 1));
        }
        if (X == 0 || !Membership.Contains(Data.Index(X - 1, Y)))
        {
            AddEdge(Cell, X - 1, Y, FIntPoint(X, Y + 1), FIntPoint(X, Y));
        }
    }
    if (Edges.IsEmpty())
    {
        return {};
    }
    TArray<FVector> Boundary;
    for (int32 StartIndex = 0; StartIndex < Edges.Num(); ++StartIndex)
    {
        if (Edges[StartIndex].bUsed)
        {
            continue;
        }
        TArray<FVector> Loop;
        int32 EdgeIndex = StartIndex;
        const FIntPoint First = Edges[StartIndex].Start;
        for (int32 Guard = 0; Guard <= Edges.Num(); ++Guard)
        {
            FLakeBoundaryEdge& Edge = Edges[EdgeIndex];
            if (Edge.bUsed)
            {
                break;
            }
            Edge.bUsed = true;
            Loop.Emplace(Edge.Position.X, Edge.Position.Y, SurfaceHeight);
            if (Edge.End == First)
            {
                break;
            }
            EdgeIndex = INDEX_NONE;
            for (int32 CandidateIndex = 0; CandidateIndex < Edges.Num(); ++CandidateIndex)
            {
                if (!Edges[CandidateIndex].bUsed && Edges[CandidateIndex].Start == Edge.End)
                {
                    EdgeIndex = CandidateIndex;
                    break;
                }
            }
            if (EdgeIndex == INDEX_NONE)
            {
                Loop.Reset();
                break;
            }
        }
        if (Loop.Num() > Boundary.Num())
        {
            Boundary = MoveTemp(Loop);
        }
    }
    if (Boundary.Num() < 4)
    {
        return {};
    }
    double Perimeter = 0.0;
    for (int32 Index = 0; Index < Boundary.Num(); ++Index)
    {
        Perimeter += FVector2D::Distance(
            FVector2D(Boundary[Index]),
            FVector2D(Boundary[(Index + 1) % Boundary.Num()])
        );
    }
    // Retain enough of the sub-cell edge interpolation for smoothing to work
    // with. The previous 28-point target followed by a very large Douglas-
    // Peucker tolerance threw most of that information away and reduced
    // small lakes back to rounded rectangles.
    const double MinimumSpacing = FMath::Max(300.0, FinalPointSpacing * 0.55);
    const double MaximumSpacing = FMath::Max(
        MinimumSpacing, Data.CellSize * 0.55
    );
    const double AdaptiveSpacing = FMath::Clamp(
        Perimeter / 48.0, MinimumSpacing, MaximumSpacing
    );
    TArray<FVector> Reduced = ResamplePolyline(Boundary, AdaptiveSpacing, true);
    Boundary = ChaikinSmooth(Reduced, true, 2);
    const double SimplificationTolerance = FMath::Min(
        FMath::Max(150.0, FinalPointSpacing * 0.65),
        Data.CellSize * 0.10
    );
    return SimplifyFeaturePolyline(Boundary, SimplificationTolerance, true);
}

struct FLakeCandidate
{
    TArray<int32> Cells;
    double SurfaceHeight = TNumericLimits<double>::Max();
    double MaximumDepth = 0.0;
    double CatchmentArea = 0.0;
    double MinX = TNumericLimits<double>::Max();
    double MaxX = -TNumericLimits<double>::Max();
    double MinY = TNumericLimits<double>::Max();
    double MaxY = -TNumericLimits<double>::Max();
    bool bRiverTerminus = false;
    int32 RimSpillCell = INDEX_NONE;
    double RimSpillHeight = TNumericLimits<double>::Max();
};

static TArray<bool> BuildAuthoritativeRiverNetwork(
    const FAvenorStripData& Data,
    double MountainStartArea,
    double LowlandStartArea,
    double MinimumSystemLength,
    int32& OutSeedCellCount,
    int32& OutContinuationCellCount,
    int32& OutRejectedSystemCount
)
{
    OutSeedCellCount = 0;
    OutContinuationCellCount = 0;
    OutRejectedSystemCount = 0;
    TArray<bool> Channel;
    Channel.Init(false, Data.Height.Num());
    TArray<bool> SeedCells;
    SeedCells.Init(false, Data.Height.Num());
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        const double MountainFraction = FMath::Clamp(
            Data.Slope[Cell] / 0.18, 0.0, 1.0
        );
        const double StartArea = FMath::Lerp(
            LowlandStartArea, MountainStartArea, MountainFraction
        ) * DrainageSourceMultiplier(Data, Cell);
        // Use the D8 accumulation here, matching the D8-based PrimaryReceiver
        // topology this function builds Channel from - the MFD Accumulation
        // used for erosion incision doesn't necessarily agree cell-for-cell
        // with which single path PrimaryReceiver says the water takes.
        if (Data.AccumulationD8[Cell] >= StartArea
            && PrimaryReceiver(Data, Cell) != INDEX_NONE)
        {
            SeedCells[Cell] = true;
            Channel[Cell] = true;
        }
    }

    for (int32 SeedCell = 0; SeedCell < Data.Height.Num(); ++SeedCell)
    {
        if (!SeedCells[SeedCell])
        {
            continue;
        }
        int32 Cell = SeedCell;
        for (int32 Guard = 0; Guard < Data.Height.Num(); ++Guard)
        {
            const int32 Receiver = PrimaryReceiver(Data, Cell);
            if (Receiver == INDEX_NONE || !Channel.IsValidIndex(Receiver) || Channel[Receiver])
            {
                break;
            }
            Channel[Receiver] = true;
            Cell = Receiver;
        }
    }
    TArray<int32> SystemParent;
    SystemParent.Init(INDEX_NONE, Data.Height.Num());
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (Channel[Cell])
        {
            SystemParent[Cell] = Cell;
        }
    }
    auto FindSystemRoot = [&](int32 Cell)
    {
        int32 Root = Cell;
        while (SystemParent[Root] != Root)
        {
            Root = SystemParent[Root];
        }
        while (SystemParent[Cell] != Cell)
        {
            const int32 Next = SystemParent[Cell];
            SystemParent[Cell] = Root;
            Cell = Next;
        }
        return Root;
    };
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (!Channel[Cell])
        {
            continue;
        }
        const int32 Receiver = PrimaryReceiver(Data, Cell);
        if (Receiver == INDEX_NONE || !Channel[Receiver])
        {
            continue;
        }
        const int32 RootA = FindSystemRoot(Cell);
        const int32 RootB = FindSystemRoot(Receiver);
        if (RootA != RootB)
        {
            SystemParent[RootB] = RootA;
        }
    }
    TArray<double> SystemLength;
    SystemLength.Init(0.0, Data.Height.Num());
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (!Channel[Cell])
        {
            continue;
        }
        const int32 Receiver = PrimaryReceiver(Data, Cell);
        if (Receiver != INDEX_NONE && Channel[Receiver])
        {
            SystemLength[FindSystemRoot(Cell)] += FVector2D::Distance(Data.CellPosition(Cell), Data.CellPosition(Receiver));
        }
    }
    TSet<int32> RejectedRoots;
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (Channel[Cell])
        {
            const int32 Root = FindSystemRoot(Cell);
            if (SystemLength[Root] < MinimumSystemLength)
            {
                RejectedRoots.Add(Root);
                Channel[Cell] = false;
            }
        }
    }
    OutRejectedSystemCount = RejectedRoots.Num();

    TArray<int32> Endpoints;
    for (int32 Cell = 0; Cell < Channel.Num(); ++Cell)
    {
        if (!Channel[Cell])
        {
            continue;
        }
        const int32 Receiver = PrimaryReceiver(Data, Cell);
        if (Receiver != INDEX_NONE
            && Channel.IsValidIndex(Receiver)
            && !Channel[Receiver])
        {
            Endpoints.Add(Cell);
        }
    }
    for (int32 Endpoint : Endpoints)
    {
        int32 Cell = Endpoint;
        for (int32 Guard = 0; Guard < Data.Height.Num(); ++Guard)
        {
            const int32 Receiver = PrimaryReceiver(Data, Cell);
            if (Receiver == INDEX_NONE || !Channel.IsValidIndex(Receiver))
            {
                break;
            }
            if (Channel[Receiver])
            {
                break;
            }
            Channel[Receiver] = true;
            Cell = Receiver;
        }
    }

    for (int32 Cell = 0; Cell < Channel.Num(); ++Cell)
    {
        if (Channel[Cell])
        {
            SeedCells[Cell] ? ++OutSeedCellCount : ++OutContinuationCellCount;
        }
    }
    return Channel;
}

static void RebuildD8Accumulation(FAvenorStripData& Data)
{
    const int32 CellCount = Data.Height.Num();
    Data.AccumulationD8.SetNumUninitialized(CellCount);
    const bool bHasRunoff = Data.Runoff.Num() == CellCount;
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        const double Runoff = bHasRunoff
            ? FMath::Clamp(Data.Runoff[Cell], 0.05, 2.0)
            : 1.0;
        Data.AccumulationD8[Cell] =
            Data.CellAreaSquareKilometres() * Runoff;
    }
    TArray<int32> Order;
    Order.SetNumUninitialized(CellCount);
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        Order[Cell] = Cell;
    }
    Order.Sort([&](int32 A, int32 B)
    {
        return Data.FilledHeight[A] > Data.FilledHeight[B];
    });
    for (int32 Cell : Order)
    {
        const int32 Receiver = PrimaryReceiver(Data, Cell);
        if (Receiver != INDEX_NONE && Data.AccumulationD8.IsValidIndex(Receiver))
        {
            Data.AccumulationD8[Receiver] += Data.AccumulationD8[Cell];
        }
    }
}

// A small tributary can run parallel to a much larger, lower trunk because
// D8 only examines the eight immediate neighbours. Over geomorphic time a
// low, narrow divide in that situation is normally breached by headward
// erosion (stream capture). Resolve only that conservative case: nearby,
// substantially stronger lower channel, minor intervening rise, and never a
// shortcut to the river's own existing downstream path.
static int32 CaptureNearbyDominantChannels(
    FAvenorStripData& Data,
    const TArray<bool>& Channel
)
{
    if (Channel.Num() != Data.Height.Num())
    {
        return 0;
    }
    TArray<int32> ChannelOrder;
    for (int32 Cell = 0; Cell < Channel.Num(); ++Cell)
    {
        if (Channel[Cell])
        {
            ChannelOrder.Add(Cell);
        }
    }
    ChannelOrder.Sort([&](int32 A, int32 B)
    {
        return Data.AccumulationD8[A] > Data.AccumulationD8[B];
    });

    constexpr int32 SearchRadius = 3;
    int32 CaptureCount = 0;
    for (int32 Cell : ChannelOrder)
    {
        const int32 X = Cell % Data.Columns;
        const int32 Y = Cell / Data.Columns;
        if (Data.IsBoundary(X, Y))
        {
            continue;
        }
        const double CellHeight = Data.FilledHeight[Cell];
        const double CellArea = Data.AccumulationD8[Cell];
        int32 BestTarget = INDEX_NONE;
        double BestScore = 0.0;
        for (int32 DY = -SearchRadius; DY <= SearchRadius; ++DY)
        {
            for (int32 DX = -SearchRadius; DX <= SearchRadius; ++DX)
            {
                if ((DX == 0 && DY == 0)
                    || DX * DX + DY * DY > SearchRadius * SearchRadius)
                {
                    continue;
                }
                const int32 NX = X + DX;
                const int32 NY = Y + DY;
                if (!Data.IsValid(NX, NY))
                {
                    continue;
                }
                const int32 Target = Data.Index(NX, NY);
                if (!Channel[Target])
                {
                    continue;
                }
                const double TargetArea = Data.AccumulationD8[Target];
                const double MinimumDominance = FMath::Max(
                    CellArea * 1.20,
                    CellArea + Data.CellAreaSquareKilometres() * 4.0
                );
                if (TargetArea < MinimumDominance)
                {
                    continue;
                }

                bool bAlreadyDownstream = false;
                int32 Probe = Cell;
                for (int32 Step = 0; Step < 24; ++Step)
                {
                    Probe = PrimaryReceiver(Data, Probe);
                    if (Probe == INDEX_NONE)
                    {
                        break;
                    }
                    if (Probe == Target)
                    {
                        bAlreadyDownstream = true;
                        break;
                    }
                }
                if (bAlreadyDownstream)
                {
                    continue;
                }

                const FVector2D StartPosition = Data.CellPosition(Cell);
                const FVector2D TargetPosition = Data.CellPosition(Target);
                const double Distance = FVector2D::Distance(
                    StartPosition, TargetPosition
                );
                const double Drop = CellHeight - Data.FilledHeight[Target];
                if (Drop < FMath::Max(50.0, Distance * 0.004))
                {
                    continue;
                }
                double MaximumBarrier = CellHeight;
                constexpr int32 BarrierSamples = 6;
                for (int32 Sample = 1; Sample < BarrierSamples; ++Sample)
                {
                    const double Alpha =
                        static_cast<double>(Sample) / BarrierSamples;
                    MaximumBarrier = FMath::Max(
                        MaximumBarrier,
                        Data.SampleGrid(
                            Data.Height,
                            FMath::Lerp(StartPosition, TargetPosition, Alpha)
                        )
                    );
                }
                const double AllowedBarrier = FMath::Max(
                    100.0, Distance * 0.035
                );
                if (MaximumBarrier > CellHeight + AllowedBarrier)
                {
                    continue;
                }
                const double Score = (Drop / FMath::Max(1.0, Distance))
                    * (1.0 + FMath::Loge(TargetArea / FMath::Max(0.001, CellArea)));
                if (Score > BestScore)
                {
                    BestScore = Score;
                    BestTarget = Target;
                }
            }
        }
        if (BestTarget != INDEX_NONE)
        {
            Data.ReceiverD8[Cell] = BestTarget;
            ++CaptureCount;
        }
    }
    if (CaptureCount > 0)
    {
        RebuildD8Accumulation(Data);
    }
    return CaptureCount;
}

static TArray<int32> FindRiverFedBasinSeeds(
    const FAvenorStripData& Data,
    const TArray<bool>& Channel,
    double SignificantConfluenceArea
)
{
    TArray<int32> Seeds;
    constexpr double DepressionThreshold = 50.0;
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (!Channel[Cell])
        {
            continue;
        }
        const int32 X = Cell % Data.Columns;
        const int32 Y = Cell / Data.Columns;
        constexpr int32 BoundaryMargin = 3;
        const bool bNearBoundary =
            X <= BoundaryMargin
            || X >= Data.Columns - 1 - BoundaryMargin
            || Y <= BoundaryMargin
            || Y >= Data.Rows - 1 - BoundaryMargin;
        if (bNearBoundary)
        {
            continue;
        }
        const bool bMeaningfulDepression =
            Data.DepressionFillHeight.IsValidIndex(Cell)
            && (Data.DepressionFillHeight[Cell] - Data.Height[Cell])
                >= DepressionThreshold;
        const int32 Receiver = PrimaryReceiver(Data, Cell);
        // In principle a cell whose receiver has fallen out of the channel
        // network is an interior terminus. In practice this never fires:
        // BuildAuthoritativeRiverNetwork's endpoint-continuation guarantees
        // every channel cell's receiver is also a channel cell all the way
        // to the map edge, and boundary cells are excluded above. Kept as a
        // defensive check (e.g. if that invariant is ever relaxed), but the
        // real signal for "this basin deserves the mandatory/looser lake
        // acceptance path" is bMeaningfulDepression or bSignificantConfluence
        // below - a wide, shallow pool fed by a real river is exactly as
        // legitimate a lake as a deep one, and depth alone was previously
        // the only way in.
        const bool bInteriorTerminus =
            Receiver == INDEX_NONE
            || !Channel.IsValidIndex(Receiver)
            || !Channel[Receiver];
        const bool bSignificantConfluence =
            Data.AccumulationD8.IsValidIndex(Cell)
            && Data.AccumulationD8[Cell] >= SignificantConfluenceArea;
        if (bMeaningfulDepression || bInteriorTerminus || bSignificantConfluence)
        {
            Seeds.Add(Cell);
        }
    }
    return Seeds;
}

static void ExtractLakes(
    FAvenorStripData& Data,
    const TArray<int32>& MandatoryTerminusSeeds,
    double MinimumDepth,
    double MinimumBedDepth,
    double MaximumBedDepth,
    double MaximumArea,
    int32 MaximumCount,
    double MaximumCoverageFraction,
    double BankBlendWidth,
    double DepthRampWidth,
    double FeaturePointSpacing,
    double SurfaceInset,
    bool bWantOutflows,
    TArray<int32>& OutOutflowSeeds
)
{
    Data.Lakes.Reset();
    Data.LakeIndex.Init(INDEX_NONE, Data.Height.Num());
    OutOutflowSeeds.Reset();
    if (MaximumCount <= 0)
    {
        return;
    }
    TSet<int32> MandatorySeedSet(MandatoryTerminusSeeds);
    const double WorldArea = Data.Height.Num() * Data.CellAreaSquareKilometres();
    // MaximumLakeArea's historical 250 km2 ceiling was effectively irrelevant
    // on the default strip; the 3% total-coverage allowance let one basin
    // consume roughly 60 km2 and dominate an entire generated district.
    // Keep exceptional large lakes possible on larger worlds, but scale the
    // single-lake ceiling to the geography being generated.
    const double SustainableMaximumLakeArea = FMath::Min(
        MaximumArea,
        FMath::Max(
            Data.CellAreaSquareKilometres() * 16.0,
            WorldArea * 0.0125
        )
    );
    TArray<bool> Candidate;
    Candidate.Init(false, Data.Height.Num());
    constexpr double ShorelineFillThreshold = 50.0;
    for (int32 Y = 1; Y + 1 < Data.Rows; ++Y)
    {
        for (int32 X = 1; X + 1 < Data.Columns; ++X)
        {
            const int32 Cell = Data.Index(X, Y);
            Candidate[Cell] = (Data.DepressionFillHeight[Cell] - Data.Height[Cell]) >= ShorelineFillThreshold;
        }
    }
    // A basin is a connected component of genuinely filled depression cells.
    // Do not dilate this mask to bridge nearby components: doing so joins
    // hydraulically unrelated depressions solely because they are close on
    // the analysis grid, absorbs the separating hillside, and then assigns
    // the merged shape one (usually incorrect) minimum surface height.
    TArray<bool> Visited;
    Visited.Init(false, Data.Height.Num());
    TArray<FLakeCandidate> Basins;
    constexpr int32 CardinalX[4] = {1, 0, -1, 0};
    constexpr int32 CardinalY[4] = {0, 1, 0, -1};
    for (int32 SeedCell = 0; SeedCell < Data.Height.Num(); ++SeedCell)
    {
        if (!Candidate[SeedCell] || Visited[SeedCell])
        {
            continue;
        }
        FLakeCandidate Basin;
        TArray<int32> Queue;
        Queue.Add(SeedCell);
        Visited[SeedCell] = true;
        for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
        {
            const int32 Cell = Queue[QueueIndex];
            Basin.Cells.Add(Cell);
            Basin.SurfaceHeight = FMath::Min(
                Basin.SurfaceHeight,
                Data.DepressionFillHeight[Cell]
            );
            Basin.MaximumDepth = FMath::Max(Basin.MaximumDepth, Data.DepressionFillHeight[Cell] - Data.Height[Cell]);
            // D8, not MFD: catchment sizing feeds mandatory/optional lake
            // acceptance, which is decided from the same D8 channel network
            // (see FindRiverFedBasinSeeds/PrimaryReceiver) that determines
            // whether this basin is a real river terminus in the first place.
            Basin.CatchmentArea = FMath::Max(Basin.CatchmentArea, Data.AccumulationD8[Cell]);
            const FVector2D CellPos = Data.CellPosition(Cell);
            Basin.MinX = FMath::Min(Basin.MinX, CellPos.X);
            Basin.MaxX = FMath::Max(Basin.MaxX, CellPos.X);
            Basin.MinY = FMath::Min(Basin.MinY, CellPos.Y);
            Basin.MaxY = FMath::Max(Basin.MaxY, CellPos.Y);
            const int32 X = Cell % Data.Columns;
            const int32 Y = Cell / Data.Columns;
            for (int32 Direction = 0; Direction < 4; ++Direction)
            {
                const int32 NX = X + CardinalX[Direction];
                const int32 NY = Y + CardinalY[Direction];
                if (!Data.IsValid(NX, NY))
                {
                    continue;
                }
                const int32 Neighbor = Data.Index(NX, NY);
                if (Candidate[Neighbor])
                {
                    if (!Visited[Neighbor])
                    {
                        Visited[Neighbor] = true;
                        Queue.Add(Neighbor);
                    }
                }
                else if (Data.Height[Neighbor] < Basin.RimSpillHeight)
                {
                    Basin.RimSpillHeight = Data.Height[Neighbor];
                    Basin.RimSpillCell = Neighbor;
                }
            }
        }
        const double Area = Basin.Cells.Num() * Data.CellAreaSquareKilometres();
        for (int32 Cell : Basin.Cells)
        {
            if (MandatorySeedSet.Contains(Cell))
            {
                Basin.bRiverTerminus = true;
                break;
            }
        }
        const double SanityCap = SustainableMaximumLakeArea * 1.5;
        if (Area >= Data.CellAreaSquareKilometres() * 2.0 && Area <= SanityCap)
        {
            Basins.Add(MoveTemp(Basin));
        }
    }
    TArray<FLakeCandidate> MandatoryBasins;
    TArray<FLakeCandidate> OptionalBasins;
    for (FLakeCandidate& Basin : Basins)
    {
        (Basin.bRiverTerminus ? MandatoryBasins : OptionalBasins).Add(MoveTemp(Basin));
    }
    MandatoryBasins.Sort([](const FLakeCandidate& A, const FLakeCandidate& B) { return A.CatchmentArea > B.CatchmentArea; });
    OptionalBasins.Sort([](const FLakeCandidate& A, const FLakeCandidate& B)
    {
        return A.CatchmentArea * FMath::Sqrt(A.MaximumDepth) > B.CatchmentArea * FMath::Sqrt(B.MaximumDepth);
    });
    TArray<FLakeCandidate> OrderedBasins;
    OrderedBasins.Reserve(MandatoryBasins.Num() + OptionalBasins.Num());
    for (FLakeCandidate& Basin : MandatoryBasins) { OrderedBasins.Add(MoveTemp(Basin)); }
    for (FLakeCandidate& Basin : OptionalBasins) { OrderedBasins.Add(MoveTemp(Basin)); }

    const double MaximumTotalLakeArea = WorldArea * FMath::Clamp(MaximumCoverageFraction, 0.0, 0.5);
    double AcceptedLakeArea = 0.0;
    int32 AcceptedCount = 0;
    Data.RiverTerminusLakeCandidates = MandatoryBasins.Num();
    for (const FLakeCandidate& CandidateBasin : OrderedBasins)
    {
        const double BasinArea = CandidateBasin.Cells.Num() * Data.CellAreaSquareKilometres();
        // A basin only a handful of cells across has no boundary detail for
        // the shoreline smoothing pipeline to work with - it traces as a
        // blocky, unmistakably grid-aligned near-rectangle no matter how
        // much smoothing runs afterward. Below this size it can never read
        // as a natural pond outline, so reject it as a lake outright rather
        // than spawn something that looks like a rendering bug.
        if (BasinArea <= 0.0 || CandidateBasin.Cells.Num() < 9)
        {
            continue;
        }
        const double BaseRequiredDepth = CandidateBasin.bRiverTerminus
            ? MinimumDepth
            : MinimumDepth * 1.5;
        double MeanSlope = 0.0;
        double MeanResistance = 0.0;
        double MeanDesert = 0.0;
        double MeanTemperature = 0.0;
        double MeanMoisture = 0.0;
        for (int32 Cell : CandidateBasin.Cells)
        {
            MeanSlope += Data.Slope.IsValidIndex(Cell) ? Data.Slope[Cell] : 0.0;
            MeanResistance += Data.Resistance.IsValidIndex(Cell) ? Data.Resistance[Cell] : 0.0;
            MeanDesert += Data.DesertMask.IsValidIndex(Cell) ? Data.DesertMask[Cell] : 0.0;
            MeanTemperature += Data.MacroTemperature.IsValidIndex(Cell)
                ? Data.MacroTemperature[Cell] : 0.5;
            MeanMoisture += Data.MacroMoisture.IsValidIndex(Cell)
                ? Data.MacroMoisture[Cell] : 0.5;
        }
        const double BasinCellCount = FMath::Max(1, CandidateBasin.Cells.Num());
        MeanSlope /= BasinCellCount;
        MeanResistance /= BasinCellCount;
        MeanDesert /= BasinCellCount;
        MeanTemperature /= BasinCellCount;
        MeanMoisture /= BasinCellCount;
        const double HeatStress = Smooth01(FMath::Clamp(
            (MeanTemperature - 0.55) / 0.35, 0.0, 1.0
        ));
        const double DrynessStress = Smooth01(FMath::Clamp(
            (0.58 - MeanMoisture) / 0.43, 0.0, 1.0
        ));
        const double EvaporationStress = HeatStress * DrynessStress;
        const double RequiredDepth = BaseRequiredDepth
            * FMath::Lerp(1.0, 2.1, EvaporationStress);
        const double SupportedBasinArea = FMath::Max(
            Data.CellAreaSquareKilometres() * 4.0,
            CandidateBasin.CatchmentArea
                * (CandidateBasin.bRiverTerminus ? 0.20 : 0.10)
                * FMath::Lerp(1.0, 0.30, EvaporationStress)
        );
        if (AcceptedCount >= MaximumCount ||
            CandidateBasin.MaximumDepth < RequiredDepth ||
            BasinArea > SustainableMaximumLakeArea ||
            BasinArea > SupportedBasinArea ||
            AcceptedLakeArea + BasinArea > MaximumTotalLakeArea)
        {
            continue;
        }
        {
            const double BoundingWidth = FMath::Max(1.0, CandidateBasin.MaxX - CandidateBasin.MinX);
            const double BoundingHeight = FMath::Max(1.0, CandidateBasin.MaxY - CandidateBasin.MinY);
            const double FillRatio = BasinArea / FMath::Max(0.0001, (BoundingWidth * BoundingHeight) / 10000000000.0);
            const double MinimumFillRatio = CandidateBasin.bRiverTerminus ? 0.12 : 0.18;
            const double AspectRatio = FMath::Max(BoundingWidth, BoundingHeight)
                / FMath::Max(1.0, FMath::Min(BoundingWidth, BoundingHeight));
            if (FillRatio < MinimumFillRatio
                || (AspectRatio > 7.5 && FillRatio < 0.28))
            {
                continue;
            }

            const bool bAridCanyonTrough =
                MeanDesert > 0.38
                && MeanResistance > 0.26
                && MeanSlope > 0.045
                && AspectRatio > 3.5
                && FillRatio < 0.35;
            if (bAridCanyonTrough)
            {
                continue;
            }
        }
        FLakeBasin Lake;
        Lake.SurfaceHeight = CandidateBasin.SurfaceHeight;
        Lake.MaximumDepth = FMath::Clamp(
            FMath::Max(CandidateBasin.MaximumDepth, MinimumBedDepth),
            MinimumBedDepth, FMath::Max(MinimumBedDepth, MaximumBedDepth)
        );
        // The actual ground-carve depth for this specific basin, not floored
        // up to MinimumBedDepth like MaximumDepth above. A shallow accepted
        // basin should carve a shallow bed, not the same fixed depth as
        // every other lake in the world.
        Lake.ModifierBedDepth = FMath::Clamp(
            CandidateBasin.MaximumDepth, 100.0, FMath::Max(100.0, MaximumBedDepth)
        );
        Lake.BankBlendWidth = BankBlendWidth;
        Lake.DepthRampWidth = DepthRampWidth;
        Lake.Shoreline = TraceComponentBoundary(
            Data, CandidateBasin.Cells, Lake.SurfaceHeight, FeaturePointSpacing
        );
        if (Lake.Shoreline.Num() < 4)
        {
            continue;
        }
        Lake.ShorelineHeight = CandidateBasin.SurfaceHeight;
        Lake.SurfaceHeight = CandidateBasin.SurfaceHeight
            - FMath::Max(0.0, SurfaceInset);
        for (FVector& Point : Lake.Shoreline)
        {
            Point.Z = Lake.SurfaceHeight;
        }
        double BasinInradius = 0.0;
        for (int32 Cell : CandidateBasin.Cells)
        {
            double EdgeDistance = 0.0;
            if (IsInsidePolygon(Data.CellPosition(Cell), Lake.Shoreline, &EdgeDistance))
            {
                BasinInradius = FMath::Max(BasinInradius, EdgeDistance);
            }
        }
        const double MinimumRamp = FMath::Max(500.0, Data.CellSize * 0.15);
        const double BasinRamp = FMath::Max(MinimumRamp, BasinInradius * 0.65);
        Lake.DepthRampWidth = FMath::Clamp(FMath::Max(DepthRampWidth, BasinInradius * 0.35), MinimumRamp, BasinRamp);
        for (const FVector& Point : Lake.Shoreline)
        {
            Lake.Bounds += FVector2D(Point);
        }
        Lake.Bounds = Lake.Bounds.ExpandBy(BankBlendWidth);
        const int32 NewLakeIndex = Data.Lakes.Num();
        for (int32 Cell : CandidateBasin.Cells)
        {
            Data.LakeIndex[Cell] = NewLakeIndex;
        }
        AcceptedLakeArea += BasinArea;
        ++AcceptedCount;
        if (CandidateBasin.bRiverTerminus)
        {
            ++Data.AcceptedRiverTerminusLakes;
        }
        else
        {
            ++Data.AcceptedOptionalLakes;
        }
        if (bWantOutflows && CandidateBasin.RimSpillCell != INDEX_NONE)
        {
            // Start at the first dry spill cell itself. Advancing immediately
            // to its receiver could skip the short connector between perched
            // basins or land directly inside the lower lake, so no visible
            // outflow reach was emitted between two different water levels.
            const int32 OutflowStart = CandidateBasin.RimSpillCell;
            if (PrimaryReceiver(Data, OutflowStart) != INDEX_NONE &&
                (!Data.LakeIndex.IsValidIndex(OutflowStart) ||
                 Data.LakeIndex[OutflowStart] == INDEX_NONE))
            {
                OutOutflowSeeds.Add(OutflowStart);
            }
        }
        Data.Lakes.Add(MoveTemp(Lake));
    }
}
} // namespace UE::Avenor::Strip

namespace UE::Avenor::Strip
{
static void EnforceDownhill(
    TArray<FVector>& Points,
    double MaximumSurfaceUndercut = 200.0
)
{
    if (Points.Num() < 2)
    {
        return;
    }
    for (int32 Index = 1; Index < Points.Num(); ++Index)
    {
        const double HorizontalDistance = FVector2D::Distance(FVector2D(Points[Index - 1]), FVector2D(Points[Index]));
        const double GroundHeight = Points[Index].Z;
        const double DesiredHeight = FMath::Min(
            GroundHeight,
            Points[Index - 1].Z - HorizontalDistance * 0.00015
        );
        // Never solve a locally awkward spline by cumulatively driving the
        // water surface far underground. If the route cannot remain downhill
        // within this small allowance, preserve the near-ground datum; the
        // geometry problem remains visible for diagnosis instead of becoming
        // an artificial gorge hundreds of metres deep.
        Points[Index].Z = FMath::Max(
            DesiredHeight,
            GroundHeight - FMath::Max(0.0, MaximumSurfaceUndercut)
        );
    }
}

static void ConstrainMeandersToValley(
    const FAvenorStripData& Data,
    const TArray<FVector>& BasePoints,
    TArray<FVector>& MeanderedPoints,
    double LowlandFraction
)
{
    if (BasePoints.Num() != MeanderedPoints.Num() || BasePoints.Num() < 3)
    {
        return;
    }

    const double Lowland = FMath::Clamp(LowlandFraction, 0.0, 1.0);
    // Wider than a single cell so an ordinary hillslope undulation doesn't
    // read as "left the valley" and snap the spline straight back to the
    // unmeandered base point; the flood-fill valley/lake logic elsewhere is
    // still what actually keeps the river out of real terrain.
    const double AllowedLateralRise = FMath::Lerp(
        Data.CellSize * 0.10,
        Data.CellSize * 0.40,
        Lowland
    );
    for (int32 Index = 1; Index + 1 < MeanderedPoints.Num(); ++Index)
    {
        const FVector2D BasePosition(BasePoints[Index]);
        const FVector2D CandidatePosition(MeanderedPoints[Index]);
        const double BaseHeight = Data.SampleGrid(Data.Height, BasePosition);
        const double CandidateHeight = Data.SampleGrid(
            Data.Height, CandidatePosition
        );
        const double CandidateSlope = Data.SampleGrid(
            Data.Slope, CandidatePosition
        );
        const double Rise = FMath::Max(0.0, CandidateHeight - BaseHeight);
        const double RiseScale = Rise <= AllowedLateralRise
            ? 1.0
            : FMath::Clamp(
                AllowedLateralRise / FMath::Max(1.0, Rise),
                0.0,
                1.0
            );
        // Only true steep terrain (not every moderate hillslope undulation)
        // should flatten a meander back to the raw flow-cell path - that
        // window used to start at 0.045 (a mild 4.5% grade), which combined
        // with the per-reach Lowland gate to suppress almost all wiggle
        // outside dead-flat valley floors and left visibly straight,
        // grid-aligned segments through ordinary hilly ground.
        const double SlopeScale = 1.0 - Smooth01(FMath::Clamp(
            (CandidateSlope - 0.09) / 0.13,
            0.0,
            1.0
        ));
        const double Scale = FMath::Clamp(
            FMath::Min(RiseScale, SlopeScale),
            0.0,
            1.0
        );
        MeanderedPoints[Index].X = FMath::Lerp(
            BasePoints[Index].X, MeanderedPoints[Index].X, Scale
        );
        MeanderedPoints[Index].Y = FMath::Lerp(
            BasePoints[Index].Y, MeanderedPoints[Index].Y, Scale
        );
    }
}

// Kept for source compatibility with earlier generated data. New river extraction
// no longer calls this per-point lateral snap because independently choosing a
// local low point on each sample could alternate sides of a valley and produce
// zig-zagging/crossing water splines.
static void ProjectRiverToLocalThalweg(
    const FAvenorStripData& Data,
    TArray<FVector>& Points
)
{
    if (Points.Num() < 3)
    {
        return;
    }
    for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
    {
        const FVector2D Previous(Points[Index - 1]);
        const FVector2D Current(Points[Index]);
        const FVector2D Next(Points[Index + 1]);
        const FVector2D Tangent = (Next - Previous).GetSafeNormal();
        if (Tangent.IsNearlyZero())
        {
            continue;
        }
        const FVector2D Normal = Rotate90(Tangent);
        const double LocalSlope = Data.SampleGrid(Data.Slope, Current);
        const double Steepness = Smooth01(FMath::Clamp(
            (LocalSlope - 0.025) / 0.10, 0.0, 1.0
        ));
        const double SearchRadius = Data.CellSize
            * FMath::Lerp(1.45, 0.75, Steepness);
        FVector2D Best = Current;
        double BestHeight = Data.SampleGrid(Data.Height, Current);
        double BestScore = BestHeight;
        for (int32 Step = -6; Step <= 6; ++Step)
        {
            if (Step == 0)
            {
                continue;
            }
            const double Offset = SearchRadius
                * static_cast<double>(Step) / 6.0;
            const FVector2D Candidate = Current + Normal * Offset;
            if (!Data.Bounds.IsInside(
                FVector(Candidate.X, Candidate.Y, Data.Bounds.GetCenter().Z)))
            {
                continue;
            }
            const double CandidateHeight = Data.SampleGrid(Data.Height, Candidate);
            const double CandidateSlope = Data.SampleGrid(Data.Slope, Candidate);
            const double UphillPenalty = FMath::Max(
                0.0, CandidateHeight - BestHeight
            ) * FMath::Lerp(0.6, 1.8, Steepness);
            const double Score = CandidateHeight
                + FMath::Abs(Offset) * 0.025
                + CandidateSlope * Data.CellSize * 0.10
                + UphillPenalty;
            if (Score < BestScore - 25.0)
            {
                BestScore = Score;
                BestHeight = CandidateHeight;
                Best = Candidate;
            }
        }
        const double Snap = FMath::Lerp(0.62, 0.95, Steepness);
        Points[Index].X = FMath::Lerp(Current.X, Best.X, Snap);
        Points[Index].Y = FMath::Lerp(Current.Y, Best.Y, Snap);
        Points[Index].Z = BestHeight;
    }
}

static double Cross2D(
    const FVector2D& A,
    const FVector2D& B,
    const FVector2D& C
)
{
    const FVector2D AB = B - A;
    const FVector2D AC = C - A;
    return AB.X * AC.Y - AB.Y * AC.X;
}

static bool ProperSegmentsIntersect(
    const FVector2D& A,
    const FVector2D& B,
    const FVector2D& C,
    const FVector2D& D
)
{
    const double AB_C = Cross2D(A, B, C);
    const double AB_D = Cross2D(A, B, D);
    const double CD_A = Cross2D(C, D, A);
    const double CD_B = Cross2D(C, D, B);
    return AB_C * AB_D < 0.0 && CD_A * CD_B < 0.0;
}

static void CutOffSelfIntersections(TArray<FVector>& Points)
{
    // Resolve the active channel the same way a natural neck cutoff does:
    // retain the new short connection and discard the enclosed loop.
    for (int32 Pass = 0; Pass < 16 && Points.Num() >= 4; ++Pass)
    {
        bool bCut = false;
        for (int32 First = 0;
             First + 1 < Points.Num() && !bCut; ++First)
        {
            for (int32 Second = First + 2;
                 Second + 1 < Points.Num(); ++Second)
            {
                if (ProperSegmentsIntersect(
                    FVector2D(Points[First]),
                    FVector2D(Points[First + 1]),
                    FVector2D(Points[Second]),
                    FVector2D(Points[Second + 1])
                ))
                {
                    Points.RemoveAt(
                        First + 1,
                        Second - First,
                        EAllowShrinking::No
                    );
                    bCut = true;
                    break;
                }
            }
        }
        if (!bCut)
        {
            break;
        }
    }
}

static void SuppressPathologicalMeanders(
    const TArray<FVector>& BasePoints,
    TArray<FVector>& Points,
    double CellSize
)
{
    if (Points.Num() < 4 || BasePoints.Num() != Points.Num())
    {
        return;
    }

    const double MinimumSeparation = CellSize * 1.6;
    for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
    {
        const FVector2D Incoming =
            (FVector2D(Points[Index]) - FVector2D(Points[Index - 1])).GetSafeNormal();
        const FVector2D Outgoing =
            (FVector2D(Points[Index + 1]) - FVector2D(Points[Index])).GetSafeNormal();
        const double Dot = FVector2D::DotProduct(Incoming, Outgoing);
        if (Dot < -0.10)
        {
            const double Severity = FMath::Clamp((-0.10 - Dot) / 0.90, 0.0, 1.0);
            const double Keep = FMath::Lerp(0.55, 0.18, Severity);
            Points[Index].X = FMath::Lerp(BasePoints[Index].X, Points[Index].X, Keep);
            Points[Index].Y = FMath::Lerp(BasePoints[Index].Y, Points[Index].Y, Keep);
        }
    }

    for (int32 Index = 4; Index + 1 < Points.Num(); ++Index)
    {
        double ClosestEarlier = TNumericLimits<double>::Max();
        for (int32 Earlier = 0; Earlier + 2 < Index; ++Earlier)
        {
            ClosestEarlier = FMath::Min(
                ClosestEarlier,
                SegmentDistance(
                    FVector2D(Points[Index]),
                    FVector2D(Points[Earlier]),
                    FVector2D(Points[Earlier + 1])
                )
            );
        }
        if (ClosestEarlier < MinimumSeparation)
        {
            const double SeparationAlpha = FMath::Clamp(
                ClosestEarlier / MinimumSeparation, 0.0, 1.0
            );
            const double Keep = FMath::Lerp(0.20, 0.75, SeparationAlpha);
            Points[Index].X = FMath::Lerp(BasePoints[Index].X, Points[Index].X, Keep);
            Points[Index].Y = FMath::Lerp(BasePoints[Index].Y, Points[Index].Y, Keep);
        }
    }

    // Proximity and turn-angle damping above reduce tight necks but do not
    // mathematically prohibit two non-adjacent segments from crossing. A
    // live single-channel river cannot cross itself: a natural cutoff leaves
    // an oxbow/abandoned channel. Revert the enclosed displaced span to the
    // non-self-crossing drainage route whenever a proper crossing remains.
    for (int32 Pass = 0; Pass < 4; ++Pass)
    {
        bool bRemovedCrossing = false;
        for (int32 First = 0;
             First + 1 < Points.Num() && !bRemovedCrossing; ++First)
        {
            for (int32 Second = First + 2;
                 Second + 1 < Points.Num(); ++Second)
            {
                if (!ProperSegmentsIntersect(
                    FVector2D(Points[First]),
                    FVector2D(Points[First + 1]),
                    FVector2D(Points[Second]),
                    FVector2D(Points[Second + 1])
                ))
                {
                    continue;
                }
                for (int32 Index = First + 1; Index <= Second; ++Index)
                {
                    Points[Index].X = BasePoints[Index].X;
                    Points[Index].Y = BasePoints[Index].Y;
                }
                bRemovedCrossing = true;
                break;
            }
        }
        if (!bRemovedCrossing)
        {
            break;
        }
    }
}

struct FRiverCandidate
{
    TArray<int32> Cells;
    double Score = 0.0;
    int32 StartLakeIndex = INDEX_NONE;
    int32 EndLakeIndex = INDEX_NONE;
};

static bool FindClosestLakeShorePoint(
    const FAvenorStripData& Data,
    const FVector2D& Position,
    double MaximumDistance,
    int32& OutLakeIndex,
    FVector2D& OutPoint)
{
    double BestDistance = MaximumDistance;
    OutLakeIndex = INDEX_NONE;
    for (int32 LakeIndex = 0; LakeIndex < Data.Lakes.Num(); ++LakeIndex)
    {
        const FLakeBasin& Lake = Data.Lakes[LakeIndex];
        if (Lake.Shoreline.Num() < 3 ||
            !Lake.Bounds.ExpandBy(MaximumDistance).IsInside(Position))
        {
            continue;
        }
        for (int32 PointIndex = 0; PointIndex < Lake.Shoreline.Num(); ++PointIndex)
        {
            const FVector2D A(Lake.Shoreline[PointIndex]);
            const FVector2D B(Lake.Shoreline[(PointIndex + 1) % Lake.Shoreline.Num()]);
            const FVector2D Segment = B - A;
            const double SegmentLengthSquared = Segment.SizeSquared();
            const double Alpha = SegmentLengthSquared > UE_DOUBLE_SMALL_NUMBER
                ? FMath::Clamp(FVector2D::DotProduct(Position - A, Segment) / SegmentLengthSquared, 0.0, 1.0)
                : 0.0;
            const FVector2D Candidate = A + Segment * Alpha;
            const double Distance = FVector2D::Distance(Position, Candidate);
            if (Distance < BestDistance)
            {
                BestDistance = Distance;
                OutLakeIndex = LakeIndex;
                OutPoint = Candidate;
            }
        }
    }
    return OutLakeIndex != INDEX_NONE;
}

static void BlendRiverEndpointToHeight(
    TArray<FVector>& Points,
    bool bAtStart,
    double TargetHeight,
    double ApproachLength
)
{
    if (Points.Num() < 2)
    {
        return;
    }
    const int32 EndpointIndex = bAtStart ? 0 : Points.Num() - 1;
    const double HeightDelta = TargetHeight - Points[EndpointIndex].Z;
    double Distance = 0.0;
    for (int32 Step = 0; Step < Points.Num(); ++Step)
    {
        const int32 Index = bAtStart ? Step : Points.Num() - 1 - Step;
        if (Step > 0)
        {
            const int32 Previous = bAtStart ? Index - 1 : Index + 1;
            Distance += FVector2D::Distance(
                FVector2D(Points[Previous]), FVector2D(Points[Index])
            );
        }
        if (Distance >= ApproachLength)
        {
            break;
        }
        const double Weight = Smooth01(1.0 - Distance / FMath::Max(1.0, ApproachLength));
        Points[Index].Z += HeightDelta * Weight;
    }
    Points[EndpointIndex].Z = TargetHeight;
}

static void AnchorRiverToLakes(
    const FAvenorStripData& Data,
    TArray<FVector>& Points,
    int32 StartLakeIndex,
    int32 EndLakeIndex)
{
    if (Points.Num() < 2)
    {
        return;
    }
    constexpr double MinimumGradient = 0.0006;
    const bool bStartsAtLake = Data.Lakes.IsValidIndex(StartLakeIndex);
    const bool bEndsAtLake = Data.Lakes.IsValidIndex(EndLakeIndex);
    const double ApproachLength = FMath::Max(Data.CellSize * 4.0, 40000.0);

    if (bStartsAtLake)
    {
        BlendRiverEndpointToHeight(
            Points, true, Data.Lakes[StartLakeIndex].SurfaceHeight, ApproachLength
        );
    }
    if (bEndsAtLake)
    {
        BlendRiverEndpointToHeight(
            Points, false, Data.Lakes[EndLakeIndex].SurfaceHeight, ApproachLength
        );
    }

    if (bStartsAtLake && bEndsAtLake)
    {
        TArray<double> Distances;
        Distances.SetNum(Points.Num());
        Distances[0] = 0.0;
        for (int32 Index = 1; Index < Points.Num(); ++Index)
        {
            Distances[Index] = Distances[Index - 1] + FVector2D::Distance(
                FVector2D(Points[Index - 1]), FVector2D(Points[Index])
            );
        }
        const double TotalLength = Distances.Last();
        const double StartHeight = Data.Lakes[StartLakeIndex].SurfaceHeight;
        const double EndHeight = Data.Lakes[EndLakeIndex].SurfaceHeight;
        if (StartHeight >= EndHeight + TotalLength * MinimumGradient)
        {
            for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
            {
                const double HighestAllowed =
                    StartHeight - Distances[Index] * MinimumGradient;
                const double LowestAllowed =
                    EndHeight + (TotalLength - Distances[Index]) * MinimumGradient;
                Points[Index].Z = FMath::Clamp(
                    Points[Index].Z, LowestAllowed, HighestAllowed
                );
            }
        }
        else
        {
            for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
            {
                const double Alpha = TotalLength > UE_DOUBLE_SMALL_NUMBER
                    ? Distances[Index] / TotalLength : 0.0;
                Points[Index].Z = FMath::Lerp(StartHeight, EndHeight, Alpha);
            }
        }
    }
    else if (bStartsAtLake)
    {
        EnforceDownhill(Points);
    }
    else if (bEndsAtLake)
    {
        for (int32 Index = Points.Num() - 2; Index >= 0; --Index)
        {
            const double HorizontalDistance = FVector2D::Distance(
                FVector2D(Points[Index]), FVector2D(Points[Index + 1])
            );
            Points[Index].Z = FMath::Max(
                Points[Index].Z,
                Points[Index + 1].Z + HorizontalDistance * MinimumGradient
            );
        }
    }
    if (bStartsAtLake)
    {
        Points[0].Z = Data.Lakes[StartLakeIndex].SurfaceHeight;
    }
    if (bEndsAtLake)
    {
        Points.Last().Z = Data.Lakes[EndLakeIndex].SurfaceHeight;
    }
}

static void AddLevelLakeJunctionPads(
    const FAvenorStripData& Data,
    TArray<FVector>& Points,
    int32 StartLakeIndex,
    int32 EndLakeIndex
)
{
    if (Points.Num() < 2)
    {
        return;
    }
    const double PadLength = FMath::Clamp(Data.CellSize * 0.05, 200.0, 500.0);

    if (Data.Lakes.IsValidIndex(StartLakeIndex))
    {
        const double LakeHeight = Data.Lakes[StartLakeIndex].SurfaceHeight;
        const FVector Shore = Points[0];
        FVector2D Downstream = FVector2D(Points[1]) - FVector2D(Shore);
        if (Downstream.Normalize())
        {
            const FVector Inside(
                Shore.X - Downstream.X * PadLength,
                Shore.Y - Downstream.Y * PadLength,
                LakeHeight
            );
            const FVector Outside(
                Shore.X + Downstream.X * PadLength,
                Shore.Y + Downstream.Y * PadLength,
                LakeHeight
            );
            Points[0].Z = LakeHeight;
            Points.Insert(Inside, 0);
            Points.Insert(Outside, 2);
        }
    }

    if (Data.Lakes.IsValidIndex(EndLakeIndex) && Points.Num() >= 2)
    {
        const double LakeHeight = Data.Lakes[EndLakeIndex].SurfaceHeight;
        const int32 ShoreIndex = Points.Num() - 1;
        const FVector Shore = Points[ShoreIndex];
        FVector2D IntoLake = FVector2D(Shore) - FVector2D(Points[ShoreIndex - 1]);
        if (IntoLake.Normalize())
        {
            const FVector Outside(
                Shore.X - IntoLake.X * PadLength,
                Shore.Y - IntoLake.Y * PadLength,
                LakeHeight
            );
            const FVector Inside(
                Shore.X + IntoLake.X * PadLength,
                Shore.Y + IntoLake.Y * PadLength,
                LakeHeight
            );
            Points[ShoreIndex].Z = LakeHeight;
            Points.Insert(Outside, ShoreIndex);
            Points.Add(Inside);
        }
    }
}

static void ExtractRivers(
    FAvenorStripData& Data,
    const TArray<bool>& AuthoritativeChannel,
    const TArray<int32>& ForcedOutflowSeeds,
    int32 Seed,
    double MainRiverArea,
    double HeadwaterWidth,
    double MainRiverWidth,
    double MaximumDepth,
    double HeadwaterValleyWidth,
    double MainValleyWidth,
    double MaximumValleyDepth,
    double MeanderStrength,
    double FeaturePointSpacing,
    int32 MaximumReaches,
    bool bCanyons,
    double CanyonStartArea,
    double ChannelSteepness
)
{
    Data.Rivers.Reset();
    if (MaximumReaches <= 0)
    {
        return;
    }
    TArray<bool> Channel = AuthoritativeChannel;
    if (Channel.Num() != Data.Height.Num())
    {
        Channel.Init(false, Data.Height.Num());
    }
    for (int32 Seed2 : ForcedOutflowSeeds)
    {
        if (Data.Height.IsValidIndex(Seed2) &&
            (!Data.LakeIndex.IsValidIndex(Seed2) || Data.LakeIndex[Seed2] == INDEX_NONE) &&
            PrimaryReceiver(Data, Seed2) != INDEX_NONE)
        {
            int32 Cell = Seed2;
            for (int32 Guard = 0; Guard < Data.Height.Num() && Data.Height.IsValidIndex(Cell); ++Guard)
            {
                Channel[Cell] = true;
                const int32 Receiver = PrimaryReceiver(Data, Cell);
                if (Receiver == INDEX_NONE || (Channel.IsValidIndex(Receiver) && Channel[Receiver]))
                {
                    break;
                }
                Cell = Receiver;
            }
        }
    }
    TSet<int32> ForcedSeedSet(ForcedOutflowSeeds);
    TArray<int32> UpstreamCount;
    UpstreamCount.Init(0, Data.Height.Num());
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (!Channel[Cell])
        {
            continue;
        }
        const int32 Receiver = PrimaryReceiver(Data, Cell);
        if (Receiver != INDEX_NONE && Channel[Receiver])
        {
            ++UpstreamCount[Receiver];
        }
    }
    TArray<int32> Starts;
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        const bool bInsideLake = Data.LakeIndex.IsValidIndex(Cell) && Data.LakeIndex[Cell] != INDEX_NONE;
        if (Channel[Cell] && !bInsideLake && UpstreamCount[Cell] != 1)
        {
            Starts.Add(Cell);
        }
    }
    for (int32 ForcedSeed : ForcedOutflowSeeds)
    {
        if (Channel.IsValidIndex(ForcedSeed) && Channel[ForcedSeed])
        {
            Starts.AddUnique(ForcedSeed);
        }
    }
    Starts.Sort([&](int32 A, int32 B) { return Data.FilledHeight[A] > Data.FilledHeight[B]; });

    TSet<int64> UsedEdges;
    TArray<FRiverCandidate> Candidates;
    // Stopping at every confluence/breakpoint is correct for graph topology,
    // but two breakpoints can sit right next to each other (a confluence
    // immediately followed by another confluence, or by a lake-adjacent
    // cell), producing a degenerate connector reach only a cell or two long.
    // Later smoothing/resampling/simplification can collapse a reach that
    // short to nothing, leaving a visible gap in the river even though the
    // underlying drainage network is fully connected end to end. Only honour
    // a breakpoint once a reach has covered a minimum real distance since
    // its own start; a too-short connector instead continues through and is
    // absorbed into a longer, reliably renderable reach. UsedEdges still
    // prevents the passed-through breakpoint's own Start entry from
    // re-rendering the same ground: it is left with zero new edges and
    // dropped by the Cells.Num() >= 2 check below.
    const double MinimumReachLength = FMath::Max(
        Data.CellSize * 2.0, FeaturePointSpacing * 1.5
    );
    for (int32 Start : Starts)
    {
        FRiverCandidate CandidateReach;
        if (ForcedSeedSet.Contains(Start))
        {
            FVector2D ShorePoint;
            FindClosestLakeShorePoint(
                Data, Data.CellPosition(Start), Data.CellSize * 2.5,
                CandidateReach.StartLakeIndex, ShorePoint
            );
        }
        int32 Cell = Start;
        double AccumulatedLength = 0.0;
        for (int32 Guard = 0; Guard < Data.Height.Num(); ++Guard)
        {
            CandidateReach.Cells.Add(Cell);
            const int32 Receiver = PrimaryReceiver(Data, Cell);
            if (Receiver != INDEX_NONE && Data.LakeIndex.IsValidIndex(Receiver) &&
                Data.LakeIndex[Receiver] != INDEX_NONE)
            {
                CandidateReach.EndLakeIndex = Data.LakeIndex[Receiver];
            }
            if (Receiver == INDEX_NONE || !Channel[Receiver])
            {
                break;
            }
            const int64 EdgeKey = (static_cast<int64>(Cell) << 32) | static_cast<uint32>(Receiver);
            if (UsedEdges.Contains(EdgeKey))
            {
                break;
            }
            UsedEdges.Add(EdgeKey);
            AccumulatedLength += FVector2D::Distance(
                Data.CellPosition(Cell), Data.CellPosition(Receiver)
            );
            Cell = Receiver;
            if (Data.LakeIndex.IsValidIndex(Cell) && Data.LakeIndex[Cell] != INDEX_NONE)
            {
                break;
            }
            if (UpstreamCount[Cell] != 1 && AccumulatedLength >= MinimumReachLength)
            {
                CandidateReach.Cells.Add(Cell);
                break;
            }
        }
        if (CandidateReach.Cells.Num() >= 2)
        {
            const int32 EndCell = CandidateReach.Cells.Last();
            CandidateReach.Score = CandidateReach.Cells.Num() * Data.CellSize *
                FMath::Sqrt(FMath::Max(0.01, Data.AccumulationD8[EndCell])) *
                (1.0 + 0.35 * FMath::Clamp(Data.Slope[Start] / 0.12, 0.0, 1.0));
            Candidates.Add(MoveTemp(CandidateReach));
        }
    }
    Candidates.Sort([](const FRiverCandidate& A, const FRiverCandidate& B) { return A.Score > B.Score; });
    const int32 ReachCount = FMath::Min(MaximumReaches, Candidates.Num());
    for (int32 ReachIndex = 0; ReachIndex < ReachCount; ++ReachIndex)
    {
        const FRiverCandidate& CandidateReach = Candidates[ReachIndex];
        TArray<FVector> Points;
        Points.Reserve(CandidateReach.Cells.Num());
        double MeanSlope = 0.0;
        double MeanResistance = 0.0;
        double MeanDesert = 0.0;
        double MeanMoisture = 0.0;
        double MeanMountain = 0.0;
        for (int32 Cell : CandidateReach.Cells)
        {
            const FVector2D Position = Data.CellPosition(Cell);
            Points.Emplace(Position.X, Position.Y, Data.FilledHeight[Cell]);
            MeanSlope += Data.Slope[Cell];
            MeanResistance += Data.Resistance.IsValidIndex(Cell) ? Data.Resistance[Cell] : 0.0;
            MeanDesert += Data.DesertMask.IsValidIndex(Cell)
                ? Data.DesertMask[Cell] : 0.0;
            MeanMoisture += Data.Moisture.IsValidIndex(Cell)
                ? Data.Moisture[Cell] : 0.5;
            MeanMountain += Data.MountainMask.IsValidIndex(Cell)
                ? Data.MountainMask[Cell] : 0.0;
        }
        MeanSlope /= CandidateReach.Cells.Num();
        MeanResistance /= CandidateReach.Cells.Num();
        MeanDesert /= CandidateReach.Cells.Num();
        MeanMoisture /= CandidateReach.Cells.Num();
        MeanMountain /= CandidateReach.Cells.Num();

        const int32 EndCell = CandidateReach.Cells.Last();
        const double Area = Data.AccumulationD8[EndCell];
        const double RiverAlpha = DrainageScaleAlpha(Area, MainRiverArea);
        // Hydrological receiver cells are the authoritative route. On steeper
        // reaches, smoothing is suppressed so the visual spline cannot cut
        // sideways across the valley wall.
        // D8 supplies topology, not final spline geometry. Round its eight-
        // direction staircase before adding meander; a single Chaikin pass
        // left alternate grid steps visibly sawtoothing at analysis-cell
        // scale on long reaches.
        Points = ChaikinSmooth(Points, false, 2);
        Points = ResamplePolyline(Points, Data.CellSize * 0.80, false);
        // A hard clamp to exactly 0 at a 12% grade gave every reach through
        // typically-mountainous terrain zero meander whatsoever - not just
        // reduced, mathematically zero, since Lowland multiplies straight
        // into AddBroadMeanders' Amplitude. Widened and floored so a steep
        // reach still gets a small residual wiggle instead of a perfectly
        // straight line; genuinely torrential terrain still tapers hard.
        const double LowlandFraction = FMath::Clamp(
            1.0 - MeanSlope / 0.22, 0.12, 1.0
        );
        const double ValleyFreedom = FMath::Clamp(
            (1.0 - MeanResistance * 0.72)
                * (1.0 - MeanMountain * 0.62),
            0.18,
            1.0
        );
        const TArray<FVector> BaseMeanderPoints = Points;
        AddBroadMeanders(
            Points,
            Data.CellSize,
            MeanderStrength,
            LowlandFraction,
            RiverAlpha,
            ValleyFreedom,
            Seed ^ (ReachIndex * 0x45D9F3B),
            Data.Bounds
        );
        ConstrainMeandersToValley(
            Data, BaseMeanderPoints, Points, LowlandFraction
        );
        SuppressPathologicalMeanders(
            BaseMeanderPoints, Points, Data.CellSize
        );
        const TArray<FVector> PreFinalSmoothPoints = Points;
        const int32 FinalSmoothIterations = MeanSlope > 0.035
            ? 1 : (MeanSlope > 0.018 ? 1 : 2);
        Points = ChaikinSmooth(Points, false, FinalSmoothIterations);
        // Chaikin changes the point count, so resample both lines to a common
        // count. Do not run the discrete lateral valley search again here:
        // that point-by-point resnap was reintroducing the D8 staircase we
        // had just smoothed. Instead, only pull a curved point back toward
        // the proven valley route when it climbs materially above it.
        TArray<FVector> FinalConstraintBase = ResamplePolyline(
            PreFinalSmoothPoints, Data.CellSize * 0.70, false
        );
        Points = ResamplePolyline(Points, Data.CellSize * 0.70, false);
        if (FinalConstraintBase.Num() == Points.Num())
        {
            const double AllowedRise = FMath::Max(
                75.0, Data.CellSize * FMath::Lerp(0.018, 0.045, LowlandFraction)
            );
            const double FullCorrectionRise = FMath::Max(
                AllowedRise + 1.0, Data.CellSize * 0.16
            );
            for (int32 PointIndex = 1;
                 PointIndex + 1 < Points.Num(); ++PointIndex)
            {
                const FVector2D CurvedPosition(Points[PointIndex]);
                const FVector2D ValleyPosition(FinalConstraintBase[PointIndex]);
                const double CurvedHeight = Data.SampleGrid(
                    Data.Height, CurvedPosition
                );
                const double ValleyHeight = Data.SampleGrid(
                    Data.Height, ValleyPosition
                );
                const double PullBack = Smooth01(FMath::Clamp(
                    (CurvedHeight - ValleyHeight - AllowedRise)
                        / (FullCorrectionRise - AllowedRise),
                    0.0, 1.0
                ));
                Points[PointIndex].X = FMath::Lerp(
                    Points[PointIndex].X,
                    FinalConstraintBase[PointIndex].X,
                    PullBack * 0.82
                );
                Points[PointIndex].Y = FMath::Lerp(
                    Points[PointIndex].Y,
                    FinalConstraintBase[PointIndex].Y,
                    PullBack * 0.82
                );
            }
        }
        // One light finishing pass blends any guarded points back into the
        // curve without moving the route far enough to cross a valley wall.
        Points = ChaikinSmooth(Points, false, 1);
        Points = ResamplePolyline(
            Points,
            FMath::Clamp(FeaturePointSpacing, 100.0, Data.CellSize),
            false
        );
        CutOffSelfIntersections(Points);
        for (FVector& Point : Points)
        {
            const FVector2D Position(Point);
            Point.Z = Data.SampleGrid(Data.Height, Position);
        }
        EnforceDownhill(Points);

        if (Data.Lakes.IsValidIndex(CandidateReach.StartLakeIndex))
        {
            int32 IgnoredLakeIndex = INDEX_NONE;
            FVector2D ShorePoint;
            if (FindClosestLakeShorePoint(
                Data, FVector2D(Points[0]), Data.CellSize * 2.5,
                IgnoredLakeIndex, ShorePoint) && IgnoredLakeIndex == CandidateReach.StartLakeIndex)
            {
                Points[0].X = ShorePoint.X;
                Points[0].Y = ShorePoint.Y;
            }
        }
        if (Data.Lakes.IsValidIndex(CandidateReach.EndLakeIndex))
        {
            int32 IgnoredLakeIndex = INDEX_NONE;
            FVector2D ShorePoint;
            if (FindClosestLakeShorePoint(
                Data, FVector2D(Points.Last()), Data.CellSize * 2.5,
                IgnoredLakeIndex, ShorePoint) && IgnoredLakeIndex == CandidateReach.EndLakeIndex)
            {
                Points.Last().X = ShorePoint.X;
                Points.Last().Y = ShorePoint.Y;
            }
        }
        FRiverReach River;
        River.Points = MoveTemp(Points);
        River.DrainageArea = Area;
        River.StartLakeIndex = CandidateReach.StartLakeIndex;
        River.EndLakeIndex = CandidateReach.EndLakeIndex;
        River.Width = FMath::Lerp(HeadwaterWidth, MainRiverWidth, RiverAlpha);
        River.Depth = FMath::Lerp(FMath::Max(120.0, MaximumDepth * 0.12), MaximumDepth, RiverAlpha);
        River.ValleyHalfWidth = FMath::Lerp(HeadwaterValleyWidth, MainValleyWidth, RiverAlpha);
        // Erosion has already created the broad valley. Ordinary active rivers
        // only add modest channel/bank relief; canyon classification below is
        // the only path to large cliff-sided incision.
        River.ValleyDepth = FMath::Lerp(
            River.Depth * 0.45,
            FMath::Min(MaximumValleyDepth * 0.24, River.Depth * 0.90),
            RiverAlpha
        );
        River.ChannelSteepness = ChannelSteepness;
        const double SteepValley = Smooth01(FMath::Clamp(
            (MeanSlope - 0.025) / 0.10, 0.0, 1.0
        ));
        River.ValleyHalfWidth *= FMath::Lerp(1.0, 0.36, SteepValley);
        River.ValleyDepth *= FMath::Lerp(1.0, 0.76, SteepValley);
        const double HumidValley = Smooth01(FMath::Clamp(
            (MeanMoisture - 0.48) / 0.42, 0.0, 1.0
        ));
        River.ValleyHalfWidth *= FMath::Lerp(0.96, 1.18, HumidValley);
        River.ValleyDepth *= FMath::Lerp(0.94, 1.08, HumidValley);
        // bCanyons is currently always enabled (resolved unconditionally
        // true, with no exposed toggle to turn it off), so the old
        // "bCanyons || bClimateCanyon" OR made the desert check redundant -
        // any reach that was steep and resistant enough got canyon-carved
        // regardless of climate, including ordinary hilly/temperate rivers.
        // Real mesas and canyons are a hot/dry-climate landform, so require
        // both the feature toggle AND real desert climate here.
        const bool bClimateCanyon = MeanDesert > 0.42;
        if (bCanyons && bClimateCanyon
            && Area >= CanyonStartArea
            && MeanSlope > 0.045
            && MeanResistance > 0.22)
        {
            River.ValleyHalfWidth *= FMath::Lerp(0.75, 0.42, MeanResistance);
            River.ValleyDepth = FMath::Max(River.ValleyDepth, MaximumValleyDepth * FMath::Lerp(1.1, 1.5, MeanResistance));
            River.CrossSectionExponent = FMath::Lerp(0.9, 0.55, MeanResistance);
            River.bIsCanyon = true;
        }
        else
        {
            River.CrossSectionExponent = FMath::Lerp(1.65, 0.82, FMath::Clamp(MeanSlope / 0.16, 0.0, 1.0));
        }
        for (const FVector& Point : River.Points)
        {
            River.Bounds += FVector2D(Point);
        }
        River.Bounds = River.Bounds.ExpandBy(River.ValleyHalfWidth);
        Data.Rivers.Add(MoveTemp(River));
    }

    // This runs after all the meander/valley-following work above, on the
    // final points that decide what the water spline actually looks like -
    // at FeaturePointSpacing*4 (20m with the 5m default) it was routinely
    // larger than the real curvature it was supposed to just be thinning.
    // Douglas-Peucker keeps a bend's peak only if it deviates from the
    // straight chord by more than the tolerance, so any valley bend or
    // meander with under ~20m of perpendicular deflection - common for a
    // moderate mountain reach even where the underlying terrain genuinely
    // curves - was being flattened into dead-straight segments between
    // widely-spaced points. Shrunk so it only removes truly redundant
    // near-collinear points, not the curvature the rest of this function
    // spent its effort building.
    const double SimplificationTolerance = FMath::Max(
        FeaturePointSpacing * 1.2,
        Data.CellSize * 0.035
    );
    for (FRiverReach& River : Data.Rivers)
    {
        AnchorRiverToLakes(
            Data, River.Points, River.StartLakeIndex, River.EndLakeIndex
        );
        River.Points = SimplifyFeaturePolyline(
            River.Points, SimplificationTolerance, false
        );
        CutOffSelfIntersections(River.Points);
        EnforceDownhill(River.Points);
        AddLevelLakeJunctionPads(
            Data, River.Points, River.StartLakeIndex, River.EndLakeIndex
        );
        River.Bounds = FBox2D(ForceInit);
        for (const FVector& Point : River.Points)
        {
            River.Bounds += FVector2D(Point);
        }
        River.Bounds = River.Bounds.ExpandBy(River.ValleyHalfWidth);
    }
}
} // namespace UE::Avenor::Strip

double FAvenorStripData::SampleHeight(const FVector2D& Position) const
{
    return SampleGrid(Height, Position);
}

float FAvenorStripData::SampleChannel(FName Channel, const FVector2D& Position) const
{
    using namespace UE::Avenor::Strip;
    if (Channel == ElevationChannel)
    {
        return static_cast<float>(FMath::Clamp(
            (SampleHeight(Position) - Bounds.Min.Z) / FMath::Max(1.0, Bounds.GetSize().Z), 0.0, 1.0
        ));
    }
    if (Channel == SlopeChannel)
    {
        return static_cast<float>(FMath::Clamp(SampleGrid(Slope, Position) / 0.35, 0.0, 1.0));
    }
    if (Channel == WetnessChannel)
    {
        return static_cast<float>(FMath::Clamp(FMath::Loge(1.0 + SampleGrid(Accumulation, Position)) / 6.0, 0.0, 1.0));
    }
    if (Channel == MountainChannel)
    {
        return static_cast<float>(FMath::Clamp(SampleGrid(MountainMask, Position), 0.0, 1.0));
    }
    if (Channel == HillChannel)
    {
        return static_cast<float>(FMath::Clamp(SampleGrid(HillMask, Position), 0.0, 1.0));
    }
    if (Channel == DesertChannel)
    {
        return static_cast<float>(FMath::Clamp(SampleGrid(DesertMask, Position), 0.0, 1.0));
    }
    if (Channel == PlainsChannel)
    {
        return static_cast<float>(FMath::Clamp(SampleGrid(PlainsMask, Position), 0.0, 1.0));
    }
    if (Channel == LakeChannel)
    {
        for (const FLakeBasin& Lake : Lakes)
        {
            if (Lake.Bounds.IsInside(Position) && IsInsidePolygon(Position, Lake.Shoreline))
            {
                return 1.0f;
            }
        }
        return 0.0f;
    }
    if (Channel == RiverChannel)
    {
        for (const FRiverReach& River : Rivers)
        {
            if (!River.Bounds.IsInside(Position))
            {
                continue;
            }
            for (int32 Index = 0; Index + 1 < River.Points.Num(); ++Index)
            {
                const double Distance = SegmentDistance(Position, FVector2D(River.Points[Index]), FVector2D(River.Points[Index + 1]));
                if (Distance <= River.ValleyHalfWidth)
                {
                    return static_cast<float>(1.0 - Distance / River.ValleyHalfWidth);
                }
            }
        }
        return 0.0f;
    }
    return 0.0f;
}

namespace UE::Avenor::Strip
{
static double EvaluateLandform(
    const FVector2D& Position,
    int32 Seed,
    double ReliefHeight,
    double StructuralScale,
    double TectonicActivity,
    double RiftStrength,
    bool bClimateEnabled,
    double ClimateTemperature,
    double ClimateMoisture,
    double& OutResistance,
    double& OutMountainMask,
    double& OutHillMask,
    double& OutDesertMask,
    double& OutPlainsMask
)
{
    const double Relief = FMath::Max(25000.0, ReliefHeight);
    // StructuralScale arrives already resolved (explicit or auto-derived from
    // world size) in ResolveSettings(). Only guard against degenerate values
    // here - do not re-derive from world bounds, or an explicit user choice
    // would get silently overridden a second time.
    const double Scale = FMath::Clamp(StructuralScale, 100000.0, 5000000.0);
    const double Activity = FMath::Clamp(TectonicActivity, 0.0, 1.0);
    const double RiftAmount = FMath::Clamp(RiftStrength, 0.0, 1.0);

    // Computed early (needs only the climate inputs, not any of the terrain
    // fields below) so it can also shape mountain crest character further
    // down, not just the desert/dune/mesa terms it originally only fed.
    // Desert/mesa/canyon character belongs to genuinely hot-dry climate, not
    // merely warm-dry (that reads as savannah/scrubland instead). Ramp this
    // sharply through the warm->hot boundary rather than starting just above
    // the cold/temperate line, so warm-dry areas stay savannah-like and only
    // deep-hot-dry areas get full desert/resistant-cap/canyon treatment.
    const double HotFraction = bClimateEnabled
        ? Smooth01(FMath::Clamp(
            (ClimateTemperature - 0.58) / 0.30, 0.0, 1.0
        )) : 0.0;
    const double Dryness = bClimateEnabled
        ? Smooth01(FMath::Clamp(
            (0.58 - ClimateMoisture) / 0.48, 0.0, 1.0
        )) : 0.0;
    const double Aridity = HotFraction * Dryness;

    const FVector2D SeedOffset(
        static_cast<double>((Seed * 92821) & 0x7ffff),
        static_cast<double>((Seed * 68917) & 0x7ffff)
    );
    auto AvoidCardinalAlignment = [](double Angle)
    {
        double Wrapped = FMath::Fmod(Angle, PI * 0.5);
        if (Wrapped < 0.0)
        {
            Wrapped += PI * 0.5;
        }
        const double CardinalDistance = FMath::Min(Wrapped, PI * 0.5 - Wrapped);
        return FMath::Fmod(
            Angle + (CardinalDistance < 0.14 ? 0.19 : 0.0), PI
        );
    };
    const double RawSeedAngleA = FMath::Fmod(
        FMath::Abs(static_cast<double>(Seed)) * 0.000913 + 0.37, PI
    );
    const double RawSeedAngleB = FMath::Fmod(
        RawSeedAngleA + 1.047
            + FMath::Abs(static_cast<double>(Seed)) * 0.000217,
        PI
    );
    const double RawSeedAngleC = FMath::Fmod(
        RawSeedAngleB + 0.731
            + FMath::Abs(static_cast<double>(Seed)) * 0.000137,
        PI
    );
    const double SeedAngleA = AvoidCardinalAlignment(RawSeedAngleA);
    const double SeedAngleB = AvoidCardinalAlignment(RawSeedAngleB);
    const double SeedAngleC = AvoidCardinalAlignment(RawSeedAngleC);
    const FVector2D AxisA(FMath::Cos(SeedAngleA), FMath::Sin(SeedAngleA));
    const FVector2D AxisB(FMath::Cos(SeedAngleB), FMath::Sin(SeedAngleB));
    const FVector2D AxisC(FMath::Cos(SeedAngleC), FMath::Sin(SeedAngleC));
    const FVector2D AcrossA = Rotate90(AxisA);
    const FVector2D AcrossB = Rotate90(AxisB);
    const FVector2D AcrossC = Rotate90(AxisC);

    auto Oriented = [](const FVector2D& P,
                       const FVector2D& Along,
                       const FVector2D& Across,
                       double AlongStretch,
                       double AcrossStretch)
    {
        return FVector2D(
            FVector2D::DotProduct(P, Along) / FMath::Max(0.05, AlongStretch),
            FVector2D::DotProduct(P, Across) / FMath::Max(0.05, AcrossStretch)
        );
    };

    const FVector2D MacroWarp(
        Fbm(Position, Scale * 1.8,
            SeedOffset + FVector2D(137.0, 911.0), 4, 0.56, 2.0),
        Fbm(Position, Scale * 1.7,
            SeedOffset + FVector2D(733.0, 271.0), 4, 0.56, 2.03)
    );
    const FVector2D Warped = Position + MacroWarp * Scale * 0.28;

    const double Province = 0.5 + 0.5 * Fbm(
        Warped, Scale * 1.55,
        SeedOffset + FVector2D(1901.0, 331.0),
        4, 0.57, 2.0
    );
    const double Province2 = 0.5 + 0.5 * Fbm(
        Warped, Scale * 1.15,
        SeedOffset + FVector2D(433.0, 1777.0),
        4, 0.55, 2.07
    );
    const double MountainProvince = Smooth01(FMath::Clamp(
        (Province * 0.62 + Province2 * 0.38 - 0.55) / 0.31,
        0.0, 1.0
    ));
    const double HillProvince = Smooth01(FMath::Clamp(
        (Province * 0.42 + Province2 * 0.58 - 0.35) / 0.44,
        0.0, 1.0
    )) * (1.0 - MountainProvince * 0.46);
    const double QuietProvince = FMath::Clamp(
        1.0 - MountainProvince * 0.70 - HillProvince * 0.46,
        0.0, 1.0
    );

    const double PlateField = Fbm(
        Warped, Scale * 2.6,
        SeedOffset + FVector2D(1201.0, 331.0),
        5, 0.57, 2.0
    );
    const FVector2D StructureWarp(
        Fbm(Warped, Scale * 2.1,
            SeedOffset + FVector2D(229.0, 1871.0), 3, 0.57, 2.0),
        Fbm(Warped, Scale * 2.0,
            SeedOffset + FVector2D(1619.0, 503.0), 3, 0.57, 2.0)
    );
    const FVector2D RegionalWarped = Warped + StructureWarp * Scale * 0.24;
    const double RegionalA = Fbm(
        Oriented(RegionalWarped, AxisC, AcrossC, 2.7, 0.95),
        Scale * 3.0,
        SeedOffset + FVector2D(149.0, 1267.0),
        4, 0.55, 2.0
    );
    const double RegionalB = Fbm(
        Oriented(RegionalWarped, AxisA, AcrossA, 2.2, 1.10),
        Scale * 2.55,
        SeedOffset + FVector2D(2011.0, 317.0),
        4, 0.55, 2.03
    );
    const double RegionalStructure = FMath::Clamp(
        RegionalA * 0.62 + RegionalB * 0.38, -1.0, 1.0
    );
    const double Uplift = FMath::Clamp(
        PlateField * 0.72 + RegionalStructure * 0.28,
        -1.0, 1.0
    );
    const double PositiveUplift = Smooth01(FMath::Clamp(
        (Uplift + 0.12) / 0.92, 0.0, 1.0
    ));
    const double Subsidence = Smooth01(FMath::Clamp(
        (-Uplift + 0.10) / 0.88, 0.0, 1.0
    ));

    // Bend each geological belt with a genuinely broad, restrained domain
    // warp. A previous version used Scale*0.4 (a shorter wavelength) with an
    // enormous 0.8*Scale displacement and consequently amplified local noise
    // into corrugated relief rather than bending the range at macro scale.
    const FVector2D BeltWarpedA = Warped + AcrossA * Fbm(
        Warped, Scale * 3.8,
        SeedOffset + FVector2D(743.0, 2117.0), 3, 0.57, 2.0
    ) * Scale * 0.32;
    const FVector2D BeltWarpedB = Warped + AcrossB * Fbm(
        Warped, Scale * 3.4,
        SeedOffset + FVector2D(187.0, 2381.0), 3, 0.57, 2.0
    ) * Scale * 0.30;
    const double BeltA = RidgedFbm(
        Oriented(BeltWarpedA, AxisA, AcrossA, 4.2, 0.82),
        Scale * 0.95,
        SeedOffset + FVector2D(421.0, 719.0),
        3
    );
    const double BeltB = RidgedFbm(
        Oriented(BeltWarpedB, AxisB, AcrossB, 3.4, 0.90),
        Scale * 1.05,
        SeedOffset + FVector2D(877.0, 149.0),
        3
    );
    // Select one dominant structural orientation regionally. Adding both
    // ridged fields everywhere makes their crests visibly intersect as an X.
    const double RawBeltSelector = 0.5 + 0.5 * Fbm(
        Warped, Scale * 3.6,
        SeedOffset + FVector2D(3181.0, 1297.0), 3, 0.56, 2.0
    );
    const double BeltSelector = Smooth01(FMath::Clamp(
        (RawBeltSelector - 0.43) / 0.14, 0.0, 1.0
    ));
    const double BeltSignal = FMath::Clamp(
        FMath::Lerp(BeltA, BeltB, BeltSelector), 0.0, 1.0
    );
    double BroadRange = Smooth01(FMath::Clamp(
        (BeltSignal - FMath::Lerp(0.45, 0.35, Activity)) / 0.41,
        0.0, 1.0
    )) * FMath::Lerp(0.50, 1.0, MountainProvince)
       * FMath::Lerp(0.68, 1.0, PositiveUplift);

    // Deliberately no strip-centre or spine term here. Geology is generated
    // independent of the monorail corridor; keeping the corridor buildable
    // is handled later by directly altering the terrain along its path,
    // not by biasing the height field the corridor might run through.
    const double CrestRidges = RidgedFbm(
        Warped, Scale * 0.42,
        SeedOffset + FVector2D(947.0, 271.0),
        3
    );
    const double CrestVariation = 0.5 + 0.5 * Fbm(
        Warped, Scale * 0.42,
        SeedOffset + FVector2D(211.0, 883.0),
        4, 0.54, 2.07
    );
    // Reverted: this session tried a climate-linked crest sharpener here
    // (Coldness, then Coldness+Aridity as "Harshness") plus a secondary
    // ridge/peak-detail layer further down. Neither fixed the reported
    // straight, flat-sided, geometric-looking peaks, and one attempt (a
    // second ridged-noise layer) made it visibly worse. Reverted both back
    // to the original fixed formula rather than keep layering unproven
    // changes on top of a problem that predates this session's edits.
    const double CrestShape = FMath::Clamp(
        0.88
            + (CrestRidges - 0.50) * 0.16
            + (CrestVariation - 0.50) * 0.12,
        0.76, 1.03
    );
    OutMountainMask = FMath::Clamp(BroadRange, 0.0, 1.0);

    const double UplandRidges = RidgedFbm(
        Warped, Scale * 0.62,
        SeedOffset + FVector2D(347.0, 1039.0),
        3
    );
    const double Rolling = Fbm(
        Warped, Scale * 0.62,
        SeedOffset + FVector2D(1171.0, 673.0),
        3, 0.54, 2.03
    );
    const double HillDetail = Fbm(
        Warped, Scale * 0.24,
        SeedOffset + FVector2D(163.0, 1429.0),
        2, 0.52, 2.1
    );
    const double FoothillEnvelope = Smooth01(FMath::Clamp(
        (BeltSignal - 0.38) / 0.38, 0.0, 1.0
    )) * MountainProvince * (1.0 - OutMountainMask * 0.52);
    const double IndependentHills = HillProvince * Smooth01(FMath::Clamp(
        (0.40 * UplandRidges
            + 0.40 * (0.5 + 0.5 * Rolling)
            + 0.20 * Province2 - 0.38) / 0.44,
        0.0, 1.0
    ));
    OutHillMask = FMath::Clamp(
        FMath::Max(FoothillEnvelope * 0.82, IndependentHills)
            * (1.0 - OutMountainMask * 0.62),
        0.0, 1.0
    );

    const FVector2D RiftWarp = Warped + FVector2D(
        Fbm(Warped, Scale * 1.2,
            SeedOffset + FVector2D(509.0, 193.0), 3) * Scale * 0.16,
        Fbm(Warped, Scale * 1.2,
            SeedOffset + FVector2D(827.0, 557.0), 3) * Scale * 0.16
    );
    const double RiftLine = RidgedFbm(
        Oriented(RiftWarp, AxisB, AcrossB, 4.5, 0.72),
        Scale * 0.72,
        SeedOffset + FVector2D(919.0, 1601.0),
        5
    );
    const double RiftContinuity = 0.5 + 0.5 * Fbm(
        Warped, Scale * 1.8,
        SeedOffset + FVector2D(1733.0, 449.0),
        3, 0.58, 2.0
    );
    const double RiftMask = Smooth01(FMath::Clamp(
        (RiftLine * FMath::Lerp(0.55, 1.0, Subsidence)
            * FMath::Lerp(0.65, 1.0, RiftContinuity) - 0.50) / 0.34,
        0.0, 1.0
    )) * RiftAmount * (1.0 - OutMountainMask * 0.72);
    const double RiftShoulder = Smooth01(FMath::Clamp(
        (RiftLine - 0.35) / 0.40, 0.0, 1.0
    )) * (1.0 - RiftMask) * RiftAmount;

    const double BasinNoise = 0.5 + 0.5 * Fbm(
        Warped, Scale * 1.45,
        SeedOffset + FVector2D(1543.0, 271.0),
        4, 0.56, 2.0
    );
    const double BasinMask = Subsidence
        * Smooth01(FMath::Clamp((BasinNoise - 0.46) / 0.36, 0.0, 1.0))
        * (1.0 - OutMountainMask * 0.72);

    OutPlainsMask = FMath::Clamp(
        FMath::Max(QuietProvince, 1.0 - OutMountainMask * 0.96
            - OutHillMask * 0.72 - RiftMask * 0.42),
        0.0, 1.0
    );

    const double RegionalElevation = Fbm(
        RegionalWarped, Scale * 2.35,
        SeedOffset + FVector2D(2273.0, 1291.0),
        4, 0.56, 2.0
    );
    double Height = Relief * 0.060
        + RegionalElevation * Relief * 0.080
        + Uplift * Relief * 0.075
        + RegionalStructure * Relief * 0.025;

    const double MountainHeight = Smooth01(OutMountainMask);
    // Preserve kilometre-scale relief without the self-amplifying term that
    // turned every small mask fluctuation into a large bump and pushed strong
    // crests toward the same ceiling.
    const double MountainRelief = FMath::Pow(MountainHeight, 1.16);
    Height += MountainRelief * Relief
        * FMath::Lerp(0.62, 1.02, Activity)
        * FMath::Lerp(0.90, 1.10, CrestVariation)
        * CrestShape;

    // Reverted: two attempts this session (a ridged secondary-ridge layer,
    // then a plain-Fbm replacement) tried adding extra height detail here
    // to break up the mountain belt's smooth cross-section. Neither was
    // confirmed to fix the reported straight, flat-sided peaks, and the
    // first made it visibly worse (a geometric "X" crease). Removed rather
    // than keep stacking unproven changes on this code path.

    Height += FoothillEnvelope * Relief * 0.18
        * (0.72 + UplandRidges * 0.28);
    // Humid hill country should be broad and rounded, not globally rougher.
    // Rolling controls the macro undulation; short detail is deliberately
    // small and fades further in moist climates.
    const double HillDetailScale = bClimateEnabled
        ? FMath::Lerp(1.0, 0.45, ClimateMoisture)
        : 0.70;
    Height += IndependentHills * Relief * (
        0.060
        + (0.5 + 0.5 * Rolling) * 0.075
        + UplandRidges * 0.025
        + HillDetail * 0.008 * HillDetailScale
    );

    Height -= RiftMask * Relief * FMath::Lerp(0.12, 0.30, RiftAmount)
        * FMath::Lerp(0.86, 1.08, RiftLine);
    Height += RiftShoulder * Relief * 0.045;
    Height -= BasinMask * Relief * 0.085;

    const double PlainRoll = Fbm(
        Warped, Scale * 1.35,
        SeedOffset + FVector2D(101.0, 43.0),
        3, 0.56, 2.0
    );
    Height += OutPlainsMask * Relief * PlainRoll * 0.014;

    OutDesertMask = Aridity * FMath::Clamp(
        1.0 - OutMountainMask * 0.38, 0.0, 1.0
    );

    const double Lithology = 0.5 + 0.5 * Fbm(
        Warped, FMath::Max(90000.0, Scale * 0.22),
        SeedOffset + FVector2D(811.0, 397.0),
        4, 0.58, 2.0
    );
    // Gating this to Hill/Foothill alone left arid true mountains with no
    // shape distinction at all from any other mountain - they got the
    // correct Desert biome label and colour, but the actual relief was
    // identical to a wet temperate range, because the differential-
    // weathering escarpment/mesa erosion this drives (EvolveTerrainFromDra
    // inage's CanyonSuitability and residual-mesa terms already multiply in
    // Mountain, but only ever saw it through OutResistance, which this was
    // the only real source of on non-hill terrain) never had anywhere to
    // engage on a mountain's main mass. Real hot deserts have canyon-cut,
    // mesa-and-butte mountain terrain, not lush rounded ranges.
    const double ResistantCap = OutDesertMask
        * FMath::Clamp(
            OutHillMask + FoothillEnvelope * 0.45 + OutMountainMask * 0.85,
            0.0, 1.0
        )
        * Smooth01(FMath::Clamp((Lithology - 0.48) / 0.38, 0.0, 1.0));

    // Real dune fields aren't confined to dead-flat basins - gentle desert
    // hillslopes (ergs climbing a bajada, not just pan-flat sand seas) carry
    // dune texture too, just fading out as slope/relief increases.
    const double DuneSuitability = OutDesertMask
        * FMath::Clamp(OutPlainsMask + OutHillMask * 0.35, 0.0, 1.0)
        * (1.0 - ResistantCap * 0.85)
        * (1.0 - RiftMask * 0.55);
    if (DuneSuitability > 0.03)
    {
        const double WindAngle = FMath::Fmod(
            FMath::Abs(static_cast<double>(Seed)) * 0.000731 + 0.83,
            PI
        );
        const FVector2D WindDirection(
            FMath::Cos(WindAngle), FMath::Sin(WindAngle)
        );
        const double DuneWavelength = FMath::Clamp(
            Scale * 0.07, 45000.0, 160000.0
        );
        const double PhaseWarp = Fbm(
            Warped, DuneWavelength * 5.2,
            SeedOffset + FVector2D(1877.0, 1021.0),
            3, 0.55, 2.0
        ) * DuneWavelength * 0.70;
        const double WindCoordinate =
            FVector2D::DotProduct(Warped, WindDirection) + PhaseWarp;
        const double DuneWave = 0.5 + 0.5 * FMath::Sin(
            2.0 * PI * WindCoordinate / DuneWavelength
        );
        Height += (FMath::Pow(DuneWave, 2.1) - 0.31)
            * Relief * 0.012 * DuneSuitability;
    }

    OutResistance = FMath::Clamp(
        OutMountainMask * 0.24
            + OutHillMask * 0.09
            + OutDesertMask * 0.10
            + ResistantCap * 0.62
            + RiftShoulder * 0.10,
        0.0, 1.0
    );
    return Height;
}
} // namespace UE::Avenor::Strip

using namespace UE::Avenor::Strip;

namespace UE::Avenor::Strip
{
// Shared by BuildMacroClimate (stretches the base regional climate before it
// drives desert/dune terrain shaping) and RefineClimateFromHydrology
// (stretches the final post-lapse-rate, post-hydrology climate before biome
// classification). A percentile remap is monotonic - it cannot reorder
// cells, so a spatial gradient's shape survives - but it expands whatever
// real spread exists in this particular generated world to actually fill
// the target range, instead of leaving it clustered wherever the raw
// formula's one-directional biases or boundary-straddling defaults happen
// to push it.
static double PercentileValue(TArray<double> Values, double Percentile)
{
    if (Values.Num() == 0)
    {
        return 0.5;
    }
    Values.Sort();
    const int32 Index = FMath::Clamp(
        FMath::RoundToInt(Percentile * static_cast<double>(Values.Num() - 1)),
        0, Values.Num() - 1
    );
    return Values[Index];
}

static double StretchToRange(double Value, double Lo, double Hi, double TargetLo, double TargetHi)
{
    if (Hi - Lo < 1e-4)
    {
        return FMath::Clamp((TargetLo + TargetHi) * 0.5, 0.0, 1.0);
    }
    const double Alpha = (Value - Lo) / (Hi - Lo);
    return FMath::Clamp(FMath::Lerp(TargetLo, TargetHi, Alpha), 0.0, 1.0);
}

static void BuildMacroClimate(
    FAvenorStripData& Data,
    const AAvenorStripTerrainGenerator& Generator
)
{
    const int32 CellCount = Data.Columns * Data.Rows;
    Data.MacroTemperature.Init(0.5, CellCount);
    Data.MacroMoisture.Init(0.5, CellCount);
    Data.Temperature.Init(0.5, CellCount);
    Data.Moisture.Init(0.5, CellCount);
    Data.Runoff.Init(1.0, CellCount);
    Data.Biome.Init(
        static_cast<uint8>(EAvenorBiomeClass::TemperateMoist), CellCount
    );
    if (!Generator.bGenerateClimate || CellCount == 0)
    {
        return;
    }

    const double LongMin = Generator.LongAxis == EAvenorStripLongAxis::X
        ? Data.Bounds.Min.X : Data.Bounds.Min.Y;
    const double LongLength = Generator.LongAxis == EAvenorStripLongAxis::X
        ? Data.Bounds.GetSize().X : Data.Bounds.GetSize().Y;
    const double Spacing = FMath::Max(
        100000.0, Generator.ClimateRegionSpacing
    );
    const int32 AnchorCount = FMath::Max(
        2, FMath::CeilToInt(LongLength / Spacing) + 1
    );

    TArray<double> TemperatureAnchors;
    TArray<double> MoistureAnchors;
    TemperatureAnchors.SetNumUninitialized(AnchorCount);
    MoistureAnchors.SetNumUninitialized(AnchorCount);
    FRandomStream Random(Generator.Seed ^ 0x4a6f7921);
    TemperatureAnchors[0] = FMath::Clamp(
        Generator.ClimateTemperature
            + Random.FRandRange(-0.04f, 0.04f)
                * Generator.ClimateRegionalVariation,
        0.03, 0.97
    );
    MoistureAnchors[0] = FMath::Clamp(
        Generator.ClimateMoisture
            + Random.FRandRange(-0.08f, 0.08f)
                * Generator.ClimateRegionalVariation,
        0.03, 0.97
    );
    double TemperatureTrend = Random.FRandRange(-0.06f, 0.06f);
    double MoistureTrend = Random.FRandRange(-0.10f, 0.10f);
    const double MaximumTemperatureStep = Generator.bShowcaseClimateCompression
        ? 0.22 : 0.11;
    const double ShowcaseVariation = Generator.bShowcaseClimateCompression
        ? 2.2 : 1.0;
    const double ShowcaseMoistureVariation =
        Generator.bShowcaseClimateCompression ? 1.6 : 1.0;
    for (int32 Index = 1; Index < AnchorCount; ++Index)
    {
        TemperatureTrend = FMath::Clamp(
            TemperatureTrend * 0.72
                + Random.FRandRange(-0.055f, 0.055f)
                    * Generator.ClimateRegionalVariation
                    * ShowcaseVariation,
            -MaximumTemperatureStep,
            MaximumTemperatureStep
        );
        MoistureTrend = FMath::Clamp(
            MoistureTrend * 0.55
                + Random.FRandRange(-0.12f, 0.12f)
                    * Generator.ClimateRegionalVariation
                    * ShowcaseMoistureVariation,
            -0.18,
            0.18
        );
        TemperatureAnchors[Index] = FMath::Clamp(
            TemperatureAnchors[Index - 1] + TemperatureTrend,
            0.03, 0.97
        );
        MoistureAnchors[Index] = FMath::Clamp(
            MoistureAnchors[Index - 1] + MoistureTrend,
            0.03, 0.97
        );
    }

    const FVector2D NoiseSeed(
        static_cast<double>(Generator.Seed) * 13.17,
        static_cast<double>(Generator.Seed) * -7.31
    );
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        const FVector2D Position = Data.CellPosition(Cell);
        const double LongPosition = Generator.LongAxis
            == EAvenorStripLongAxis::X ? Position.X : Position.Y;
        const double AnchorPosition = FMath::Clamp(
            (LongPosition - LongMin) / Spacing,
            0.0,
            static_cast<double>(AnchorCount - 1)
        );
        const int32 A = FMath::Min(
            FMath::FloorToInt(AnchorPosition), AnchorCount - 2
        );
        const double Alpha = Smooth01(AnchorPosition - A);
        const double MacroTemperature = FMath::Lerp(
            TemperatureAnchors[A], TemperatureAnchors[A + 1], Alpha
        );
        const double MacroMoisture = FMath::Lerp(
            MoistureAnchors[A], MoistureAnchors[A + 1], Alpha
        );
        const double TemperatureNoise = Fbm(
            Position, Spacing * 2.4, NoiseSeed + FVector2D(193.0, 811.0),
            3, 0.5, 2.0
        );
        const double MoistureNoise = Fbm(
            Position, Spacing * 1.7, NoiseSeed + FVector2D(619.0, 277.0),
            3, 0.55, 2.0
        );
        const double Temperature = FMath::Clamp(
            MacroTemperature
                + TemperatureNoise
                    * Generator.ClimateRegionalVariation * 0.07,
            0.0, 1.0
        );
        const double Precipitation = FMath::Clamp(
            MacroMoisture
                + MoistureNoise
                    * Generator.ClimateRegionalVariation * 0.14,
            0.0, 1.0
        );
        Data.MacroTemperature[Cell] = Temperature;
        Data.MacroMoisture[Cell] = Precipitation;
    }

    // This regional climate is what EvaluateLandform reads to decide where
    // terrain actually earns desert/dune character (via HotFraction/Dryness
    // thresholds around 0.58) - not just what colours the biome map later.
    // With anchors centred on the ClimateTemperature/ClimateMoisture
    // sliders (0.5/0.5 by default) and only modest per-anchor drift, the
    // raw values rarely reach those thresholds anywhere on a given world,
    // so terrain shape stayed geologically uniform regardless of the
    // labelled biome. Stretch to this world's own 10th/90th percentile
    // spread so real dune deserts, and later real cold/hot classification,
    // actually appear somewhere instead of the range never reaching the
    // shaping thresholds at all.
    const double TemperatureLowPercentile = PercentileValue(Data.MacroTemperature, 0.10);
    const double TemperatureHighPercentile = PercentileValue(Data.MacroTemperature, 0.90);
    const double MoistureLowPercentile = PercentileValue(Data.MacroMoisture, 0.10);
    const double MoistureHighPercentile = PercentileValue(Data.MacroMoisture, 0.90);
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        const double Temperature = StretchToRange(
            Data.MacroTemperature[Cell],
            TemperatureLowPercentile, TemperatureHighPercentile,
            0.05, 0.95
        );
        const double Precipitation = StretchToRange(
            Data.MacroMoisture[Cell],
            MoistureLowPercentile, MoistureHighPercentile,
            0.05, 0.95
        );
        Data.MacroTemperature[Cell] = Temperature;
        Data.MacroMoisture[Cell] = Precipitation;
        Data.Temperature[Cell] = Temperature;
        Data.Moisture[Cell] = Precipitation;
        Data.Runoff[Cell] = FMath::Clamp(
            0.28 + Precipitation * 1.45 - Temperature * 0.22,
            0.08, 1.75
        );
    }
}

static void ApplyTerrainClimate(
    FAvenorStripData& Data,
    const AAvenorStripTerrainGenerator& Generator
)
{
    const int32 CellCount = Data.Columns * Data.Rows;
    if (!Generator.bGenerateClimate
        || Data.Temperature.Num() != CellCount
        || Data.Moisture.Num() != CellCount
        || Data.Runoff.Num() != CellCount)
    {
        return;
    }

    // Rain shadow: march a short ray upwind of each cell and find the
    // highest terrain crossed. A high barrier upwind means the prevailing
    // wind already dropped its moisture climbing that ridge, so this cell is
    // in its lee and drier than the regional baseline; a cell that is itself
    // rising above its immediate upwind neighbour is on the windward face
    // catching that lift, so it is wetter. This is a strip world with no
    // ocean/trade-wind simulation, so this is deliberately a simple
    // terrain-silhouette approximation, not an atmospheric model.
    const bool bRainShadow = Generator.RainShadowStrength > 0.0;
    const double WindAngleRadians = FMath::DegreesToRadians(
        Generator.PrevailingWindDirectionDegrees
    );
    const FVector2D UpwindDirection(
        -FMath::Cos(WindAngleRadians), -FMath::Sin(WindAngleRadians)
    );
    const double RainShadowSearchDistance = FMath::Clamp(
        Generator.StructuralScale * 2.2, 400000.0, 6000000.0
    );
    constexpr int32 RainShadowSamples = 7;

    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        // Always derive local climate from the regional baseline so this pass
        // can safely be repeated after erosion reshapes the final surface.
        const double RegionalTemperature = Data.MacroTemperature.IsValidIndex(Cell)
            ? Data.MacroTemperature[Cell] : Data.Temperature[Cell];
        const double RegionalMoisture = Data.MacroMoisture.IsValidIndex(Cell)
            ? Data.MacroMoisture[Cell] : Data.Moisture[Cell];
        const double ElevationMetres = FMath::Max(0.0, Data.Height[Cell]) / 100.0;
        const double ElevationFraction = FMath::Clamp(
            ElevationMetres / 3000.0, 0.0, 1.5
        );
        const double MountainExposure = FMath::Clamp(
            Data.MountainMask[Cell], 0.0, 1.0
        );
        const double HillExposure = FMath::Clamp(
            Data.HillMask.IsValidIndex(Cell) ? Data.HillMask[Cell] : 0.0,
            0.0, 1.0
        );
        // Lapse-rate cooling from absolute elevation, plus an elevation-
        // independent cooling for exposed mountain/hill terrain (wind
        // exposure, bare rock, thin soil). The latter used to be multiplied
        // by ElevationFraction, which made it nearly inert anywhere below
        // ~1500m regardless of how mountainous the terrain actually was - a
        // steep, rocky mountain shoulder or saddle at a few hundred metres
        // read as "warm" purely because it wasn't the literal summit. Final
        // biome classification below determines the snowline from the
        // resulting temperature rather than a fixed Z.
        Data.Temperature[Cell] = FMath::Clamp(
            RegionalTemperature - ElevationFraction * 0.46
                - MountainExposure * 0.16
                - HillExposure * 0.06,
            0.0, 1.0
        );

        double RainShadowMoisture = 0.0;
        if (bRainShadow)
        {
            const FVector2D Position = Data.CellPosition(Cell);
            double MaxUpwindHeight = Data.Height[Cell];
            double NearUpwindHeight = Data.Height[Cell];
            for (int32 Step = 1; Step <= RainShadowSamples; ++Step)
            {
                const double Distance = RainShadowSearchDistance
                    * (static_cast<double>(Step) / RainShadowSamples);
                const double SampledHeight = Data.SampleGrid(
                    Data.Height, Position + UpwindDirection * Distance
                );
                MaxUpwindHeight = FMath::Max(MaxUpwindHeight, SampledHeight);
                if (Step == 1)
                {
                    NearUpwindHeight = SampledHeight;
                }
            }
            const double BarrierRiseMetres = FMath::Max(
                0.0, MaxUpwindHeight - Data.Height[Cell]
            ) / 100.0;
            const double LeeFactor = Smooth01(FMath::Clamp(
                BarrierRiseMetres / 1200.0, 0.0, 1.0
            ));
            const double WindwardRiseMetres = FMath::Max(
                0.0, Data.Height[Cell] - NearUpwindHeight
            ) / 100.0;
            const double WindwardFactor = Smooth01(FMath::Clamp(
                WindwardRiseMetres / 400.0, 0.0, 1.0
            ));
            RainShadowMoisture = (WindwardFactor * 0.28 - LeeFactor * 0.55)
                * Generator.RainShadowStrength;
        }

        Data.Moisture[Cell] = FMath::Clamp(
            RegionalMoisture
                - ElevationFraction * 0.11
                - MountainExposure * 0.035
                + RainShadowMoisture,
            0.0, 1.0
        );
        Data.Runoff[Cell] = FMath::Clamp(
            0.28 + Data.Moisture[Cell] * 1.45
                - Data.Temperature[Cell] * 0.22,
            0.08, 1.75
        );
    }
}

// Reinterpret the structural masks from the surface that actually survives
// erosion. This is deliberately continuous: it does not place mountains. It
// recognises mountain/upland morphology from prominence, relief, slope and the
// underlying structural support after valleys, rifts and drainage are carved.
static void ReclassifyFinalLandforms(FAvenorStripData& Data)
{
    const int32 CellCount = Data.Columns * Data.Rows;
    if (CellCount <= 0)
    {
        return;
    }
    TArray<double> NewMountain;
    TArray<double> NewHill;
    TArray<double> NewPlains;
    NewMountain.SetNumUninitialized(CellCount);
    NewHill.SetNumUninitialized(CellCount);
    NewPlains.SetNumUninitialized(CellCount);
    const double Radius = FMath::Max(100000.0, Data.CellSize * 24.0);
    static const FVector2D Directions[] = {
        FVector2D(1,0), FVector2D(-1,0), FVector2D(0,1), FVector2D(0,-1),
        FVector2D(0.70710678,0.70710678), FVector2D(-0.70710678,0.70710678),
        FVector2D(0.70710678,-0.70710678), FVector2D(-0.70710678,-0.70710678)
    };
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        const FVector2D Position = Data.CellPosition(Cell);
        const double Height = Data.Height[Cell];
        double LocalMin = Height;
        double LocalMax = Height;
        for (const FVector2D& Direction : Directions)
        {
            for (double Scale : {0.45, 1.0})
            {
                const FVector2D SamplePosition = Position + Direction * Radius * Scale;
                const double SampleHeight = Data.SampleGrid(Data.Height, SamplePosition);
                LocalMin = FMath::Min(LocalMin, SampleHeight);
                LocalMax = FMath::Max(LocalMax, SampleHeight);
            }
        }
        const double Prominence = FMath::Max(0.0, Height - LocalMin);
        const double Relief = FMath::Max(0.0, LocalMax - LocalMin);
        const double ProminenceFactor = Smooth01(FMath::Clamp(
            (Prominence - 25000.0) / 105000.0, 0.0, 1.0));
        const double ReliefFactor = Smooth01(FMath::Clamp(
            (Relief - 40000.0) / 125000.0, 0.0, 1.0));
        const double ElevationFactor = Smooth01(FMath::Clamp(
            (Height - 55000.0) / 145000.0, 0.0, 1.0));
        const double SlopeFactor = Smooth01(FMath::Clamp(
            (Data.Slope[Cell] - 0.018) / 0.12, 0.0, 1.0));
        const double StructuralMountain = FMath::Clamp(Data.MountainMask[Cell], 0.0, 1.0);
        const double StructuralHill = FMath::Clamp(Data.HillMask[Cell], 0.0, 1.0);
        const double EmergentMountain = ProminenceFactor * ReliefFactor
            * FMath::Lerp(0.62, 1.0, FMath::Max(ElevationFactor, SlopeFactor));
        const double Mountain = FMath::Clamp(FMath::Max(
            StructuralMountain * FMath::Lerp(0.58, 1.0, ReliefFactor),
            EmergentMountain
        ), 0.0, 1.0);
        const double HillRelief = Smooth01(FMath::Clamp(
            (Relief - 12000.0) / 65000.0, 0.0, 1.0));
        const double HillProminence = Smooth01(FMath::Clamp(
            (Prominence - 8000.0) / 52000.0, 0.0, 1.0));
        const double Hill = FMath::Clamp(FMath::Max(
            StructuralHill * 0.72, HillRelief * HillProminence * 0.92
        ) * (1.0 - Mountain * 0.78), 0.0, 1.0);
        NewMountain[Cell] = Mountain;
        NewHill[Cell] = Hill;
        NewPlains[Cell] = FMath::Clamp(
            1.0 - Mountain * 0.95 - Hill * 0.72, 0.0, 1.0);
    }
    Data.MountainMask = MoveTemp(NewMountain);
    Data.HillMask = MoveTemp(NewHill);
    Data.PlainsMask = MoveTemp(NewPlains);
}

static void RefineClimateFromHydrology(
    FAvenorStripData& Data,
    const TArray<bool>& RiverNetwork,
    const AAvenorStripTerrainGenerator& Generator
)
{
    const int32 CellCount = Data.Columns * Data.Rows;
    if (!Generator.bGenerateClimate
        || Data.Temperature.Num() != CellCount
        || Data.Moisture.Num() != CellCount
        || Data.Biome.Num() != CellCount)
    {
        return;
    }

    const double FarDistance = TNumericLimits<double>::Max() * 0.25;
    TArray<double> WaterDistance;
    WaterDistance.Init(FarDistance, CellCount);
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        const bool bRiver = RiverNetwork.IsValidIndex(Cell)
            && RiverNetwork[Cell];
        const bool bLake = Data.LakeIndex.IsValidIndex(Cell)
            && Data.LakeIndex[Cell] != INDEX_NONE;
        if (bRiver || bLake)
        {
            WaterDistance[Cell] = 0.0;
        }
    }

    const double Cardinal = Data.CellSize;
    const double Diagonal = Data.CellSize * 1.4142135623730951;
    auto Relax = [&](int32 Cell, int32 Neighbor, double Cost)
    {
        WaterDistance[Cell] = FMath::Min(
            WaterDistance[Cell], WaterDistance[Neighbor] + Cost
        );
    };
    for (int32 Y = 0; Y < Data.Rows; ++Y)
    {
        for (int32 X = 0; X < Data.Columns; ++X)
        {
            const int32 Cell = Data.Index(X, Y);
            if (X > 0) Relax(Cell, Data.Index(X - 1, Y), Cardinal);
            if (Y > 0) Relax(Cell, Data.Index(X, Y - 1), Cardinal);
            if (X > 0 && Y > 0)
                Relax(Cell, Data.Index(X - 1, Y - 1), Diagonal);
            if (X + 1 < Data.Columns && Y > 0)
                Relax(Cell, Data.Index(X + 1, Y - 1), Diagonal);
        }
    }
    for (int32 Y = Data.Rows - 1; Y >= 0; --Y)
    {
        for (int32 X = Data.Columns - 1; X >= 0; --X)
        {
            const int32 Cell = Data.Index(X, Y);
            if (X + 1 < Data.Columns)
                Relax(Cell, Data.Index(X + 1, Y), Cardinal);
            if (Y + 1 < Data.Rows)
                Relax(Cell, Data.Index(X, Y + 1), Cardinal);
            if (X + 1 < Data.Columns && Y + 1 < Data.Rows)
                Relax(Cell, Data.Index(X + 1, Y + 1), Diagonal);
            if (X > 0 && Y + 1 < Data.Rows)
                Relax(Cell, Data.Index(X - 1, Y + 1), Diagonal);
        }
    }

    const double InfluenceDistance = FMath::Max(
        Data.CellSize, Generator.ClimateWaterInfluenceDistance
    );

    // Temperature and moisture up to this point are built from anchors
    // centred on the user's ClimateTemperature/ClimateMoisture sliders
    // (0.5/0.5 by default) and then only ever pushed downward from there -
    // elevation lapse rate, mountain/hill exposure and slope drying all
    // subtract, nothing adds outside a narrow water-proximity boost. With
    // defaults that sit exactly on a classification boundary and terrain
    // that is mostly hills/mountains, that one-directional bias collapses
    // almost the entire map into a single quadrant (observed: "Temperate
    // Dry" everywhere on a 30km world) even though the macro anchors and
    // per-cell noise really do vary across the map - the raw values just
    // never spread far enough from the boundary to cross it. Stretch both
    // fields by their own actual distribution across this generated world
    // (a percentile remap, not a fixed formula) so whatever real spread
    // exists - however small in absolute terms - is expanded to use the
    // full classification range. This preserves the spatial gradient (a
    // monotonic remap cannot reorder cells, so cold stays colder than warm,
    // wet stays wetter than dry) while guaranteeing the biome palette
    // actually varies instead of saturating to one bucket.
    TArray<double> RawAvailableMoisture;
    RawAvailableMoisture.SetNumUninitialized(CellCount);
    TArray<double> WaterProximityByCell;
    WaterProximityByCell.SetNumUninitialized(CellCount);
    TArray<double> FlowMoistureByCell;
    FlowMoistureByCell.SetNumUninitialized(CellCount);
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        const double WaterProximity = Smooth01(
            1.0 - WaterDistance[Cell] / InfluenceDistance
        );
        const double FlowMoisture = FMath::Clamp(
            FMath::Loge(1.0 + FMath::Max(0.0, Data.Accumulation[Cell]))
                / 7.0,
            0.0, 1.0
        );
        const double SlopeDrying = FMath::Clamp(
            Data.Slope[Cell] / 0.32, 0.0, 1.0
        ) * 0.24;
        WaterProximityByCell[Cell] = WaterProximity;
        FlowMoistureByCell[Cell] = FlowMoisture;
        RawAvailableMoisture[Cell] = FMath::Clamp(
            Data.Moisture[Cell]
                + WaterProximity * Generator.ClimateWaterMoistureBoost
                + FlowMoisture * 0.10
                - SlopeDrying,
            0.0, 1.0
        );
    }

    const double MoistureLowPercentile = PercentileValue(RawAvailableMoisture, 0.10);
    const double MoistureHighPercentile = PercentileValue(RawAvailableMoisture, 0.90);
    const double TemperatureLowPercentile = PercentileValue(Data.Temperature, 0.10);
    const double TemperatureHighPercentile = PercentileValue(Data.Temperature, 0.90);

    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        const double RegionalMoisture = Data.Moisture[Cell];
        const double WaterProximity = WaterProximityByCell[Cell];
        const double FlowMoisture = FlowMoistureByCell[Cell];
        const double AvailableMoisture = StretchToRange(
            RawAvailableMoisture[Cell],
            MoistureLowPercentile, MoistureHighPercentile,
            0.08, 0.92
        );
        Data.Moisture[Cell] = AvailableMoisture;

        EAvenorBiomeClass Biome = EAvenorBiomeClass::TemperateMoist;
        const double Temperature = StretchToRange(
            Data.Temperature[Cell],
            TemperatureLowPercentile, TemperatureHighPercentile,
            0.08, 0.92
        );
        Data.Temperature[Cell] = Temperature;
        // Correction: an earlier pass removed Alpine/Snow from here on the
        // theory that AvenorBiomeBlendMap's local-override bake was the
        // sole, correct place for that distinction. That bake has since
        // been found to be the actual bug (see AvenorBiomeBlendMap.cpp) -
        // it was independently re-deriving local climate from raw elevation
        // and clobbering the correct values on every asset save.
        // BuildClimateTextureTiles' WorldLocalBiomePixels bake (this file)
        // reads Data.Biome directly and specifically checks for SnowIce/
        // AlpineTundra to decide whether a cell carries a local override at
        // all - removing them here silently broke that path instead of
        // fixing anything. This is the one and only place Alpine/Snow are
        // decided, using the same rain-shadow-aware, percentile-stretched
        // Temperature as everything else in this function.
        const double FinalMountain = FMath::Clamp(
            Data.MountainMask[Cell], 0.0, 1.0
        );
        const double ElevationMetres = FMath::Max(0.0, Data.Height[Cell]) / 100.0;
        const bool bHighTerrain = FinalMountain > 0.28 || ElevationMetres > 1400.0;
        if (Temperature < 0.105 && bHighTerrain)
        {
            Biome = EAvenorBiomeClass::SnowIce;
        }
        else if (Temperature < 0.32 && bHighTerrain)
        {
            Biome = EAvenorBiomeClass::AlpineTundra;
        }
        else if (Temperature >= 0.50
            && Data.DesertMask.IsValidIndex(Cell) && Data.DesertMask[Cell] > 0.40
            && RegionalMoisture < 0.42
            && AvailableMoisture >= 0.48
            && WaterProximity > 0.30)
        {
            Biome = EAvenorBiomeClass::Oasis;
        }
        else if (AvailableMoisture > 0.84
            && Data.Slope[Cell] < 0.04
            && (WaterProximity > 0.25 || FlowMoisture > 0.55))
        {
            Biome = EAvenorBiomeClass::Wetland;
        }
        else
        {
            const bool bMoist = AvailableMoisture >= 0.5;
            if (Temperature < 0.25)
                Biome = bMoist ? EAvenorBiomeClass::ColdMoist
                               : EAvenorBiomeClass::ColdDry;
            else if (Temperature < 0.5)
                Biome = bMoist ? EAvenorBiomeClass::TemperateMoist
                               : EAvenorBiomeClass::TemperateDry;
            else if (Temperature < 0.75)
                Biome = bMoist ? EAvenorBiomeClass::WarmMoist
                               : EAvenorBiomeClass::WarmDry;
            else
                Biome = bMoist ? EAvenorBiomeClass::HotWet
                               : EAvenorBiomeClass::HotDry;
        }
        Data.Biome[Cell] = static_cast<uint8>(Biome);
    }
}

static TSharedPtr<FAvenorStripData> GenerateData(const AAvenorStripTerrainGenerator& Generator)
{
    const FBox Bounds = Generator.GetGenerationBounds();
    if (!Bounds.IsValid)
    {
        return nullptr;
    }
    TSharedPtr<FAvenorStripData> Data = MakeShared<FAvenorStripData>();
    Data->Bounds = Bounds;
    const FVector Size = Bounds.GetSize();
    double CellSize = FMath::Max(2500.0, Generator.AnalysisCellSize);
    int64 Columns = FMath::Max<int64>(2, FMath::CeilToInt(Size.X / CellSize));
    int64 Rows = FMath::Max<int64>(2, FMath::CeilToInt(Size.Y / CellSize));
    const int64 MaximumCells = FMath::Clamp<int64>(Generator.MaximumAnalysisCells, 10000, 2000000);
    if (Columns * Rows > MaximumCells)
    {
        CellSize *= FMath::Sqrt(static_cast<double>(Columns * Rows) / static_cast<double>(MaximumCells));
        Columns = FMath::Max<int64>(2, FMath::CeilToInt(Size.X / CellSize));
        Rows = FMath::Max<int64>(2, FMath::CeilToInt(Size.Y / CellSize));
    }
    Data->Columns = static_cast<int32>(Columns);
    Data->Rows = static_cast<int32>(Rows);
    Data->CellSize = CellSize;
    const int32 CellCount = Data->Columns * Data->Rows;
    Data->Height.SetNumUninitialized(CellCount);
    Data->Resistance.SetNumUninitialized(CellCount);
    Data->MountainMask.SetNumUninitialized(CellCount);
    Data->HillMask.SetNumUninitialized(CellCount);
    Data->DesertMask.SetNumUninitialized(CellCount);
    Data->PlainsMask.SetNumUninitialized(CellCount);

    BuildMacroClimate(*Data, Generator);

    Data->RequestedMountainRanges = 0;
    Data->PlacedMountainRanges = 0;

    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        double CellResistance = 0.0;
        double CellMountainMask = 0.0;
        double CellHillMask = 0.0;
        double CellDesertMask = 0.0;
        double CellPlainsMask = 0.0;
        Data->Height[Cell] = EvaluateLandform(
            Data->CellPosition(Cell), Generator.Seed,
            Generator.StructuralRelief, Generator.StructuralScale,
            Generator.TectonicActivity, Generator.RiftStrength,
            Generator.bGenerateClimate,
            Data->Temperature[Cell], Data->Moisture[Cell],
            CellResistance,
            CellMountainMask, CellHillMask, CellDesertMask, CellPlainsMask
        );
        Data->Resistance[Cell] = CellResistance;
        Data->MountainMask[Cell] = CellMountainMask;
        Data->HillMask[Cell] = CellHillMask;
        Data->DesertMask[Cell] = CellDesertMask;
        Data->PlainsMask[Cell] = CellPlainsMask;
    }

    SmoothLowReliefTerrain(*Data, 3);
    ApplyTerrainClimate(*Data, Generator);

    ApplyThermalErosion(
        *Data, Generator.ThermalErosionIterations, Generator.ThermalErosionStrength,
        Generator.TalusAngleDegrees, Generator.ErosionResistanceStrength
    );
    ApplyStreamPowerErosion(
        *Data, Generator.StreamPowerIterations, Generator.StreamPowerStrength,
        Generator.MountainStreamStartArea, Generator.LowlandStreamStartArea, Generator.DrainageEpsilon,
        Generator.ErosionResistanceStrength
    );

    EvolveTerrainFromDrainage(
        *Data, FMath::Clamp(Generator.StreamPowerIterations / 2 + 1, 2, 6),
        Generator.StreamPowerStrength,
        Generator.MountainStreamStartArea, Generator.LowlandStreamStartArea,
        Generator.DrainageEpsilon, Generator.ErosionResistanceStrength
    );
    ApplyThermalErosion(
        *Data, FMath::Clamp(Generator.ThermalErosionIterations / 3, 1, 4),
        Generator.ThermalErosionStrength * 0.32,
        Generator.TalusAngleDegrees, Generator.ErosionResistanceStrength
    );
    PriorityFlood(*Data, Generator.DrainageEpsilon);
    BuildContinuousFlow(*Data);

    // Erosion is now authoritative for the final landform identity. Rebuild
    // mountain/hill/plains masks from the surviving surface, then reapply the
    // regional climate so altitude and final relief drive alpine transitions.
    ReclassifyFinalLandforms(*Data);
    ApplyTerrainClimate(*Data, Generator);
    ComputeDepressionFillHeight(*Data);

    TArray<bool> AuthoritativeRiverNetwork;
    if (Generator.bGenerateRivers)
    {
        AuthoritativeRiverNetwork = BuildAuthoritativeRiverNetwork(
            *Data, Generator.MountainStreamStartArea, Generator.LowlandStreamStartArea,
            Generator.MinimumRiverSystemLength,
            Data->RiverSeedCells, Data->RiverContinuationCells,
            Data->RejectedShortRiverSystems
        );
        const int32 StreamCaptureCount = CaptureNearbyDominantChannels(
            *Data, AuthoritativeRiverNetwork
        );
        if (StreamCaptureCount > 0)
        {
            // Receiver topology and D8 accumulation changed; rebuild the
            // authoritative channel mask so every captured tributary follows
            // its new trunk and widths use the combined downstream flow.
            AuthoritativeRiverNetwork = BuildAuthoritativeRiverNetwork(
                *Data,
                Generator.MountainStreamStartArea,
                Generator.LowlandStreamStartArea,
                Generator.MinimumRiverSystemLength,
                Data->RiverSeedCells,
                Data->RiverContinuationCells,
                Data->RejectedShortRiverSystems
            );
            UE_LOG(
                LogTemp,
                Display,
                TEXT("Avenor hydrology: %d minor drainage divides breached by stream capture."),
                StreamCaptureCount
            );
        }
        for (bool bChannelCell : AuthoritativeRiverNetwork)
        {
            Data->AuthoritativeRiverCells += bChannelCell ? 1 : 0;
        }
    }

    TArray<int32> OutflowSeeds;
    if (Generator.bGenerateLakes)
    {
        const TArray<int32> MandatoryTerminusSeeds = Generator.bGenerateRivers
            ? FindRiverFedBasinSeeds(
                *Data, AuthoritativeRiverNetwork, Generator.MainRiverArea * 0.4)
            : TArray<int32>();
        ExtractLakes(
            *Data, MandatoryTerminusSeeds, Generator.MinimumLakeDepth, Generator.MinimumLakeBedDepth,
            Generator.MaximumLakeBedDepth, Generator.MaximumLakeArea, Generator.MaximumLakeCount,
            Generator.MaximumLakeCoverageFraction, Generator.LakeBankBlendWidth, Generator.LakeDepthRampWidth,
            Generator.FeatureSplinePointSpacing, Generator.LakeSurfaceInset,
            Generator.bGenerateLakeOutflows, OutflowSeeds
        );
    }
    else
    {
        Data->LakeIndex.Init(INDEX_NONE, CellCount);
    }
    if (Generator.bGenerateRivers)
    {
        ExtractRivers(
            *Data, AuthoritativeRiverNetwork, OutflowSeeds, Generator.Seed,
            Generator.MainRiverArea, Generator.HeadwaterWidth, Generator.MainRiverWidth,
            Generator.MaximumRiverDepth, Generator.HeadwaterValleyHalfWidth, Generator.MainValleyHalfWidth,
            Generator.MaximumValleyDepth, Generator.LowlandMeanderStrength,
            Generator.FeatureSplinePointSpacing, Generator.MaximumRiverReaches,
            Generator.bGenerateMesasAndCanyons, Generator.CanyonStartArea,
            Generator.RiverChannelSteepness
        );
    }
    if (Generator.bGenerateOcean)
    {
        const double Inset = FMath::Clamp(Generator.CoastTransitionWidth, CellSize, FMath::Min(Size.X, Size.Y) * 0.42);
        TArray<FVector> Boundary = {
            FVector(Bounds.Min.X + Inset, Bounds.Min.Y + Inset, Generator.SeaLevel),
            FVector(Bounds.Max.X - Inset, Bounds.Min.Y + Inset, Generator.SeaLevel),
            FVector(Bounds.Max.X - Inset, Bounds.Max.Y - Inset, Generator.SeaLevel),
            FVector(Bounds.Min.X + Inset, Bounds.Max.Y - Inset, Generator.SeaLevel)
        };
        Boundary = ChaikinSmooth(Boundary, true, 2);
        Data->OceanBoundary = ResamplePolyline(Boundary, FMath::Max(CellSize, 5000.0), true);
    }
    RefineClimateFromHydrology(
        *Data, AuthoritativeRiverNetwork, Generator
    );
    return Data;
}

class FStripTerrainOp final : public UE::MeshPartition::IModifierBackgroundOp
{
public:
    explicit FStripTerrainOp(FName Name) : IModifierBackgroundOp(Name) {}

    virtual void GetInstancesInBounds(const FBox& InBounds, TArray<FInstanceInfo>& OutInstances) const override
    {
        if (!WorldBounds.Intersect(InBounds))
        {
            return;
        }
        FInstanceInfo& Instance = OutInstances.AddDefaulted_GetRef();
        Instance.InstanceID = 0;
        Instance.Bounds = WorldBounds;
        Instance.ReadViewComponents = UE::MeshPartition::EMeshViewComponents::VertexPos;
        Instance.WriteViewComponents = static_cast<UE::MeshPartition::EMeshViewComponents>(
            UE::MeshPartition::EMeshViewComponents::VertexPos |
            UE::MeshPartition::EMeshViewComponents::VertexAttributeWeight
        );
        Instance.UsedChannels = {
            ElevationChannel, SlopeChannel, WetnessChannel, RiverChannel, LakeChannel,
            MountainChannel, HillChannel, DesertChannel, PlainsChannel
        };
    }

    virtual void ApplyModifications(
        UE::MeshPartition::FMeshView& MeshView,
        const FTransform3d& MeshTransform,
        const FInstanceInfo& InstanceInfo
    ) const override
    {
        (void)InstanceInfo;
        const UAvenorTerrainData* Data = TerrainData.Get();
        if (!Data || !Data->HasValidData())
        {
            return;
        }
        FAvenorTerrainHeightChunkCache ChunkCache;
        for (int32 Vertex = 0; Vertex < MeshView.VertexCount(); ++Vertex)
        {
            FVector3d WorldPosition = MeshTransform.TransformPosition(MeshView.GetVertexPos(Vertex));
            const FVector2D XY(WorldPosition.X, WorldPosition.Y);
            FAvenorTerrainSample Sample;
            if (!Data->SampleTerrain(XY, Sample, ChunkCache))
            {
                continue;
            }
            float FinalHeight = Sample.Height;
            Data->SampleFinalHeight(
                XY, FinalHeight, ChunkCache, nullptr
            );
            const double SampledHeight = static_cast<double>(FinalHeight);
            WorldPosition.Z = BaseWorldZ + SampledHeight;
            MeshView.SetVertexPos(Vertex, MeshTransform.InverseTransformPosition(WorldPosition));
            MeshView.SetVertexAttributeWeight(
                ElevationChannel,
                Vertex,
                static_cast<float>(FMath::Clamp(
                    (SampledHeight - Data->WorldBounds.Min.Z) /
                        FMath::Max(1.0, Data->WorldBounds.GetSize().Z),
                    0.0,
                    1.0
                ))
            );
            MeshView.SetVertexAttributeWeight(SlopeChannel, Vertex, FMath::Clamp(Sample.Slope / 0.35f, 0.0f, 1.0f));
            MeshView.SetVertexAttributeWeight(WetnessChannel, Vertex, FMath::Clamp(FMath::Loge(1.0f + Sample.Accumulation) / 6.0f, 0.0f, 1.0f));
            MeshView.SetVertexAttributeWeight(RiverChannel, Vertex, Data->SampleRiverWeight(XY, ChunkCache));
            MeshView.SetVertexAttributeWeight(LakeChannel, Vertex, Data->SampleLakeWeight(XY, ChunkCache));
            MeshView.SetVertexAttributeWeight(MountainChannel, Vertex, FMath::Clamp(Sample.Mountain, 0.0f, 1.0f));
            MeshView.SetVertexAttributeWeight(HillChannel, Vertex, FMath::Clamp(Sample.Hill, 0.0f, 1.0f));
            MeshView.SetVertexAttributeWeight(DesertChannel, Vertex, FMath::Clamp(Sample.Desert, 0.0f, 1.0f));
            MeshView.SetVertexAttributeWeight(PlainsChannel, Vertex, FMath::Clamp(Sample.Plains, 0.0f, 1.0f));
        }
    }

    virtual bool DisableDDCWrite() const override
    {
        const UAvenorTerrainData* Data = TerrainData.Get();
        return !Data || !Data->HasValidData();
    }
    static FGuid Version() { return FGuid(TEXT("849c98b2-52c1-48dd-9431-9864e17fc8ae")); }

    FBox WorldBounds = FBox(ForceInit);
    double BaseWorldZ = 0.0;
    TStrongObjectPtr<UAvenorTerrainData> TerrainData;
};

template<typename TWaterActor>
static TWaterActor* SpawnWaterActor(
    UWorld& World,
    const FString& Label,
    FName OwnerTag
)
{
    UActorFactory* Factory = GEditor ? GEditor->FindActorFactoryForActorClass(TWaterActor::StaticClass()) : nullptr;
    if (!Factory)
    {
        return nullptr;
    }
    TWaterActor* Actor = Cast<TWaterActor>(Factory->CreateActor(TWaterActor::StaticClass(), World.GetCurrentLevel(), FTransform::Identity));
    if (Actor)
    {
        Actor->SetActorLabel(Label);
        Actor->SetFolderPath(TEXT("Avenor/Generated/StripWater"));
        Actor->Tags.AddUnique(GeneratedWaterTag);
        Actor->Tags.AddUnique(OwnerTag);
    }
    return Actor;
}

static void ConfigureWaterSpline(
    UWaterSplineComponent& Spline,
    const TArray<FVector>& Points,
    bool bClosed
)
{
    Spline.SetSplinePoints(Points, ESplineCoordinateSpace::World, false);
    Spline.SetClosedLoop(bClosed, false);
    for (int32 Index = 0; Index < Spline.GetNumberOfSplinePoints(); ++Index)
    {
        Spline.SetSplinePointType(Index, ESplinePointType::Curve, false);
        if (bClosed && Points.Num() >= 3)
        {
            const int32 PreviousIndex = (Index - 1 + Points.Num()) % Points.Num();
            const int32 NextIndex = (Index + 1) % Points.Num();
            FVector Tangent = (Points[NextIndex] - Points[PreviousIndex]) * 0.5;
            Tangent.Z = 0.0;
            Spline.SetTangentsAtSplinePoint(
                Index,
                Tangent,
                Tangent,
                ESplineCoordinateSpace::World,
                false
            );
        }
    }
    Spline.UpdateSpline();
}

static void ConfigureRiverSpline(UWaterSplineComponent& Spline, const TArray<FVector>& Points, double FullWidth, double Depth)
{
    ConfigureWaterSpline(Spline, Points, false);
    for (int32 Index = 0; Index < Points.Num(); ++Index)
    {
        const int32 PreviousIndex = FMath::Max(0, Index - 1);
        const int32 NextIndex = FMath::Min(Points.Num() - 1, Index + 1);
        FVector Tangent = (Points[NextIndex] - Points[PreviousIndex]) * 0.5;
        if (Index == 0 && Points.Num() > 1)
        {
            Tangent = Points[1] - Points[0];
        }
        else if (Index + 1 == Points.Num() && Points.Num() > 1)
        {
            Tangent = Points[Index] - Points[Index - 1];
        }
        Tangent.Z = 0.0;
        Spline.SetTangentsAtSplinePoint(
            Index,
            Tangent,
            Tangent,
            ESplineCoordinateSpace::World,
            false
        );
    }
    Spline.UpdateSpline();
    UWaterSplineMetadata* Metadata = Cast<UWaterSplineMetadata>(Spline.GetSplinePointsMetadata());
    if (Metadata)
    {
        Metadata->Fixup(Spline.GetNumberOfSplinePoints(), &Spline);
        const float MetadataWidth = static_cast<float>(FMath::Max(100.0, FullWidth));
        const float WaterDepth = static_cast<float>(FMath::Max(1.0, Depth));
        for (int32 Index = 0; Index < Spline.GetNumberOfSplinePoints(); ++Index)
        {
            if (Metadata->RiverWidth.Points.IsValidIndex(Index))
            {
                Metadata->RiverWidth.Points[Index].OutVal = MetadataWidth;
            }
            if (Metadata->Depth.Points.IsValidIndex(Index))
            {
                Metadata->Depth.Points[Index].OutVal = WaterDepth;
            }
        }
    }
    Spline.K2_SynchronizeAndBroadcastDataChange();
}

static void ConfigureWaterTerrainSettings(
    AWaterBody& Water,
    bool bLake,
    double ChannelDepth,
    double BankOrShoreWidth,
    const FAvenorWaterTerrainSettings& Settings,
    double EdgeOffsetScale = 1.0
)
{
    // EdgeOffsetScale narrows the flat dry-bank shelf before the falloff ramp
    // for steep-sided reaches (canyons), so the water meets the carved walls
    // almost directly instead of sitting behind a floodplain-width shelf.
    // A dry edge-offset shelf is useful around a level lake, but on a river
    // it can raise/preserve a continuous levee after hydrology has already
    // chosen its route. That is the visible divider in the parallel
    // "highway river" case. Ordinary river modifiers should only cut the wet
    // channel and blend outward from its edge.
    const double DryBankWidth = bLake
        ? FMath::Max(0.0, Settings.DryBankWidth)
            * FMath::Clamp(EdgeOffsetScale, 0.2, 1.5)
        : 0.0;

    FWaterCurveSettings& Curve = const_cast<FWaterCurveSettings&>(
        Water.GetWaterCurveSettings()
    );
    Curve.bUseCurveChannel = true;
    Curve.ChannelDepth = static_cast<float>(FMath::Max(100.0, ChannelDepth));
    Curve.ChannelEdgeOffset = static_cast<float>(DryBankWidth);
    Curve.CurveRampWidth = static_cast<float>(FMath::Max(100.0, BankOrShoreWidth));

    FWaterBodyHeightmapSettings& Heightmap = const_cast<FWaterBodyHeightmapSettings&>(
        Water.GetWaterHeightmapSettings()
    );
    // Never let a river brush raise terrain above the solved base height.
    // Lakes retain alpha blending because their level shore/basin transition
    // deliberately needs to meet one common water surface.
    Heightmap.BlendMode = bLake
        ? EWaterBrushBlendType::AlphaBlend
        : EWaterBrushBlendType::Min;
    Heightmap.FalloffSettings.FalloffMode = EWaterBrushFalloffMode::Width;
    Heightmap.FalloffSettings.FalloffWidth = static_cast<float>(FMath::Max(100.0, BankOrShoreWidth));
    Heightmap.FalloffSettings.EdgeOffset = static_cast<float>(DryBankWidth);
    Heightmap.FalloffSettings.ZOffset = 0.0f;
    Heightmap.Effects.Blurring.bBlurShape = Settings.BlurRadius > 0;
    Heightmap.Effects.Blurring.Radius = FMath::Clamp(Settings.BlurRadius, 0, 16);
    const float Roughness = static_cast<float>(FMath::Clamp(Settings.EdgeRoughness, 0.0, 1.0));
    Heightmap.Effects.CurlNoise.Curl1Amount = Roughness * (bLake ? 1800.0f : 600.0f);
    Heightmap.Effects.CurlNoise.Curl1Tiling = bLake ? 65000.0f : 30000.0f;
    Heightmap.Effects.CurlNoise.Curl2Amount = Roughness * (bLake ? 650.0f : 250.0f);
    Heightmap.Effects.CurlNoise.Curl2Tiling = bLake ? 18000.0f : 9000.0f;
    Heightmap.Effects.Displacement.DisplacementHeight = 0.0f;
    Heightmap.Effects.SmoothBlending.InnerSmoothDistance = static_cast<float>(BankOrShoreWidth * 0.2);
    Heightmap.Effects.SmoothBlending.OuterSmoothDistance = static_cast<float>(BankOrShoreWidth * 0.35);

    TMap<FName, FWaterBodyWeightmapSettings>& Weightmaps =
        const_cast<TMap<FName, FWaterBodyWeightmapSettings>&>(Water.GetLayerWeightmapSettings());
    Weightmaps.Reset();
    auto AddWeight = [&](FName Name, float EdgeOffset, float FalloffWidth)
    {
        if (Name.IsNone())
        {
            return;
        }
        FWaterBodyWeightmapSettings& Weight = Weightmaps.Add(Name);
        Weight.EdgeOffset = EdgeOffset;
        Weight.FalloffWidth = FMath::Max(0.1f, FalloffWidth);
        Weight.FinalOpacity = 1.0f;
        Weight.Midpoint = 0.5f;
        Weight.TextureInfluence = 0.0f;
    };
    if (bLake)
    {
        AddWeight(Settings.LakeBedWeight, 0.0f, 100.0f);
        AddWeight(Settings.LakeShoreWeight, static_cast<float>(BankOrShoreWidth), static_cast<float>(BankOrShoreWidth));
    }
    else
    {
        AddWeight(Settings.RiverBedWeight, 0.0f, 100.0f);
        AddWeight(Settings.RiverBankWeight, static_cast<float>(BankOrShoreWidth), static_cast<float>(BankOrShoreWidth));
    }
}

static UClass* FindWaterModifierClass(FName ModifierName)
{
    static TMap<FName, TWeakObjectPtr<UClass>> Cache;
    if (const TWeakObjectPtr<UClass>* Cached = Cache.Find(ModifierName))
    {
        if (Cached->IsValid())
        {
            return Cached->Get();
        }
    }

    const FString NativeClassPath = FString::Printf(
        TEXT("/Script/MeshPartitionWater.%s"), *ModifierName.ToString()
    );
    if (UClass* NativeClass = StaticLoadClass(
        UE::MeshPartition::UModifierComponent::StaticClass(),
        nullptr,
        *NativeClassPath
    ))
    {
        if (!NativeClass->HasAnyClassFlags(CLASS_Abstract))
        {
            Cache.Add(ModifierName, NativeClass);
            return NativeClass;
        }
    }

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    FARFilter Filter;
    Filter.PackagePaths.Add(TEXT("/MeshPartitionWater"));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());

    TArray<FAssetData> Assets;
    AssetRegistry.GetAssets(Filter, Assets);
    for (const FAssetData& Asset : Assets)
    {
        if (Asset.AssetName != ModifierName)
        {
            continue;
        }

        UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
        UClass* ModifierClass = Blueprint ? Blueprint->GeneratedClass : nullptr;
        if (ModifierClass
            && ModifierClass->IsChildOf(UE::MeshPartition::UModifierComponent::StaticClass())
            && !ModifierClass->HasAnyClassFlags(CLASS_Abstract))
        {
            Cache.Add(ModifierName, ModifierClass);
            return ModifierClass;
        }
    }

    UE_LOG(
        LogTemp,
        Error,
        TEXT("Avenor water: could not resolve Epic's MeshPartitionWater %s component class as either /Script/MeshPartitionWater.%s or a /MeshPartitionWater Blueprint asset. Ensure the plugin is enabled and its content is mounted."),
        *ModifierName.ToString(),
        *ModifierName.ToString()
    );
    return nullptr;
}

static UE::MeshPartition::UModifierComponent* AddNativeWaterModifier(
    AWaterBody& Water,
    UE::MeshPartition::AMeshPartition& MeshPartition,
    FName PriorityLayer,
    UClass& ModifierClass
)
{
    if (!ModifierClass.IsChildOf(UE::MeshPartition::UModifierComponent::StaticClass())
        || ModifierClass.HasAnyClassFlags(CLASS_Abstract))
    {
        return nullptr;
    }

    UE::MeshPartition::UModifierComponent* Modifier =
        NewObject<UE::MeshPartition::UModifierComponent>(
            &Water, &ModifierClass, TEXT("AvenorMeshTerrainWaterModifier"), RF_Transactional
    );
    if (!Modifier)
    {
        return nullptr;
    }
    Modifier->bIsEditorOnly = true;
    Modifier->SetupAttachment(Water.GetRootComponent());
    Water.AddInstanceComponent(Modifier);
    Modifier->RegisterComponent();
    Modifier->BP_SetAffectedMegaMesh(&MeshPartition);
    Modifier->SetPriorityLayer(PriorityLayer);
    Modifier->SetPriority(10.0);
    Modifier->PostEditChange();
    return Modifier;
}
} // namespace UE::Avenor::Strip

TArray<FBox> UAvenorStripTerrainModifier::ComputeBounds() const
{
    const AAvenorStripTerrainGenerator* Generator = Cast<AAvenorStripTerrainGenerator>(GetOwner());
    if (!Generator)
    {
        return {};
    }
    const FBox GenerationBounds = Generator->GetGenerationBounds();
    return GenerationBounds.IsValid ? TArray<FBox>{GenerationBounds} : TArray<FBox>{};
}

TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
UAvenorStripTerrainModifier::CreateBackgroundOp(UE::MeshPartition::EBuildType BuildType) const
{
    (void)BuildType;
    TSharedPtr<FStripTerrainOp> Op = MakeShared<FStripTerrainOp>(GetFName());
    const AAvenorStripTerrainGenerator* Generator = Cast<AAvenorStripTerrainGenerator>(GetOwner());
    if (Generator)
    {
        Op->WorldBounds = Generator->GetGenerationBounds();
        Op->BaseWorldZ = Generator->GetActorLocation().Z;
        Op->TerrainData.Reset(Generator->BakedTerrainData.LoadSynchronous());
        if (!Op->TerrainData.IsValid() || !Op->TerrainData->HasValidData())
        {
            UE_LOG(
                LogTemp,
                Error,
                TEXT("Avenor terrain modifier: no valid Baked Terrain Data is available; provider-only sections will not be written to DDC.")
            );
        }
    }
    return Op;
}

FGuid UAvenorStripTerrainModifier::GetCodeVersionKey() const
{
    return FStripTerrainOp::Version();
}

AAvenorStripTerrainGenerator::AAvenorStripTerrainGenerator()
{
    PrimaryActorTick.bCanEverTick = false;
    SetIsSpatiallyLoaded(false);
    TerrainModifier = CreateDefaultSubobject<UAvenorStripTerrainModifier>(TEXT("StripTerrain"));
    SetRootComponent(TerrainModifier);
    FastPreviewMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("FastTerrainPreview"));
    FastPreviewMesh->SetupAttachment(TerrainModifier);
    FastPreviewMesh->bIsEditorOnly = true;
    FastPreviewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    FastPreviewMesh->SetGenerateOverlapEvents(false);
    FastPreviewMesh->SetCastShadow(false);
    FastPreviewMesh->SetHiddenInGame(true);
    FastPreviewMesh->SetVisibility(false);
    LastBuildStamp = TEXT("Never generated");
    BakedDataStatus = TEXT("No baked terrain data assigned");
    ResolveSettings();
}

void AAvenorStripTerrainGenerator::ResolveSettings()
{
    AnalysisCellSize = Erosion.AnalysisSpacing;
    StructuralRelief = FMath::Max(25000.0, Landforms.ReliefHeight);
    // 0 means "auto": derive a base feature scale from the world's short-axis
    // extent so terrain provinces stay proportionate as a test world is
    // resized, rather than inheriting a fixed absolute default tuned for one
    // particular world size. A non-zero value pins an explicit scale that
    // stays fixed regardless of world size.
    const double ShortAxisExtent = FMath::Max(
        100000.0, FMath::Min(WorldSize.X, WorldSize.Y)
    );
    StructuralScale = Landforms.StructuralScale > 0.0
        ? FMath::Clamp(Landforms.StructuralScale, 100000.0, 5000000.0)
        : FMath::Clamp(ShortAxisExtent * 0.5, 200000.0, 5000000.0);
    TectonicActivity = FMath::Clamp(Landforms.TectonicActivity, 0.0, 1.0);
    RiftStrength = FMath::Clamp(Landforms.RiftStrength, 0.0, 1.0);
    bGenerateMesasAndCanyons = true;
    MesaScale = FMath::Clamp(StructuralScale * 0.24, 150000.0, 800000.0);

    bGenerateClimate = Climate.bEnabled;
    ClimateTemperature = FMath::Clamp(Climate.Temperature, 0.0, 1.0);
    ClimateMoisture = FMath::Clamp(Climate.Moisture, 0.0, 1.0);
    ClimateRegionalVariation = FMath::Clamp(
        Climate.RegionalVariation, 0.0, 1.0
    );
    // 0 means "auto": derive region spacing from the world's long-axis
    // length so a short test world still gets several climate anchors
    // instead of only 1-2, which barely lets temperature/moisture drift
    // from the midpoint before the world ends.
    const double LongAxisExtent = FMath::Max(
        100000.0, LongAxis == EAvenorStripLongAxis::X ? WorldSize.X : WorldSize.Y
    );
    ClimateRegionSpacing = Climate.RegionSpacing > 0.0
        ? FMath::Max(100000.0, Climate.RegionSpacing)
        : FMath::Clamp(LongAxisExtent / 6.0, 300000.0, 3000000.0);
    ClimateWaterInfluenceDistance = FMath::Max(
        10000.0, Climate.WaterInfluenceDistance
    );
    ClimateWaterMoistureBoost = FMath::Clamp(
        Climate.WaterMoistureBoost, 0.0, 1.0
    );
    bShowcaseClimateCompression = Climate.bShowcaseClimateCompression;
    PrevailingWindDirectionDegrees = FMath::Fmod(
        FMath::Max(0.0, Climate.PrevailingWindDirectionDegrees), 360.0
    );
    RainShadowStrength = FMath::Clamp(Climate.RainShadowStrength, 0.0, 1.0);

    ErosionResistanceStrength = (bGenerateClimate || bGenerateMesasAndCanyons)
        ? 0.55 : 0.0;

    const double ErosionStrength = FMath::Clamp(Erosion.Strength, 0.0, 1.0);
    ThermalErosionIterations = FMath::Clamp(Erosion.Passes, 1, 24);
    StreamPowerIterations = FMath::Clamp(FMath::RoundToInt(Erosion.Passes * 0.75), 1, 18);
    ThermalErosionStrength = FMath::Lerp(0.1, 0.7, ErosionStrength);
    StreamPowerStrength = FMath::Lerp(0.2, 1.8, ErosionStrength);

    bGenerateRivers = Hydrology.bRivers;
    const double Density = FMath::Clamp(Hydrology.RiverDensity, 0.0, 1.0);
    MountainStreamStartArea = FMath::Lerp(2.0, 0.25, Density);
    LowlandStreamStartArea = FMath::Lerp(12.0, 2.0, Density);
    MinimumRiverSystemLength = Hydrology.MinimumRiverLength;
    HeadwaterWidth = 800.0 * WaterTerrain.RiverWidthScale;
    MainRiverWidth = 12000.0 * WaterTerrain.RiverWidthScale;
    MaximumRiverDepth = 1800.0 * WaterTerrain.RiverDepthScale;
    // Broad valley form belongs to erosion. These widths are now only the
    // active river's immediate valley/bank envelope, with canyon reaches able
    // to expand/deepen independently when the geomorphology calls for it.
    HeadwaterValleyHalfWidth = FMath::Max(
        2500.0, WaterTerrain.RiverBankWidth * 0.35
    );
    MainValleyHalfWidth = FMath::Max(
        12000.0, WaterTerrain.RiverBankWidth * 1.25
    );

    RefinementEdgeLengthHeadwater = FMath::Max(
        100.0, Refinement.HeadwaterEdgeLength
    );
    RefinementEdgeLengthMainRiver = FMath::Max(
        100.0, Refinement.MainRiverEdgeLength
    );
    RefinementEdgeLengthCanyon = FMath::Max(
        100.0, Refinement.CanyonEdgeLength
    );
    RefinementEdgeLengthLakeShore = FMath::Max(
        100.0, Refinement.LakeShoreEdgeLength
    );
    RefinementCoverageMargin = FMath::Max(
        0.0, Refinement.CoverageMargin
    );
    RefinementMaxTessellationLevel = FMath::Clamp(
        Refinement.MaximumTessellationLevel, 1, 6
    );

    bGenerateLakes = Hydrology.bLakes;
    MaximumLakeCount = Hydrology.MaximumLakes;
    MinimumLakeDepth = Hydrology.MinimumLakeDepression;
    MinimumLakeBedDepth = FMath::Max(100.0, WaterTerrain.LakeBedDepth * 0.35);
    MaximumLakeBedDepth = FMath::Max(MinimumLakeBedDepth, WaterTerrain.LakeBedDepth);
    LakeBankBlendWidth = WaterTerrain.LakeShoreWidth;
    LakeDepthRampWidth = FMath::Max(1000.0, WaterTerrain.LakeShoreWidth * 0.35);
    LakeSurfaceInset = FMath::Max(0.0, WaterTerrain.LakeSurfaceInset);
}

FBox AAvenorStripTerrainGenerator::GetGenerationBounds() const
{
    if (!bGeneratingGeography)
    {
        {
            FScopeLock Lock(&DataMutex);
            if (CachedData && CachedData->Bounds.IsValid)
            {
                return CachedData->Bounds;
            }
        }
        if (const UAvenorTerrainData* Asset = BakedTerrainData.LoadSynchronous())
        {
            if (Asset->HasValidData())
            {
                return Asset->WorldBounds;
            }
        }
    }
    const FVector Centre = GetActorLocation();
    const FVector Extent(
        FMath::Max(10000.0, WorldSize.X) * 0.5,
        FMath::Max(10000.0, WorldSize.Y) * 0.5,
        500000.0
    );
    return FBox(Centre - Extent, Centre + Extent);
}

TSharedPtr<const FAvenorStripData> AAvenorStripTerrainGenerator::GetOrCreateData() const
{
    FScopeLock Lock(&DataMutex);
    if (!CachedData)
    {
        CachedData = LoadBakedData();
        if (!CachedData)
        {
            UE_LOG(
                LogTemp,
                Error,
                TEXT("Avenor terrain has no valid baked data. Use Generate and Bake Geography explicitly; automatic procedural regeneration is disabled.")
            );
        }
    }
    return CachedData;
}

void AAvenorStripTerrainGenerator::InvalidateData()
{
    FScopeLock Lock(&DataMutex);
    CachedData.Reset();
    bTerrainPlanReadyForWater = false;
    bRefinementPlanReadyForWater = false;
}

void AAvenorStripTerrainGenerator::ReleaseCachedData()
{
    int32 ReleasedCells = 0;
    {
        FScopeLock Lock(&DataMutex);
        ReleasedCells = CachedData ? CachedData->Height.Num() : 0;
        CachedData.Reset();
    }
    if (ReleasedCells > 0)
    {
        UE_LOG(
            LogTemp,
            Display,
            TEXT("Avenor released the expanded terrain analysis cache (%d cells); Mesh Partition will continue from the baked chunk asset."),
            ReleasedCells
        );
    }
}

FString AAvenorStripTerrainGenerator::BuildSettingsSnapshot() const
{
    return FString::Printf(
        TEXT("v%d|seed=%d|size=%.17g,%.17g|axis=%d|")
        TEXT("land=%.17g,%.17g,%.17g,%.17g|")
        TEXT("climate=%d,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%d|")
        TEXT("erosion=%.17g,%d,%.17g|hydrology=%d,%.17g,%.17g,%d,%d,%.17g|")
        TEXT("water=%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%d,%.17g,%s,%s,%s,%s"),
        UE::Avenor::Strip::BakedData::GeneratorAlgorithmVersion,
        Seed, WorldSize.X, WorldSize.Y, static_cast<int32>(LongAxis),
        Landforms.ReliefHeight, Landforms.StructuralScale,
        Landforms.TectonicActivity, Landforms.RiftStrength,
        Climate.bEnabled, Climate.Temperature, Climate.Moisture,
        Climate.RegionalVariation, Climate.RegionSpacing,
        Climate.WaterInfluenceDistance, Climate.WaterMoistureBoost,
        Climate.bShowcaseClimateCompression,
        Erosion.Strength, Erosion.Passes, Erosion.AnalysisSpacing,
        Hydrology.bRivers, Hydrology.RiverDensity, Hydrology.MinimumRiverLength,
        Hydrology.bLakes, Hydrology.MaximumLakes, Hydrology.MinimumLakeDepression,
        WaterTerrain.RiverWidthScale, WaterTerrain.RiverDepthScale,
        WaterTerrain.RiverBankWidth, WaterTerrain.LakeBedDepth,
        WaterTerrain.LakeShoreWidth, WaterTerrain.LakeSurfaceInset,
        WaterTerrain.DryBankWidth, WaterTerrain.BlurRadius,
        WaterTerrain.EdgeRoughness,
        *WaterTerrain.RiverBedWeight.ToString(),
        *WaterTerrain.RiverBankWeight.ToString(),
        *WaterTerrain.LakeBedWeight.ToString(),
        *WaterTerrain.LakeShoreWeight.ToString()
    );
}

FString AAvenorStripTerrainGenerator::BuildSettingsHash() const
{
    const FString Snapshot = BuildSettingsSnapshot();
    return FMD5::HashAnsiString(*Snapshot);
}

bool AAvenorStripTerrainGenerator::BakeData(const TSharedPtr<const FAvenorStripData>& Data)
{
#if WITH_EDITOR
    if (!Data || Data->Columns < 2 || Data->Rows < 2)
    {
        return false;
    }

    UAvenorTerrainData* Asset = BakedTerrainData.LoadSynchronous();
    if (!Asset)
    {
        bool bCreatedAsset = false;
        const FString AssetName = FString::Printf(
            TEXT("DA_AvenorTerrainData_%s"), *GetFName().ToString()
        );
        const FString PackageName = FString::Printf(
            TEXT("/Game/Avenor/Generated/%s"), *AssetName
        );
        Asset = LoadObject<UAvenorTerrainData>(
            nullptr,
            *FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName)
        );
        if (!Asset)
        {
            UPackage* Package = CreatePackage(*PackageName);
            Asset = NewObject<UAvenorTerrainData>(
                Package,
                *AssetName,
                RF_Public | RF_Standalone | RF_Transactional
            );
            bCreatedAsset = Asset != nullptr;
        }
        if (!Asset)
        {
            return false;
        }
        if (bCreatedAsset)
        {
            FAssetRegistryModule::AssetCreated(Asset);
        }
        Modify();
        BakedTerrainData = Asset;
        MarkPackageDirty();
    }

    Asset->Modify();
    Asset->FormatVersion = UAvenorTerrainData::CurrentFormatVersion;
    Asset->GeneratorAlgorithmVersion = UE::Avenor::Strip::BakedData::GeneratorAlgorithmVersion;
    Asset->SettingsHash = BuildSettingsHash();
    Asset->GenerationSettingsSnapshot = BuildSettingsSnapshot();
    Asset->GeneratedAtUtc = FDateTime::UtcNow();
    Asset->Seed = Seed;
    Asset->WorldBounds = Data->Bounds;
    Asset->Columns = Data->Columns;
    Asset->Rows = Data->Rows;
    Asset->CellSize = Data->CellSize;
    Asset->ChunkCellSize = FMath::Clamp(BakedChunkCellSize, 16, 512);
    Asset->Chunks.Reset();
    Asset->SpineLayer = FAvenorBakedSpineLayer();

    const int32 ChunkColumns = FMath::DivideAndRoundUp(Data->Columns, Asset->ChunkCellSize);
    const int32 ChunkRows = FMath::DivideAndRoundUp(Data->Rows, Asset->ChunkCellSize);
    Asset->Chunks.Reserve(ChunkColumns * ChunkRows);
    for (int32 ChunkY = 0; ChunkY < ChunkRows; ++ChunkY)
    {
        for (int32 ChunkX = 0; ChunkX < ChunkColumns; ++ChunkX)
        {
            FAvenorTerrainDataChunk& Chunk = Asset->Chunks.AddDefaulted_GetRef();
            Chunk.ChunkCoordinate = FIntPoint(ChunkX, ChunkY);
            const int32 StartX = ChunkX * Asset->ChunkCellSize;
            const int32 StartY = ChunkY * Asset->ChunkCellSize;
            const int32 CountX = FMath::Min(Asset->ChunkCellSize, Data->Columns - StartX);
            const int32 CountY = FMath::Min(Asset->ChunkCellSize, Data->Rows - StartY);
            if (!UE::Avenor::Strip::BakedData::CompressChunk(
                *Data, StartX, StartY, CountX, CountY, Chunk
            ))
            {
                Asset->Chunks.Reset();
                return false;
            }
        }
    }

    if (bGenerateClimate)
    {
        if (!UE::Avenor::Strip::BakedData::BuildClimateTextureTiles(
            *Asset,
            *Data,
            GetFName().ToString(),
            LongAxis,
            ClimateRegionSpacing,
            WaterTerrain.RiverBankWidth
        ))
        {
            UE_LOG(
                LogTemp,
                Error,
                TEXT("Avenor climate source textures could not be generated or saved.")
            );
            Asset->Chunks.Reset();
            return false;
        }
    }
    else
    {
        Asset->ClimateTiles.Reset();
        Asset->WorldClimateMaps = FAvenorWorldClimateMapReference();
    }

    Asset->Rivers.Reset(Data->Rivers.Num());
    for (const FRiverReach& Source : Data->Rivers)
    {
        FAvenorBakedRiverReach& Target = Asset->Rivers.AddDefaulted_GetRef();
        Target.Points = Source.Points;
        Target.Width = Source.Width;
        Target.Depth = Source.Depth;
        Target.BankWidth = WaterTerrain.RiverBankWidth;
        Target.ValleyHalfWidth = Source.ValleyHalfWidth;
        Target.ValleyDepth = Source.ValleyDepth;
        Target.CrossSectionExponent = Source.CrossSectionExponent;
        Target.ChannelSteepness = Source.ChannelSteepness;
        Target.DrainageArea = Source.DrainageArea;
        Target.StartLakeIndex = Source.StartLakeIndex;
        Target.EndLakeIndex = Source.EndLakeIndex;
        Target.bIsCanyon = Source.bIsCanyon;
    }
    Asset->Lakes.Reset(Data->Lakes.Num());
    for (const FLakeBasin& Source : Data->Lakes)
    {
        FAvenorBakedLakeBasin& Target = Asset->Lakes.AddDefaulted_GetRef();
        Target.Shoreline = Source.Shoreline;
        Target.ShorelineHeight = Source.ShorelineHeight;
        Target.SurfaceHeight = Source.SurfaceHeight;
        Target.MaximumDepth = Source.MaximumDepth;
        Target.ModifierBedDepth = Source.ModifierBedDepth;
        Target.BankBlendWidth = Source.BankBlendWidth;
        Target.DepthRampWidth = Source.DepthRampWidth;
    }
    Asset->OceanBoundary = Data->OceanBoundary;
    Asset->RequestedMountainRanges = Data->RequestedMountainRanges;
    Asset->PlacedMountainRanges = Data->PlacedMountainRanges;
    Asset->AuthoritativeRiverCells = Data->AuthoritativeRiverCells;
    Asset->RiverSeedCells = Data->RiverSeedCells;
    Asset->RiverContinuationCells = Data->RiverContinuationCells;
    Asset->RejectedShortRiverSystems = Data->RejectedShortRiverSystems;
    Asset->RiverTerminusLakeCandidates = Data->RiverTerminusLakeCandidates;
    Asset->AcceptedRiverTerminusLakes = Data->AcceptedRiverTerminusLakes;
    Asset->AcceptedOptionalLakes = Data->AcceptedOptionalLakes;
    Asset->MarkPackageDirty();

    if (GEditor)
    {
        if (UEditorAssetSubsystem* AssetSubsystem =
            GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
        {
            if (!AssetSubsystem->SaveLoadedAsset(Asset, false))
            {
                UE_LOG(LogTemp, Error, TEXT("Avenor terrain data asset could not be saved: %s"), *Asset->GetPathName());
                return false;
            }
        }
    }
    BakedDataStatus = FString::Printf(
        TEXT("Current: %d cells in %d compressed chunks, %d climate tiles | %s"),
        Data->Height.Num(),
        Asset->Chunks.Num(),
        Asset->ClimateTiles.Num(),
        *Asset->SettingsHash
    );
    return true;
#else
    return false;
#endif
}

TSharedPtr<const FAvenorStripData> AAvenorStripTerrainGenerator::LoadBakedData() const
{
    const UAvenorTerrainData* Asset = BakedTerrainData.LoadSynchronous();
    if (!Asset || !Asset->HasValidData())
    {
        return nullptr;
    }

    TSharedPtr<FAvenorStripData> Data = MakeShared<FAvenorStripData>();
    Data->Bounds = Asset->WorldBounds;
    Data->Columns = Asset->Columns;
    Data->Rows = Asset->Rows;
    Data->CellSize = Asset->CellSize;
    const int32 TotalCells = Data->Columns * Data->Rows;
    Data->Height.SetNumZeroed(TotalCells);
    Data->Resistance.SetNumZeroed(TotalCells);
    Data->MountainMask.SetNumZeroed(TotalCells);
    Data->HillMask.SetNumZeroed(TotalCells);
    Data->DesertMask.SetNumZeroed(TotalCells);
    Data->PlainsMask.SetNumZeroed(TotalCells);
    Data->FilledHeight.SetNumZeroed(TotalCells);
    Data->Accumulation.SetNumZeroed(TotalCells);
    Data->Slope.SetNumZeroed(TotalCells);
    Data->ReceiverA.Init(INDEX_NONE, TotalCells);
    Data->ReceiverB.Init(INDEX_NONE, TotalCells);
    Data->ReceiverWeightA.SetNumZeroed(TotalCells);
    Data->FillParent.Init(INDEX_NONE, TotalCells);
    Data->LakeIndex.Init(INDEX_NONE, TotalCells);

    TSet<FIntPoint> SeenChunkCoordinates;
    for (const FAvenorTerrainDataChunk& Chunk : Asset->Chunks)
    {
        const int32 ExpectedStartX = Chunk.ChunkCoordinate.X * Asset->ChunkCellSize;
        const int32 ExpectedStartY = Chunk.ChunkCoordinate.Y * Asset->ChunkCellSize;
        const int32 ExpectedCountX = FMath::Min(
            Asset->ChunkCellSize, Data->Columns - ExpectedStartX
        );
        const int32 ExpectedCountY = FMath::Min(
            Asset->ChunkCellSize, Data->Rows - ExpectedStartY
        );
        if (Chunk.ChunkCoordinate.X < 0
            || Chunk.ChunkCoordinate.Y < 0
            || ExpectedCountX <= 0
            || ExpectedCountY <= 0
            || SeenChunkCoordinates.Contains(Chunk.ChunkCoordinate)
            || Chunk.StartCell != FIntPoint(ExpectedStartX, ExpectedStartY)
            || Chunk.CellCount != FIntPoint(ExpectedCountX, ExpectedCountY)
            || !UE::Avenor::Strip::BakedData::DecompressChunk(Chunk, *Data))
        {
            UE_LOG(
                LogTemp,
                Error,
                TEXT("Avenor baked terrain chunk (%d,%d) is invalid or corrupt."),
                Chunk.ChunkCoordinate.X,
                Chunk.ChunkCoordinate.Y
            );
            return nullptr;
        }
        SeenChunkCoordinates.Add(Chunk.ChunkCoordinate);
    }

    Data->Rivers.Reserve(Asset->Rivers.Num());
    for (const FAvenorBakedRiverReach& Source : Asset->Rivers)
    {
        FRiverReach& Target = Data->Rivers.AddDefaulted_GetRef();
        Target.Points = Source.Points;
        Target.Width = Source.Width;
        Target.Depth = Source.Depth;
        Target.ValleyHalfWidth = Source.ValleyHalfWidth;
        Target.ValleyDepth = Source.ValleyDepth;
        Target.CrossSectionExponent = Source.CrossSectionExponent;
        Target.ChannelSteepness = Source.ChannelSteepness;
        Target.DrainageArea = Source.DrainageArea;
        Target.StartLakeIndex = Source.StartLakeIndex;
        Target.EndLakeIndex = Source.EndLakeIndex;
        Target.bIsCanyon = Source.bIsCanyon;
        for (const FVector& Point : Target.Points)
        {
            Target.Bounds += FVector2D(Point);
        }
    }
    Data->Lakes.Reserve(Asset->Lakes.Num());
    for (const FAvenorBakedLakeBasin& Source : Asset->Lakes)
    {
        FLakeBasin& Target = Data->Lakes.AddDefaulted_GetRef();
        Target.Shoreline = Source.Shoreline;
        Target.ShorelineHeight = Source.ShorelineHeight;
        Target.SurfaceHeight = Source.SurfaceHeight;
        Target.MaximumDepth = Source.MaximumDepth;
        Target.ModifierBedDepth = Source.ModifierBedDepth;
        Target.BankBlendWidth = Source.BankBlendWidth;
        Target.DepthRampWidth = Source.DepthRampWidth;
        for (const FVector& Point : Target.Shoreline)
        {
            Target.Bounds += FVector2D(Point);
        }
    }
    Data->OceanBoundary = Asset->OceanBoundary;
    Data->RequestedMountainRanges = Asset->RequestedMountainRanges;
    Data->PlacedMountainRanges = Asset->PlacedMountainRanges;
    Data->AuthoritativeRiverCells = Asset->AuthoritativeRiverCells;
    Data->RiverSeedCells = Asset->RiverSeedCells;
    Data->RiverContinuationCells = Asset->RiverContinuationCells;
    Data->RejectedShortRiverSystems = Asset->RejectedShortRiverSystems;
    Data->RiverTerminusLakeCandidates = Asset->RiverTerminusLakeCandidates;
    Data->AcceptedRiverTerminusLakes = Asset->AcceptedRiverTerminusLakes;
    Data->AcceptedOptionalLakes = Asset->AcceptedOptionalLakes;

    if (Asset->SettingsHash != BuildSettingsHash())
    {
        UE_LOG(
            LogTemp,
            Warning,
            TEXT("Avenor baked terrain settings differ from the generator actor. The baked geography remains authoritative until Generate and Bake Geography is run explicitly.")
        );
    }
    return Data;
}

bool AAvenorStripTerrainGenerator::BindModifiersAndRefresh(bool bShowFailureDialog)
{
#if WITH_EDITOR
    UE::MeshPartition::AMeshPartition* TargetMeshPartition =
        Cast<UE::MeshPartition::AMeshPartition>(MeshPartitionActor);
    if (!TargetMeshPartition || !TerrainModifier)
    {
        if (bShowFailureDialog)
        {
            FMessageDialog::Open(
                EAppMsgType::Ok,
                FText::FromString(TEXT("Assign a valid Mesh Partition actor before refreshing terrain."))
            );
        }
        return false;
    }

    TerrainModifier->Modify();
    TerrainModifier->SetAffectedMeshPartition(nullptr);
    TerrainModifier->BP_SetAffectedMegaMesh(TargetMeshPartition);
    const TArray<FName> PriorityLayers = TerrainModifier->GetDefinitionPriorityLayers();
    if (PriorityLayers.Num() < 2)
    {
        if (bShowFailureDialog)
        {
            FMessageDialog::Open(
                EAppMsgType::Ok,
                FText::FromString(TEXT(
                    "The Mesh Partition Definition needs at least two Modifier Priority Layers. "
                    "Avenor uses the first for broad terrain and the last for refinement and water."
                ))
            );
        }
        return false;
    }
    TerrainModifier->SetPriorityLayer(PriorityLayers[0]);
    TerrainModifier->SetPriority(0.0);
    TerrainModifier->PostEditChange();

    const FName OwnerTag = MakeWaterOwnerTag(*this);
    int32 BoundRefinementModifiers = 0;
    if (UWorld* World = GetWorld())
    {
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (!It->Tags.Contains(GeneratedRefinementTag) || !It->Tags.Contains(OwnerTag))
            {
                continue;
            }
            TInlineComponentArray<UE::MeshPartition::USplineRemeshModifier*> Modifiers;
            It->GetComponents(Modifiers);
            for (UE::MeshPartition::USplineRemeshModifier* Modifier : Modifiers)
            {
                Modifier->Modify();
                Modifier->UpdateSplineData();
                Modifier->SetAffectedMeshPartition(nullptr);
                Modifier->BP_SetAffectedMegaMesh(TargetMeshPartition);
                Modifier->SetPriorityLayer(PriorityLayers.Last());
                Modifier->SetPriority(0.0);
                ++BoundRefinementModifiers;
            }
        }
    }

    int32 BoundWaterModifiers = 0;
    if (UWorld* World = GetWorld())
    {
        for (TActorIterator<AWaterBody> It(World); It; ++It)
        {
            if (!It->Tags.Contains(GeneratedWaterTag) || !It->Tags.Contains(OwnerTag))
            {
                continue;
            }
            TInlineComponentArray<UE::MeshPartition::UModifierComponent*> Modifiers;
            It->GetComponents(Modifiers);
            for (UE::MeshPartition::UModifierComponent* Modifier : Modifiers)
            {
                Modifier->Modify();
                Modifier->SetAffectedMeshPartition(nullptr);
                Modifier->BP_SetAffectedMegaMesh(TargetMeshPartition);
                Modifier->SetPriorityLayer(PriorityLayers.Last());
                Modifier->SetPriority(10.0);
                Modifier->PostEditChange();
                ++BoundWaterModifiers;
            }
        }
    }

    TargetMeshPartition->Modify();
    TargetMeshPartition->PostEditChange();
    TargetMeshPartition->ReregisterAllComponents();
    TargetMeshPartition->MarkPackageDirty();
    MarkPackageDirty();
    if (GEditor)
    {
        GEditor->RedrawLevelEditingViewports(true);
    }
    UE_LOG(
        LogTemp,
        Display,
        TEXT("Avenor refreshed Mesh Partition in-place with broad terrain, %d refinement modifiers and %d native water modifiers."),
        BoundRefinementModifiers,
        BoundWaterModifiers
    );
    return true;
#else
    return false;
#endif
}

void AAvenorStripTerrainGenerator::ClearGeneratedWater()
{
#if WITH_EDITOR
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    const FName OwnerTag = MakeWaterOwnerTag(*this);
    TArray<AWaterBody*> ToDelete;
    for (TActorIterator<AWaterBody> It(World); It; ++It)
    {
        if (!It->Tags.Contains(GeneratedWaterTag))
        {
            continue;
        }
        bool bHasAnyOwnerTag = false;
        for (const FName& Tag : It->Tags)
        {
            bHasAnyOwnerTag |= IsWaterOwnerTag(Tag);
        }
        if (It->Tags.Contains(OwnerTag) || !bHasAnyOwnerTag)
        {
            ToDelete.Add(*It);
        }
    }
    for (AWaterBody* Water : ToDelete)
    {
        World->EditorDestroyActor(Water, true);
    }
#endif
}

// ConfigureWaterTerrainSettings expects the transition width outside the
// WaterBody spline edge, not a total radius from the centreline. Passing
// half the water width again here double-counted the channel and produced a
// broad secondary trench. The water spline owns its wet width; this helper
// supplies only a modest terrain shoulder beyond it.
static double ComputeRiverBankTransitionWidth(const FRiverReach& Reach)
{
    if (Reach.bIsCanyon)
    {
        return FMath::Clamp(Reach.Width * 0.18, 150.0, 800.0);
    }
    return FMath::Clamp(Reach.Width * 0.08, 100.0, 400.0);
}

void AAvenorStripTerrainGenerator::CreateWaterActors(const TSharedPtr<const FAvenorStripData>& Data)
{
#if WITH_EDITOR
    UWorld* World = GetWorld();
    UE::MeshPartition::AMeshPartition* TargetMeshPartition =
        Cast<UE::MeshPartition::AMeshPartition>(MeshPartitionActor);
    if (!World || !Data || !TargetMeshPartition || !TerrainModifier)
    {
        return;
    }
    const TArray<FName> PriorityLayers = TerrainModifier->GetDefinitionPriorityLayers();
    if (PriorityLayers.Num() < 2)
    {
        return;
    }
    const FName OwnerTag = MakeWaterOwnerTag(*this);
    auto ToWorldPoints = [&](const TArray<FVector>& Source)
    {
        TArray<FVector> Result = Source;
        for (FVector& Point : Result)
        {
            Point.Z += GetActorLocation().Z;
        }
        return Result;
    };
    if (Data->OceanBoundary.Num() >= 4)
    {
        if (AWaterBodyOcean* Ocean = SpawnWaterActor<AWaterBodyOcean>(
            *World, TEXT("Avenor_Strip_Ocean"), OwnerTag))
        {
            ConfigureWaterSpline(*Ocean->GetWaterBodyComponent()->GetWaterSpline(), ToWorldPoints(Data->OceanBoundary), true);
            Ocean->PostEditChange();
        }
    }
    for (int32 Index = 0; Index < Data->Lakes.Num(); ++Index)
    {
        TArray<FVector> LocalShoreline = Data->Lakes[Index].Shoreline;
        for (FVector& Point : LocalShoreline)
        {
            Point.Z = Data->Lakes[Index].SurfaceHeight;
        }
        TArray<FVector> LakePoints = ToWorldPoints(LocalShoreline);
        if (AWaterBodyLake* Lake = SpawnWaterActor<AWaterBodyLake>(
            *World,
            FString::Printf(TEXT("Avenor_Strip_Lake_%02d"), Index + 1),
            OwnerTag))
        {
            ConfigureWaterSpline(*Lake->GetWaterBodyComponent()->GetWaterSpline(), LakePoints, true);
            Lake->GetWaterBodyComponent()->GetWaterSpline()->K2_SynchronizeAndBroadcastDataChange();
            ConfigureWaterTerrainSettings(
                *Lake, true, Data->Lakes[Index].ModifierBedDepth,
                Data->Lakes[Index].BankBlendWidth, WaterTerrain
            );
            Lake->PostEditChange();
        }
    }
    for (int32 Index = 0; Index < Data->Rivers.Num(); ++Index)
    {
        const FRiverReach& Reach = Data->Rivers[Index];
        TArray<FVector> RiverPoints = ToWorldPoints(Reach.Points);
        if (AWaterBodyRiver* River = SpawnWaterActor<AWaterBodyRiver>(
            *World,
            FString::Printf(TEXT("Avenor_Strip_River_%03d"), Index + 1),
            OwnerTag))
        {
            ConfigureRiverSpline(
                *River->GetWaterBodyComponent()->GetWaterSpline(), RiverPoints,
                Reach.Width, Reach.Depth
            );
            // Diagnostic mode: deliberately do not attach a native River
            // Modifier or perform a second-stage channel carve. The base
            // erosion pass owns the valley for now; the visible Water Body
            // and its refinement spline let us validate routing, Z and width
            // independently. A shallow bed modifier can be reintroduced once
            // those source geometries are proven correct.
            River->PostEditChange();
        }
    }
#endif
}

void AAvenorStripTerrainGenerator::GenerateTerrain()
{
#if WITH_EDITOR
    ResolveSettings();
    UE::MeshPartition::AMeshPartition* TargetMeshPartition =
        Cast<UE::MeshPartition::AMeshPartition>(MeshPartitionActor);
    if (!TargetMeshPartition)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(TEXT(
                "Assign a valid Mesh Partition actor to MeshPartitionActor "
                "before generating terrain."
            ))
        );
        return;
    }
    if (!TerrainModifier)
    {
        return;
    }

    TerrainModifier->SetAffectedMeshPartition(nullptr);
    TerrainModifier->BP_SetAffectedMegaMesh(TargetMeshPartition);
    const TArray<FName> PriorityLayers = TerrainModifier->GetDefinitionPriorityLayers();
    if (PriorityLayers.Num() < 2)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(TEXT(
                "The Mesh Partition Definition needs at least two Modifier "
                "Priority Layers. Avenor uses the first for the broad eroded "
                "terrain and the last for refinement plus native water carving."
            ))
        );
        return;
    }
    TerrainModifier->SetPriorityLayer(PriorityLayers[0]);
    TerrainModifier->SetPriority(0.0);

    FScopedSlowTask Progress(3.0f, FText::FromString(TEXT("Loading baked strip terrain plan...")));
    Progress.MakeDialog();
    Progress.EnterProgressFrame(1.0f, FText::FromString(TEXT("Loading compressed geography chunks")));
    bTerrainPlanReadyForWater = false;
    ClearGeneratedWater();
    ClearGeneratedRefinementSplines();
    const TSharedPtr<const FAvenorStripData> Data = GetOrCreateData();
    if (!Data)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(TEXT(
                "No valid baked Avenor terrain data is assigned. Use Generate and Bake Geography first."
            ))
        );
        return;
    }
    Progress.EnterProgressFrame(1.0f, FText::FromString(TEXT("Registering ordered terrain modifier")));
    Progress.EnterProgressFrame(1.0f, FText::FromString(TEXT("Submitting terrain modifier")));

    bTerrainPlanReadyForWater = true;

    if (!bDeferMeshRefresh)
    {
        bTerrainPlanReadyForWater = BindModifiersAndRefresh(true);
    }

    LastBuildStamp = FString::Printf(
        TEXT("Terrain submitted %s | code %s | seed %d | %d cells @ %.0fcm | structural relief %.0fcm @ %.0fcm scale | river seeds %d + %d downstream cells = %d channel cells | short systems rejected %d | %d river reaches | lakes %d (%d terminal + %d optional; %d terminal candidates) | NEXT: GENERATE REFINEMENT SPLINES"),
        *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
        *FStripTerrainOp::Version().ToString(EGuidFormats::Short),
        Seed, Data->Height.Num(), Data->CellSize,
        StructuralRelief, StructuralScale,
        Data->RiverSeedCells, Data->RiverContinuationCells,
        Data->AuthoritativeRiverCells, Data->RejectedShortRiverSystems,
        Data->Rivers.Num(), Data->Lakes.Num(),
        Data->AcceptedRiverTerminusLakes, Data->AcceptedOptionalLakes,
        Data->RiverTerminusLakeCandidates
    );
    UE_LOG(LogTemp, Display, TEXT("Avenor strip terrain submitted: %s"), *LastBuildStamp);
    if (!bDeferMeshRefresh)
    {
        ReleaseCachedData();
    }
#endif
}

void AAvenorStripTerrainGenerator::ClearGeneratedRefinementSplines()
{
#if WITH_EDITOR
    bRefinementPlanReadyForWater = false;
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    const FName OwnerTag = MakeWaterOwnerTag(*this);
    TArray<AActor*> ToDelete;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (!It->Tags.Contains(GeneratedRefinementTag))
        {
            continue;
        }
        bool bHasAnyOwnerTag = false;
        for (const FName& Tag : It->Tags)
        {
            bHasAnyOwnerTag |= IsWaterOwnerTag(Tag);
        }
        if (It->Tags.Contains(OwnerTag) || !bHasAnyOwnerTag)
        {
            ToDelete.Add(*It);
        }
    }
    for (AActor* RefinementActor : ToDelete)
    {
        World->EditorDestroyActor(RefinementActor, true);
    }
#endif
}

#if WITH_EDITOR
static bool SpawnRefinementSpline(
    UWorld& World,
    const FString& Label,
    FName OwnerTag,
    FName RefinementTag,
    const TArray<FVector>& WorldPoints,
    bool bClosed,
    double TargetEdgeLength,
    double CoverageRadius,
    int32 MaxTessellationLevel
)
{
    if (WorldPoints.Num() < 2)
    {
        return false;
    }
    AActor* RefinementActor = World.SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity);
    if (!RefinementActor)
    {
        return false;
    }
    RefinementActor->SetActorLabel(Label);
    RefinementActor->SetFolderPath(TEXT("Avenor/Generated/StripRefinement"));
    RefinementActor->Tags.AddUnique(RefinementTag);
    RefinementActor->Tags.AddUnique(OwnerTag);

    USplineComponent* SplineComp = NewObject<USplineComponent>(
        RefinementActor, USplineComponent::StaticClass(), NAME_None, RF_Transactional
    );
    RefinementActor->SetRootComponent(SplineComp);
    RefinementActor->AddInstanceComponent(SplineComp);
    SplineComp->RegisterComponent();
    SplineComp->ClearSplinePoints(false);
    SplineComp->SetSplinePoints(WorldPoints, ESplineCoordinateSpace::World, false);
    SplineComp->SetClosedLoop(bClosed, false);
    for (int32 PointIndex = 0; PointIndex < SplineComp->GetNumberOfSplinePoints(); ++PointIndex)
    {
        SplineComp->SetSplinePointType(PointIndex, ESplinePointType::Curve, false);
    }
    SplineComp->UpdateSpline();

    UE::MeshPartition::USplineRemeshModifier* Modifier = NewObject<UE::MeshPartition::USplineRemeshModifier>(
        RefinementActor, UE::MeshPartition::USplineRemeshModifier::StaticClass(), NAME_None, RF_Transactional
    );
    Modifier->SetupAttachment(SplineComp);
    RefinementActor->AddInstanceComponent(Modifier);

    Modifier->SetCurrentOperation(UE::MeshPartition::ERemeshModifierOperation::Tessellate);
    Modifier->SetUseTargetEdgeLength(true);
    Modifier->SetTessellationTargetEdgeLength(static_cast<float>(TargetEdgeLength));
    Modifier->SetMaxTessellationLevel(MaxTessellationLevel);
    Modifier->SetSplineComponent(SplineComp, true);
    if (!SetSplineRemeshRadius(*Modifier, static_cast<float>(CoverageRadius)))
    {
        World.EditorDestroyActor(RefinementActor, true);
        return false;
    }
    Modifier->RegisterComponent();
    Modifier->UpdateSplineData();
    return true;
}
#endif

void AAvenorStripTerrainGenerator::GenerateRefinementSplines()
{
#if WITH_EDITOR
    if (!bTerrainPlanReadyForWater)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(TEXT(
                "A valid baked terrain plan must be loaded first. No intermediate "
                "Mesh Partition rebuild is required before generating refinement splines."
            ))
        );
        return;
    }
    UWorld* World = GetWorld();
    UE::MeshPartition::AMeshPartition* TargetMeshPartition =
        Cast<UE::MeshPartition::AMeshPartition>(MeshPartitionActor);
    if (!World || !TargetMeshPartition)
    {
        return;
    }
    if (!TargetMeshPartition->GetActorTransform().Equals(FTransform::Identity))
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(TEXT(
                "UE 5.8.1 Spline Remesh has a coordinate-space defect when the "
                "Mesh Partition actor has a non-identity transform. Keep the "
                "Mesh Partition actor at world origin with zero rotation and "
                "unit scale before generating refinement splines."
            ))
        );
        return;
    }
    if (!TerrainModifier)
    {
        return;
    }
    const TArray<FName> PriorityLayers = TerrainModifier->GetDefinitionPriorityLayers();
    if (PriorityLayers.Num() < 2)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(TEXT(
                "The Mesh Partition Definition needs at least two Modifier "
                "Priority Layers before refinement can be generated."
            ))
        );
        return;
    }
    TerrainModifier->SetPriorityLayer(PriorityLayers[0]);

    const TSharedPtr<const FAvenorStripData> Data = GetOrCreateData();
    if (!Data)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(TEXT(
                "The baked Avenor terrain data could not be loaded for refinement."
            ))
        );
        return;
    }

    FScopedSlowTask Progress(2.0f, FText::FromString(TEXT("Generating refinement splines...")));
    Progress.MakeDialog();
    Progress.EnterProgressFrame(1.0f, FText::FromString(TEXT("Replacing this generator's refinement splines")));
    ClearGeneratedRefinementSplines();
    bRefinementPlanReadyForWater = false;
    Progress.EnterProgressFrame(1.0f, FText::FromString(TEXT("Placing spline remesh modifiers")));

    const FName OwnerTag = MakeWaterOwnerTag(*this);
    const double SourceMeshWorldZ = TargetMeshPartition->GetActorLocation().Z;
    auto ToRemeshPoints = [&](const TArray<FVector>& Source)
    {
        TArray<FVector> Result = Source;
        for (FVector& Point : Result)
        {
            Point.Z = SourceMeshWorldZ;
        }
        return Result;
    };

    int32 CreatedRiverSplines = 0;
    int32 CreatedLakeSplines = 0;
    for (int32 Index = 0; Index < Data->Rivers.Num(); ++Index)
    {
        const FRiverReach& River = Data->Rivers[Index];
        const double EdgeLength = River.bIsCanyon
            ? RefinementEdgeLengthCanyon
            : FMath::Lerp(
                RefinementEdgeLengthHeadwater, RefinementEdgeLengthMainRiver,
                FMath::Clamp(River.DrainageArea / FMath::Max(0.01, MainRiverArea), 0.0, 1.0)
            );
        // Cover the wet channel and its bank transition. Otherwise the outer
        // falloff ramp lands on the coarse base mesh and produces faceted,
        // terraced banks.
        const double CarveCoverageRadius =
            River.Width * 0.5 + ComputeRiverBankTransitionWidth(River);
        const double CoverageRadius = River.bIsCanyon
            ? FMath::Min(
                CarveCoverageRadius + RefinementCoverageMargin,
                RefinementMaximumCanyonRadius
            )
            : CarveCoverageRadius + RefinementCoverageMargin;
        if (SpawnRefinementSpline(
            *World,
            FString::Printf(TEXT("Avenor_Strip_Refine_River_%03d"), Index + 1),
            OwnerTag, GeneratedRefinementTag,
            ToRemeshPoints(River.Points), false,
            EdgeLength, CoverageRadius,
            RefinementMaxTessellationLevel
        ))
        {
            ++CreatedRiverSplines;
        }
    }
    for (int32 Index = 0; Index < Data->Lakes.Num(); ++Index)
    {
        const FLakeBasin& Lake = Data->Lakes[Index];
        const double ShoreRadius = FMath::Max(
            RefinementCoverageMargin,
            Data->CellSize * 0.6
        );
        if (SpawnRefinementSpline(
            *World,
            FString::Printf(TEXT("Avenor_Strip_Refine_Lake_%02d"), Index + 1),
            OwnerTag, GeneratedRefinementTag,
            ToRemeshPoints(Lake.Shoreline), true,
            RefinementEdgeLengthLakeShore, ShoreRadius,
            RefinementMaxTessellationLevel
        ))
        {
            ++CreatedLakeSplines;
        }
    }

    bRefinementPlanReadyForWater =
        CreatedRiverSplines == Data->Rivers.Num() &&
        CreatedLakeSplines == Data->Lakes.Num();

    if (bRefinementPlanReadyForWater && !bDeferMeshRefresh)
    {
        bRefinementPlanReadyForWater = BindModifiersAndRefresh(true);
    }

    LastBuildStamp = FString::Printf(
        TEXT("Refinement splines placed %s | code %s | %d/%d river reaches | %d/%d lake shores"),
        *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
        *FStripTerrainOp::Version().ToString(EGuidFormats::Short),
        CreatedRiverSplines, Data->Rivers.Num(),
        CreatedLakeSplines, Data->Lakes.Num()
    );
    UE_LOG(LogTemp, Display, TEXT("Avenor strip refinement splines placed: %s"), *LastBuildStamp);
    if (!bRefinementPlanReadyForWater)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(FString::Printf(
                TEXT("Some refinement modifiers could not be created (%d/%d rivers and %d/%d lakes succeeded). Check the Output Log before rebuilding terrain."),
                CreatedRiverSplines, Data->Rivers.Num(),
                CreatedLakeSplines, Data->Lakes.Num()
            ))
        );
    }
    if (!bDeferMeshRefresh)
    {
        ReleaseCachedData();
    }
#endif
}

void AAvenorStripTerrainGenerator::RegenerateAndRefreshTerrain()
{
#if WITH_EDITOR
    RebuildWorldFromBakedData();
#endif
}

void AAvenorStripTerrainGenerator::GenerateCompleteWorld()
{
#if WITH_EDITOR
    ResolveSettings();
    InvalidateData();
    bGeneratingGeography = true;
    const TSharedPtr<const FAvenorStripData> GeneratedData = GenerateData(*this);
    bGeneratingGeography = false;
    if (!GeneratedData)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(TEXT("Avenor geography generation failed; the previous baked asset was not changed."))
        );
        return;
    }
    if (!BakeData(GeneratedData))
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(TEXT("Avenor geography was calculated but could not be saved to its terrain data asset."))
        );
        return;
    }
    {
        FScopeLock Lock(&DataMutex);
        CachedData = GeneratedData;
    }
    const int32 GeneratedRiverCount = GeneratedData->Rivers.Num();
    const int32 GeneratedLakeCount = GeneratedData->Lakes.Num();
    BuildCompleteWorldFromCurrentData();
    LastBuildStamp = FString::Printf(
        TEXT("Geography generated and baked %s | asset %s | hash %s | %d compressed chunks | %d rivers | %d lakes"),
        *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
        *BakedTerrainData.ToSoftObjectPath().ToString(),
        *BuildSettingsHash(),
        BakedTerrainData.LoadSynchronous() ? BakedTerrainData.LoadSynchronous()->Chunks.Num() : 0,
        GeneratedRiverCount,
        GeneratedLakeCount
    );
    ReleaseCachedData();
#endif
}

void AAvenorStripTerrainGenerator::BuildCompleteWorldFromCurrentData()
{
#if WITH_EDITOR
    if (!CachedData)
    {
        return;
    }
    bDeferMeshRefresh = true;
    ClearGeneratedWater();
    ClearGeneratedRefinementSplines();
    GenerateTerrain();
    if (bTerrainPlanReadyForWater)
    {
        GenerateRefinementSplines();
    }
    if (bTerrainPlanReadyForWater && bRefinementPlanReadyForWater)
    {
        CreateWaterActors(CachedData);
    }
    bDeferMeshRefresh = false;

    if (!bTerrainPlanReadyForWater || !bRefinementPlanReadyForWater)
    {
        return;
    }
    bRefinementPlanReadyForWater = BindModifiersAndRefresh(true);
    LastBuildStamp = FString::Printf(
        TEXT("World rebuilt from baked geography %s | code %s | seed %d | %d rivers | %d lakes | native water modifiers bound"),
        *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
        *FStripTerrainOp::Version().ToString(EGuidFormats::Short),
        Seed,
        CachedData ? CachedData->Rivers.Num() : 0,
        CachedData ? CachedData->Lakes.Num() : 0
    );
#endif
}

void AAvenorStripTerrainGenerator::RebuildWorldFromBakedData()
{
#if WITH_EDITOR
    ResolveSettings();
    InvalidateData();
    if (!GetOrCreateData())
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(TEXT(
                "No valid baked Avenor terrain data is assigned. Generate and Bake Geography first."
            ))
        );
        return;
    }
    if (const UAvenorTerrainData* Asset = BakedTerrainData.Get())
    {
        BakedDataStatus = Asset->SettingsHash == BuildSettingsHash()
            ? FString::Printf(TEXT("Current: %s"), *Asset->SettingsHash)
            : FString::Printf(
                TEXT("BAKED TERRAIN IS OUT OF DATE: saved %s, current settings %s"),
                *Asset->SettingsHash,
                *BuildSettingsHash()
            );
    }
    BuildCompleteWorldFromCurrentData();
    ReleaseCachedData();
#endif
}

void AAvenorStripTerrainGenerator::RegenerateWaterFromBakedData()
{
#if WITH_EDITOR
    ResolveSettings();
    InvalidateData();
    if (!GetOrCreateData())
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(TEXT(
                "No valid baked Avenor terrain data is assigned. Generate and Bake Geography first."
            ))
        );
        return;
    }
    if (const UAvenorTerrainData* Asset = BakedTerrainData.Get())
    {
        BakedDataStatus = Asset->SettingsHash == BuildSettingsHash()
            ? FString::Printf(TEXT("Current: %s"), *Asset->SettingsHash)
            : FString::Printf(
                TEXT("BAKED TERRAIN IS OUT OF DATE: saved %s, current settings %s"),
                *Asset->SettingsHash,
                *BuildSettingsHash()
            );
    }
    const TSharedPtr<const FAvenorStripData> Data = CachedData;
    bTerrainPlanReadyForWater = true;
    bDeferMeshRefresh = true;
    ClearGeneratedWater();
    ClearGeneratedRefinementSplines();
    GenerateRefinementSplines();
    if (bRefinementPlanReadyForWater)
    {
        CreateWaterActors(Data);
    }
    bDeferMeshRefresh = false;
    if (bRefinementPlanReadyForWater)
    {
        bRefinementPlanReadyForWater = BindModifiersAndRefresh(true);
    }
    LastBuildStamp = FString::Printf(
        TEXT("Water regenerated from baked geography %s | %d rivers | %d lakes"),
        *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
        Data ? Data->Rivers.Num() : 0,
        Data ? Data->Lakes.Num() : 0
    );
    ReleaseCachedData();
#endif
}

void AAvenorStripTerrainGenerator::ClearGeneratedWorld()
{
#if WITH_EDITOR
    ClearGeneratedWater();
    ClearGeneratedRefinementSplines();
    ClearFastPreview();
    InvalidateData();
    LastBuildStamp = TEXT("Generated Avenor water, refinement and preview cleared");
    if (TerrainModifier)
    {
        TerrainModifier->SetAffectedMeshPartition(nullptr);
    }
    if (UE::MeshPartition::AMeshPartition* TargetMeshPartition =
        Cast<UE::MeshPartition::AMeshPartition>(MeshPartitionActor))
    {
        TargetMeshPartition->PostEditChange();
        TargetMeshPartition->ReregisterAllComponents();
    }
#endif
}

void AAvenorStripTerrainGenerator::RefreshMeshTerrainInPlace()
{
#if WITH_EDITOR
    BindModifiersAndRefresh(true);
#endif
}

void AAvenorStripTerrainGenerator::ClearFastPreview()
{
#if WITH_EDITOR
    if (FastPreviewMesh)
    {
        FastPreviewMesh->ClearAllMeshSections();
        FastPreviewMesh->SetVisibility(false);
    }
#endif
}

void AAvenorStripTerrainGenerator::GenerateFastPreview()
{
#if WITH_EDITOR
    ResolveSettings();
    if (!FastPreviewMesh)
    {
        return;
    }

    const double RequestedSpacing = FMath::Max(500.0, PreviewVertexSpacing);
    const double SizeX = FMath::Max(10000.0, PreviewSize.X);
    const double SizeY = FMath::Max(10000.0, PreviewSize.Y);
    const int32 Columns = FMath::CeilToInt(SizeX / RequestedSpacing) + 1;
    const int32 Rows = FMath::CeilToInt(SizeY / RequestedSpacing) + 1;
    constexpr int32 MaximumPreviewVertices = 250000;
    if (Columns < 2 || Rows < 2 || static_cast<int64>(Columns) * Rows > MaximumPreviewVertices)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(FString::Printf(
                TEXT("Fast preview would create %lld vertices. Increase Preview Vertex Spacing or reduce Preview Size (limit: %d vertices)."),
                static_cast<int64>(Columns) * Rows,
                MaximumPreviewVertices
            ))
        );
        return;
    }

    FScopedSlowTask Progress(2.0f, FText::FromString(TEXT("Generating fast local terrain preview...")));
    Progress.MakeDialog();
    Progress.EnterProgressFrame(1.0f, FText::FromString(TEXT("Calculating terrain and hydrology")));
    bGeneratingGeography = true;
    const TSharedPtr<const FAvenorStripData> Data = GenerateData(*this);
    bGeneratingGeography = false;
    if (!Data)
    {
        return;
    }

    Progress.EnterProgressFrame(1.0f, FText::FromString(TEXT("Building lightweight preview mesh")));
    const double StepX = SizeX / static_cast<double>(Columns - 1);
    const double StepY = SizeY / static_cast<double>(Rows - 1);
    const FVector ActorLocation = GetActorLocation();
    TArray<FVector> Vertices;
    TArray<int32> Triangles;
    TArray<FVector> Normals;
    TArray<FVector2D> UVs;
    TArray<FLinearColor> VertexColors;
    TArray<FProcMeshTangent> Tangents;
    Vertices.SetNumUninitialized(Columns * Rows);
    Normals.SetNumUninitialized(Columns * Rows);
    UVs.SetNumUninitialized(Columns * Rows);
    VertexColors.SetNumUninitialized(Columns * Rows);
    Triangles.Reserve((Columns - 1) * (Rows - 1) * 6);

    TArray<double> Heights;
    Heights.SetNumUninitialized(Columns * Rows);
    double MinimumHeight = TNumericLimits<double>::Max();
    double MaximumHeight = -TNumericLimits<double>::Max();
    auto GridIndex = [Columns](int32 X, int32 Y) { return Y * Columns + X; };
    for (int32 Y = 0; Y < Rows; ++Y)
    {
        for (int32 X = 0; X < Columns; ++X)
        {
            const int32 Index = GridIndex(X, Y);
            const double LocalX = PreviewCentreOffset.X - SizeX * 0.5 + X * StepX;
            const double LocalY = PreviewCentreOffset.Y - SizeY * 0.5 + Y * StepY;
            const double Height = Data->SampleHeight(FVector2D(
                ActorLocation.X + LocalX,
                ActorLocation.Y + LocalY
            ));
            Heights[Index] = Height;
            MinimumHeight = FMath::Min(MinimumHeight, Height);
            MaximumHeight = FMath::Max(MaximumHeight, Height);
            Vertices[Index] = FVector(LocalX, LocalY, Height + PreviewDisplayOffsetZ);
            UVs[Index] = FVector2D(
                static_cast<double>(X) / (Columns - 1),
                static_cast<double>(Y) / (Rows - 1)
            );
        }
    }

    for (int32 Y = 0; Y < Rows; ++Y)
    {
        for (int32 X = 0; X < Columns; ++X)
        {
            const int32 Index = GridIndex(X, Y);
            const double Left = Heights[GridIndex(FMath::Max(0, X - 1), Y)];
            const double Right = Heights[GridIndex(FMath::Min(Columns - 1, X + 1), Y)];
            const double Down = Heights[GridIndex(X, FMath::Max(0, Y - 1))];
            const double Up = Heights[GridIndex(X, FMath::Min(Rows - 1, Y + 1))];
            Normals[Index] = FVector(Left - Right, Down - Up, StepX + StepY).GetSafeNormal();
            const float HeightAlpha = static_cast<float>(FMath::GetRangePct(
                MinimumHeight,
                FMath::Max(MinimumHeight + 1.0, MaximumHeight),
                Heights[Index]
            ));
            VertexColors[Index] = FLinearColor::LerpUsingHSV(
                FLinearColor(0.08f, 0.22f, 0.07f),
                FLinearColor(0.72f, 0.72f, 0.68f),
                HeightAlpha
            );
        }
    }
    for (int32 Y = 0; Y + 1 < Rows; ++Y)
    {
        for (int32 X = 0; X + 1 < Columns; ++X)
        {
            const int32 A = GridIndex(X, Y);
            const int32 B = GridIndex(X + 1, Y);
            const int32 C = GridIndex(X, Y + 1);
            const int32 D = GridIndex(X + 1, Y + 1);
            Triangles.Add(A);
            Triangles.Add(B);
            Triangles.Add(D);
            Triangles.Add(A);
            Triangles.Add(D);
            Triangles.Add(C);
        }
    }

    FastPreviewMesh->ClearAllMeshSections();
    FastPreviewMesh->CreateMeshSection_LinearColor(
        0, Vertices, Triangles, Normals, UVs, VertexColors, Tangents, false
    );
    FastPreviewMesh->SetVisibility(true);
    FastPreviewMesh->MarkRenderStateDirty();
    if (GEditor)
    {
        GEditor->RedrawLevelEditingViewports(true);
    }
    LastBuildStamp = FString::Printf(
        TEXT("Fast preview generated %s | %.1f x %.1f km | %d vertices | production Mesh Partition unchanged"),
        *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
        SizeX / 100000.0,
        SizeY / 100000.0,
        Vertices.Num()
    );
#endif
}

void AAvenorStripTerrainGenerator::GenerateWater()
{
#if WITH_EDITOR
    if (!bTerrainPlanReadyForWater || !bRefinementPlanReadyForWater)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(TEXT(
                "A terrain and refinement plan is required before water can be created. "
                "Use Generate and Bake Geography for the normal one-button workflow."
            ))
        );
        return;
    }
    const TSharedPtr<const FAvenorStripData> Data = GetOrCreateData();
    if (!Data)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(TEXT(
                "The baked Avenor terrain data could not be loaded for water generation."
            ))
        );
        return;
    }

    FScopedSlowTask Progress(2.0f, FText::FromString(TEXT("Generating strip water...")));
    Progress.MakeDialog();
    Progress.EnterProgressFrame(1.0f, FText::FromString(TEXT("Replacing this generator's water")));
    ClearGeneratedWater();
    Progress.EnterProgressFrame(1.0f, FText::FromString(TEXT("Creating lakes and rivers")));
    CreateWaterActors(Data);
    BindModifiersAndRefresh(true);

    LastBuildStamp = FString::Printf(
        TEXT("Native water created %s | code %s | seed %d | %d rivers | %d lakes"),
        *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
        *FStripTerrainOp::Version().ToString(EGuidFormats::Short),
        Seed, Data->Rivers.Num(), Data->Lakes.Num()
    );
    UE_LOG(LogTemp, Display, TEXT("Avenor strip water generated: %s"), *LastBuildStamp);
    ReleaseCachedData();
#endif
}

#if WITH_EDITOR
void AAvenorStripTerrainGenerator::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    ResolveSettings();
    InvalidateData();
    Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif
