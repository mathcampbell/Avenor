#include "AvenorWorldGenerator.h"

#include "ActorFactories/ActorFactory.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "MeshPartition.h"
#include "Misc/ScopedSlowTask.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyOceanActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterSplineComponent.h"

#include <queue>
#include <vector>

struct FAvenorRiverDefinition
{
    TArray<FVector> Points;
    TArray<FVector> SplinePoints;
    double DischargeSquareKilometres = 0.0;
    double Width = 500.0;
    double Depth = 300.0;
    double ValleyHalfWidth = 12000.0;
    double CrossSectionExponent = 1.0;
    FBox2D InfluenceBounds = FBox2D(ForceInit);
};

struct FAvenorLakeDefinition
{
    TArray<FVector> Shoreline;
    double SurfaceHeight = 0.0;
    double MaximumDepth = 500.0;
    double FalloffWidth = 24000.0;
    double CatchmentSquareKilometres = 0.0;
    FBox2D InfluenceBounds = FBox2D(ForceInit);
};

struct FAvenorGeneratedWorld
{
    FBox Bounds = FBox(ForceInit);
    int32 Columns = 0;
    int32 Rows = 0;
    double CellSize = 10000.0;
    TArray<double> TerrainHeight;
    TArray<double> FilledHeight;
    TArray<double> Accumulation;
    TArray<int32> Downstream;
    TArray<int32> FillParent;
    TArray<int32> LakeIndexByCell;
    TArray<FAvenorRiverDefinition> Rivers;
    TArray<FAvenorLakeDefinition> Lakes;
    TArray<FVector> OceanBoundary;

    int32 Index(int32 X, int32 Y) const
    {
        return Y * Columns + X;
    }

    bool IsValid(int32 X, int32 Y) const
    {
        return X >= 0 && X < Columns && Y >= 0 && Y < Rows;
    }

    bool IsBoundary(int32 X, int32 Y) const
    {
        return X == 0 || Y == 0 ||
            X == Columns - 1 || Y == Rows - 1;
    }

    FVector2D Position(int32 Cell) const
    {
        const int32 X = Cell % Columns;
        const int32 Y = Cell / Columns;
        return FVector2D(
            Bounds.Min.X + (static_cast<double>(X) + 0.5) * CellSize,
            Bounds.Min.Y + (static_cast<double>(Y) + 0.5) * CellSize
        );
    }

    double CellAreaSquareKilometres() const
    {
        return CellSize * CellSize / 10000000000.0;
    }

    double SampleGrid(
        const TArray<double>& Values,
        const FVector2D& WorldPosition
    ) const
    {
        if (Values.IsEmpty() || Columns < 2 || Rows < 2)
        {
            return 0.0;
        }
        const double GridX =
            (WorldPosition.X - Bounds.Min.X) / CellSize - 0.5;
        const double GridY =
            (WorldPosition.Y - Bounds.Min.Y) / CellSize - 0.5;
        const int32 X0 = FMath::Clamp(
            FMath::FloorToInt(GridX), 0, Columns - 1
        );
        const int32 Y0 = FMath::Clamp(
            FMath::FloorToInt(GridY), 0, Rows - 1
        );
        const int32 X1 = FMath::Min(X0 + 1, Columns - 1);
        const int32 Y1 = FMath::Min(Y0 + 1, Rows - 1);
        const double AX = FMath::Clamp(GridX - X0, 0.0, 1.0);
        const double AY = FMath::Clamp(GridY - Y0, 0.0, 1.0);
        return FMath::Lerp(
            FMath::Lerp(
                Values[Index(X0, Y0)],
                Values[Index(X1, Y0)],
                AX
            ),
            FMath::Lerp(
                Values[Index(X0, Y1)],
                Values[Index(X1, Y1)],
                AX
            ),
            AY
        );
    }

    double SampleTerrainHeight(const FVector2D& Point) const
    {
        const double UnderlyingHeight = SampleGrid(TerrainHeight, Point);
        double Result = UnderlyingHeight;

        for (const FAvenorLakeDefinition& Lake : Lakes)
        {
            if (!Lake.InfluenceBounds.IsInside(Point) ||
                Lake.Shoreline.Num() < 3)
            {
                continue;
            }
            bool bInside = false;
            double ShoreDistance = TNumericLimits<double>::Max();
            for (int32 SegmentIndex = 0;
                 SegmentIndex < Lake.Shoreline.Num();
                 ++SegmentIndex)
            {
                const FVector2D A(Lake.Shoreline[SegmentIndex]);
                const FVector2D B(
                    Lake.Shoreline[
                        (SegmentIndex + 1) % Lake.Shoreline.Num()
                    ]
                );
                const FVector2D Segment = B - A;
                const double LengthSquared = Segment.SizeSquared();
                const double Alpha =
                    LengthSquared > UE_DOUBLE_KINDA_SMALL_NUMBER
                    ? FMath::Clamp(
                        FVector2D::DotProduct(Point - A, Segment) /
                            LengthSquared,
                        0.0,
                        1.0
                    )
                    : 0.0;
                ShoreDistance = FMath::Min(
                    ShoreDistance,
                    FVector2D::Distance(Point, A + Segment * Alpha)
                );
                if ((A.Y > Point.Y) != (B.Y > Point.Y))
                {
                    const double CrossingX = A.X +
                        (Point.Y - A.Y) * (B.X - A.X) /
                        (B.Y - A.Y);
                    if (Point.X < CrossingX)
                    {
                        bInside = !bInside;
                    }
                }
            }
            if (bInside)
            {
                const double DepthAlpha = FMath::SmoothStep(
                    0.0,
                    FMath::Max(1.0, Lake.FalloffWidth),
                    ShoreDistance
                );
                const double NaturalBasin =
                    Lake.SurfaceHeight -
                    Lake.MaximumDepth * DepthAlpha;
                Result = FMath::Min(Result, NaturalBasin);
            }
        }

        for (const FAvenorRiverDefinition& River : Rivers)
        {
            if (!River.InfluenceBounds.IsInside(Point) ||
                River.Points.Num() < 2)
            {
                continue;
            }
            double ClosestDistance = TNumericLimits<double>::Max();
            double SurfaceHeight = 0.0;
            for (int32 SegmentIndex = 0;
                 SegmentIndex + 1 < River.Points.Num();
                 ++SegmentIndex)
            {
                const FVector& A = River.Points[SegmentIndex];
                const FVector& B = River.Points[SegmentIndex + 1];
                const FVector2D Segment(B.X - A.X, B.Y - A.Y);
                const double LengthSquared = Segment.SizeSquared();
                const double Alpha =
                    LengthSquared > UE_DOUBLE_KINDA_SMALL_NUMBER
                    ? FMath::Clamp(
                        FVector2D::DotProduct(
                            Point - FVector2D(A), Segment
                        ) / LengthSquared,
                        0.0,
                        1.0
                    )
                    : 0.0;
                const FVector2D Closest =
                    FVector2D(A) + Segment * Alpha;
                const double Distance =
                    FVector2D::Distance(Point, Closest);
                if (Distance < ClosestDistance)
                {
                    ClosestDistance = Distance;
                    SurfaceHeight = FMath::Lerp(A.Z, B.Z, Alpha);
                }
            }
            if (ClosestDistance >= River.ValleyHalfWidth)
            {
                continue;
            }

            const double BedHeight = SurfaceHeight - River.Depth;
            const double HalfBedWidth =
                FMath::Max(100.0, River.Width * 0.5);
            const double ValleyAlpha = FMath::Clamp(
                (ClosestDistance - HalfBedWidth) /
                    FMath::Max(
                        1.0,
                        River.ValleyHalfWidth - HalfBedWidth
                    ),
                0.0,
                1.0
            );
            const double ShapedAlpha = FMath::Pow(
                ValleyAlpha,
                FMath::Max(0.2, River.CrossSectionExponent)
            );
            const double SmoothAlpha =
                ShapedAlpha * ShapedAlpha *
                (3.0 - 2.0 * ShapedAlpha);

            // Blend from the shared river bed back into the terrain actually
            // present at this point. This can only lower terrain; it cannot
            // create a fixed-datum trench, raised centre or valley platform.
            const double CarvedHeight = FMath::Lerp(
                BedHeight,
                UnderlyingHeight,
                SmoothAlpha
            );
            Result = FMath::Min(Result, CarvedHeight);
        }
        return Result;
    }
};

namespace UE::Avenor::WorldGeneration
{
static constexpr int32 DX[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
static constexpr int32 DY[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
static const FName GeneratedWaterTag(TEXT("AvenorGeneratedWater"));

struct FMountainRange
{
    FVector2D Centre = FVector2D::ZeroVector;
    FVector2D Along = FVector2D(1.0, 0.0);
    FVector2D Across = FVector2D(0.0, 1.0);
    double HalfLength = 1.0;
    double HalfWidth = 1.0;
    double PeakSpacing = 1.0;
    double Relief = 0.0;
    double Phase = 0.0;
};

struct FPriorityEntry
{
    double Height = 0.0;
    int32 Cell = INDEX_NONE;

    bool operator>(const FPriorityEntry& Other) const
    {
        return Height > Other.Height;
    }
};

static double Smooth01(double Value)
{
    const double T = FMath::Clamp(Value, 0.0, 1.0);
    return T * T * (3.0 - 2.0 * T);
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

static void AxisCoordinates(
    const FVector2D& Point,
    const FVector2D& Centre,
    EAvenorLongWorldAxis Axis,
    double& Along,
    double& Across
)
{
    const FVector2D Delta = Point - Centre;
    Along = Axis == EAvenorLongWorldAxis::X ? Delta.X : Delta.Y;
    Across = Axis == EAvenorLongWorldAxis::X ? Delta.Y : Delta.X;
}

static double SegmentDistance(
    const FVector2D& Point,
    const FVector2D& A,
    const FVector2D& B
)
{
    const FVector2D Segment = B - A;
    const double LengthSquared = Segment.SizeSquared();
    const double Alpha = LengthSquared > UE_DOUBLE_KINDA_SMALL_NUMBER
        ? FMath::Clamp(
            FVector2D::DotProduct(Point - A, Segment) /
                LengthSquared,
            0.0,
            1.0
        )
        : 0.0;
    return FVector2D::Distance(Point, A + Segment * Alpha);
}

static TArray<FVector> SmoothPolyline(
    const TArray<FVector>& Input,
    bool bClosed,
    int32 Iterations
)
{
    TArray<FVector> Points = Input;
    for (int32 Iteration = 0;
         Iteration < Iterations &&
             Points.Num() >= (bClosed ? 3 : 2);
         ++Iteration)
    {
        TArray<FVector> Smoothed;
        if (!bClosed)
        {
            Smoothed.Add(Points[0]);
        }
        const int32 SegmentCount =
            bClosed ? Points.Num() : Points.Num() - 1;
        for (int32 Index = 0; Index < SegmentCount; ++Index)
        {
            const FVector& A = Points[Index];
            const FVector& B = Points[(Index + 1) % Points.Num()];
            Smoothed.Add(FMath::Lerp(A, B, 0.25));
            Smoothed.Add(FMath::Lerp(A, B, 0.75));
        }
        if (!bClosed)
        {
            Smoothed.Add(Points.Last());
        }
        Points = MoveTemp(Smoothed);
    }
    return Points;
}

static double PointLineDistance(
    const FVector& Point,
    const FVector& A,
    const FVector& B
)
{
    return SegmentDistance(
        FVector2D(Point), FVector2D(A), FVector2D(B)
    );
}

static void SimplifyRange(
    const TArray<FVector>& Input,
    int32 First,
    int32 Last,
    double Tolerance,
    TArray<bool>& Keep
)
{
    if (Last <= First + 1)
    {
        return;
    }
    double MaximumDistance = 0.0;
    int32 MaximumIndex = INDEX_NONE;
    for (int32 Index = First + 1; Index < Last; ++Index)
    {
        const double Distance = PointLineDistance(
            Input[Index], Input[First], Input[Last]
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
        SimplifyRange(
            Input, First, MaximumIndex, Tolerance, Keep
        );
        SimplifyRange(
            Input, MaximumIndex, Last, Tolerance, Keep
        );
    }
}

static TArray<FVector> SimplifyPolyline(
    const TArray<FVector>& Input,
    double Tolerance
)
{
    if (Input.Num() <= 2)
    {
        return Input;
    }
    TArray<bool> Keep;
    Keep.Init(false, Input.Num());
    Keep[0] = true;
    Keep[Keep.Num() - 1] = true;
    SimplifyRange(
        Input, 0, Input.Num() - 1, Tolerance, Keep
    );
    TArray<FVector> Result;
    for (int32 Index = 0; Index < Input.Num(); ++Index)
    {
        if (Keep[Index])
        {
            Result.Add(Input[Index]);
        }
    }
    return Result;
}

static TArray<FVector> ResampleLinear(
    const TArray<FVector>& Input,
    double Spacing
)
{
    if (Input.Num() < 2)
    {
        return Input;
    }
    TArray<double> Lengths;
    Lengths.SetNum(Input.Num());
    Lengths[0] = 0.0;
    for (int32 Index = 1; Index < Input.Num(); ++Index)
    {
        Lengths[Index] = Lengths[Index - 1] +
            FVector2D::Distance(
                FVector2D(Input[Index - 1]),
                FVector2D(Input[Index])
            );
    }
    const double TotalLength = Lengths.Last();
    if (TotalLength <= UE_DOUBLE_KINDA_SMALL_NUMBER)
    {
        return Input;
    }
    const int32 SampleCount = FMath::Max(
        2,
        FMath::CeilToInt(TotalLength / FMath::Max(1.0, Spacing)) + 1
    );
    TArray<FVector> Result;
    Result.Reserve(SampleCount);
    int32 SegmentIndex = 0;
    for (int32 SampleIndex = 0;
         SampleIndex < SampleCount;
         ++SampleIndex)
    {
        const double Distance = TotalLength *
            static_cast<double>(SampleIndex) /
            static_cast<double>(SampleCount - 1);
        while (SegmentIndex + 1 < Lengths.Num() - 1 &&
               Lengths[SegmentIndex + 1] < Distance)
        {
            ++SegmentIndex;
        }
        const double SegmentLength = FMath::Max(
            1.0,
            Lengths[SegmentIndex + 1] - Lengths[SegmentIndex]
        );
        const double Alpha = FMath::Clamp(
            (Distance - Lengths[SegmentIndex]) / SegmentLength,
            0.0,
            1.0
        );
        Result.Add(FMath::Lerp(
            Input[SegmentIndex], Input[SegmentIndex + 1], Alpha
        ));
    }
    return Result;
}

static FVector CatmullRom(
    const FVector& P0,
    const FVector& P1,
    const FVector& P2,
    const FVector& P3,
    double T
)
{
    const double T2 = T * T;
    const double T3 = T2 * T;
    return 0.5 * (
        2.0 * P1 +
        (P2 - P0) * T +
        (2.0 * P0 - 5.0 * P1 + 4.0 * P2 - P3) * T2 +
        (-P0 + 3.0 * P1 - 3.0 * P2 + P3) * T3
    );
}

static TArray<FVector> ResampleCatmullRom(
    const TArray<FVector>& Controls,
    double Spacing
)
{
    if (Controls.Num() < 3)
    {
        return ResampleLinear(Controls, Spacing);
    }
    TArray<FVector> Dense;
    Dense.Add(Controls[0]);
    for (int32 SegmentIndex = 0;
         SegmentIndex + 1 < Controls.Num();
         ++SegmentIndex)
    {
        const FVector& P0 = Controls[FMath::Max(0, SegmentIndex - 1)];
        const FVector& P1 = Controls[SegmentIndex];
        const FVector& P2 = Controls[SegmentIndex + 1];
        const FVector& P3 = Controls[
            FMath::Min(Controls.Num() - 1, SegmentIndex + 2)
        ];
        const double SegmentLength = FVector2D::Distance(
            FVector2D(P1), FVector2D(P2)
        );
        const int32 Steps = FMath::Clamp(
            FMath::CeilToInt(
                SegmentLength / FMath::Max(1.0, Spacing)
            ),
            2,
            64
        );
        for (int32 Step = 1; Step <= Steps; ++Step)
        {
            Dense.Add(CatmullRom(
                P0, P1, P2, P3,
                static_cast<double>(Step) / Steps
            ));
        }
    }
    return Dense;
}

static void EnforceDownhillGrade(
    TArray<FVector>& Points,
    double StartHeight,
    double EndHeight
)
{
    if (Points.Num() < 2)
    {
        return;
    }
    constexpr double MinimumDrop = 0.01;
    Points[0].Z = StartHeight;
    Points.Last().Z = FMath::Min(
        EndHeight,
        StartHeight - MinimumDrop * (Points.Num() - 1)
    );
    double Previous = Points[0].Z;
    for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
    {
        const int32 Remaining = Points.Num() - 1 - Index;
        Points[Index].Z = FMath::Clamp(
            Points[Index].Z,
            Points.Last().Z + MinimumDrop * Remaining,
            Previous - MinimumDrop
        );
        Previous = Points[Index].Z;
    }
}

static void AddBroadMeanders(
    const FAvenorGeneratedWorld& Grid,
    TArray<FVector>& Points,
    double Strength,
    double MaximumAmplitude,
    int32 Seed
)
{
    if (Points.Num() < 3 || Strength <= 0.0)
    {
        return;
    }
    TArray<double> Lengths;
    Lengths.SetNum(Points.Num());
    Lengths[0] = 0.0;
    for (int32 Index = 1; Index < Points.Num(); ++Index)
    {
        Lengths[Index] = Lengths[Index - 1] +
            FVector2D::Distance(
                FVector2D(Points[Index - 1]),
                FVector2D(Points[Index])
            );
    }
    const double TotalLength = Lengths.Last();
    if (TotalLength < Grid.CellSize * 2.0)
    {
        return;
    }
    FRandomStream Random(Seed);
    const double Amplitude = FMath::Min(
        FMath::Min(
            TotalLength * 0.09,
            Grid.CellSize * 12.0
        ),
        MaximumAmplitude
    ) * Strength;
    const double Cycles = FMath::Clamp(
        TotalLength / (Grid.CellSize * 14.0), 0.75, 4.0
    );
    const double Phase = Random.FRandRange(-PI, PI);
    for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
    {
        const double Alpha = Lengths[Index] / TotalLength;
        const FVector2D Tangent = FVector2D(
            Points[Index + 1].X - Points[Index - 1].X,
            Points[Index + 1].Y - Points[Index - 1].Y
        ).GetSafeNormal();
        const FVector2D Normal(-Tangent.Y, Tangent.X);
        const double EndpointFade = FMath::Sin(PI * Alpha);
        const double Wave =
            FMath::Sin(2.0 * PI * Cycles * Alpha + Phase) * 0.72 +
            FMath::Sin(
                2.0 * PI * Cycles * 0.43 * Alpha - Phase * 0.37
            ) * 0.28;
        const FVector2D Offset =
            Normal * Amplitude * EndpointFade * Wave;
        Points[Index].X += Offset.X;
        Points[Index].Y += Offset.Y;
    }
}

static TArray<FMountainRange> CreateMountainRanges(
    const FBox& Bounds,
    EAvenorLongWorldAxis Axis,
    int32 Seed,
    double RangesPer100Km,
    double RangeLength,
    double RangeWidth,
    double PeakSpacing,
    double Relief,
    double ExclusionHalfWidth,
    double EdgeBias
)
{
    TArray<FMountainRange> Ranges;
    const FVector2D Centre(Bounds.GetCenter());
    const FVector2D Size(Bounds.GetSize());
    const double LongSize = Axis == EAvenorLongWorldAxis::X
        ? Size.X : Size.Y;
    const double CrossSize = Axis == EAvenorLongWorldAxis::X
        ? Size.Y : Size.X;
    const int32 RangeCount = FMath::Clamp(
        FMath::RoundToInt(
            RangesPer100Km * LongSize / 10000000.0
        ),
        0,
        128
    );
    FRandomStream Random(Seed * 7919 + 17);
    for (int32 RangeIndex = 0;
         RangeIndex < RangeCount;
         ++RangeIndex)
    {
        const double Along = Random.FRandRange(
            -LongSize * 0.47, LongSize * 0.47
        );
        const double AvailableSide = FMath::Max(
            0.0,
            CrossSize * 0.5 - ExclusionHalfWidth -
                RangeWidth * 0.25
        );
        if (AvailableSide <= 0.0)
        {
            continue;
        }
        const double SideSign = Random.FRand() < 0.5 ? -1.0 : 1.0;
        const double EdgeT = FMath::Pow(
            Random.FRand(),
            FMath::Lerp(1.0, 0.25, EdgeBias)
        );
        const double Across = SideSign * (
            ExclusionHalfWidth +
            RangeWidth * 0.25 +
            AvailableSide * EdgeT
        );
        const double Angle = Random.FRandRange(-0.55, 0.55);
        const FVector2D LongVector =
            Axis == EAvenorLongWorldAxis::X
            ? FVector2D(1.0, 0.0)
            : FVector2D(0.0, 1.0);
        const FVector2D CrossVector(-LongVector.Y, LongVector.X);
        FMountainRange Range;
        Range.Centre = Centre + LongVector * Along + CrossVector * Across;
        Range.Along =
            LongVector * FMath::Cos(Angle) +
            CrossVector * FMath::Sin(Angle);
        Range.Across = FVector2D(-Range.Along.Y, Range.Along.X);
        Range.HalfLength = RangeLength *
            Random.FRandRange(0.42, 0.62);
        Range.HalfWidth = RangeWidth *
            Random.FRandRange(0.42, 0.58);
        Range.PeakSpacing = PeakSpacing *
            Random.FRandRange(0.82, 1.18);
        Range.Relief = Relief * Random.FRandRange(0.78, 1.22);
        Range.Phase = Random.FRandRange(-PI, PI);
        Ranges.Add(Range);
    }
    return Ranges;
}

static double EvaluateLandform(
    const FVector2D& Point,
    const FBox& Bounds,
    EAvenorLongWorldAxis Axis,
    int32 Seed,
    const TArray<FMountainRange>& Mountains,
    bool bPlains,
    bool bHills,
    bool bMountains,
    bool bMesas,
    bool bOceans,
    double PlainsCoverage,
    double PlainsRelief,
    double HillsCoverage,
    double HillScale,
    double HillRelief,
    double MesaCoverage,
    double MesaScale,
    double MesaRelief,
    int32 MesaTerraces,
    double MountainExclusion,
    double EdgeBlendWidth,
    double SeaLevel,
    double OceanDepth,
    double CoastWidth,
    bool bOceanWidthEdges,
    bool bOceanLengthEnds
)
{
    const FVector2D Centre(Bounds.GetCenter());
    const FVector2D Offset(
        static_cast<double>(Seed) * 17.13,
        static_cast<double>(Seed) * -11.77
    );
    const double RegionScale = FMath::Max(
        300000.0,
        HillScale * 5.0
    );
    const FVector2D Warp(
        Noise(Point, RegionScale * 0.7, Offset * 0.31),
        Noise(Point, RegionScale * 0.7, Offset * 0.53)
    );
    const FVector2D WarpedPoint = Point + Warp * HillScale * 0.55;
    const double Selector = 0.5 + 0.5 * Noise(
        WarpedPoint, RegionScale, Offset * 3.7
    );
    const double PlainEnd = bPlains
        ? FMath::Clamp(PlainsCoverage, 0.0, 1.0)
        : 0.0;
    const double HillEnd = bHills
        ? FMath::Clamp(
            PlainEnd + HillsCoverage,
            PlainEnd,
            1.0
        )
        : PlainEnd;
    const double MesaStart = bMesas
        ? FMath::Clamp(1.0 - MesaCoverage, HillEnd, 1.0)
        : 1.0;
    constexpr double Blend = 0.06;
    const double PlainMask = bPlains
        ? 1.0 - Smooth01(
            (Selector - PlainEnd + Blend) / (2.0 * Blend)
        )
        : 0.0;
    const double HillMask = bHills
        ? Smooth01(
            (Selector - PlainEnd + Blend) / (2.0 * Blend)
        ) * (1.0 - Smooth01(
            (Selector - HillEnd + Blend) / (2.0 * Blend)
        ))
        : 0.0;
    const double MesaMask = bMesas
        ? Smooth01(
            (Selector - MesaStart + Blend) / (2.0 * Blend)
        )
        : 0.0;
    const double UnassignedMask = FMath::Max(
        0.0,
        1.0 - PlainMask - HillMask - MesaMask
    );

    const double Broad = Noise(
        WarpedPoint, HillScale * 1.35, Offset * 1.31
    );
    const double Medium = Noise(
        WarpedPoint, HillScale * 0.52, Offset * 2.17
    );
    const double Ridge = 1.0 - FMath::Abs(Noise(
        WarpedPoint, HillScale * 0.78, Offset * 2.93
    ));
    const double Regional = Noise(
        WarpedPoint, RegionScale, Offset * 0.79
    ) * PlainsRelief * 1.5;
    const double PlainHeight = Broad * PlainsRelief;
    const double HillHeight = (
        Broad * 0.58 + Medium * 0.28 +
        (Ridge * 2.0 - 1.0) * 0.14
    ) * HillRelief;
    const int32 Terraces = FMath::Max(1, MesaTerraces);
    const double MesaNoise = 0.5 + 0.5 * Noise(
        WarpedPoint, MesaScale, Offset * 4.41
    );
    const double MesaHeight =
        FMath::FloorToDouble(MesaNoise * Terraces) /
        static_cast<double>(Terraces) * MesaRelief;

    double Height = Regional +
        PlainHeight * PlainMask +
        HillHeight * (HillMask + UnassignedMask) +
        MesaHeight * MesaMask;

    if (bMountains)
    {
        double AlongAxis = 0.0;
        double AcrossAxis = 0.0;
        AxisCoordinates(Point, Centre, Axis, AlongAxis, AcrossAxis);
        const double AxisExclusionMask = Smooth01(
            (FMath::Abs(AcrossAxis) - MountainExclusion) /
            FMath::Max(50000.0, MountainExclusion * 0.45)
        );
        for (const FMountainRange& Mountain : Mountains)
        {
            const FVector2D Delta = Point - Mountain.Centre;
            const double Along = FVector2D::DotProduct(
                Delta, Mountain.Along
            );
            const double Across = FVector2D::DotProduct(
                Delta, Mountain.Across
            );
            const double Ellipse = FMath::Sqrt(
                FMath::Square(
                    Along / FMath::Max(1.0, Mountain.HalfLength)
                ) +
                FMath::Square(
                    Across / FMath::Max(1.0, Mountain.HalfWidth)
                )
            );
            if (Ellipse >= 1.65)
            {
                continue;
            }
            const double Core = 1.0 - Smooth01(Ellipse);
            const double Foothills = Ellipse > 1.0
                ? Smooth01((1.65 - Ellipse) / 0.65) * 0.24
                : 0.0;
            const double PeakWave = 0.58 + 0.42 * FMath::Square(
                0.5 + 0.5 * FMath::Cos(
                    Along / FMath::Max(1.0, Mountain.PeakSpacing) *
                        2.0 * PI + Mountain.Phase
                )
            );
            const double RidgeNoise = 0.72 + 0.28 * (
                1.0 - FMath::Abs(Noise(
                    Point,
                    Mountain.HalfWidth * 0.32,
                    Offset * 7.13
                ))
            );
            Height += Mountain.Relief * AxisExclusionMask *
                (Core + Foothills) * PeakWave * RidgeNoise;
        }
    }

    const double DistanceX = FMath::Min(
        Point.X - Bounds.Min.X,
        Bounds.Max.X - Point.X
    );
    const double DistanceY = FMath::Min(
        Point.Y - Bounds.Min.Y,
        Bounds.Max.Y - Point.Y
    );
    const double EdgeDistance = FMath::Min(DistanceX, DistanceY);
    const double EdgeMask = Smooth01(
        EdgeDistance / FMath::Max(1.0, EdgeBlendWidth)
    );
    Height = FMath::Lerp(Regional + PlainHeight * 0.35, Height, EdgeMask);

    if (bOceans)
    {
        const double WidthEdgeDistance =
            Axis == EAvenorLongWorldAxis::X ? DistanceY : DistanceX;
        const double LengthEndDistance =
            Axis == EAvenorLongWorldAxis::X ? DistanceX : DistanceY;
        double OceanDistance = TNumericLimits<double>::Max();
        if (bOceanWidthEdges)
        {
            OceanDistance = FMath::Min(
                OceanDistance, WidthEdgeDistance
            );
        }
        if (bOceanLengthEnds)
        {
            OceanDistance = FMath::Min(
                OceanDistance, LengthEndDistance
            );
        }
        if (OceanDistance < TNumericLimits<double>::Max())
        {
            const double CoastAlpha = Smooth01(
                OceanDistance / FMath::Max(1.0, CoastWidth)
            );
            const double OceanFloor = SeaLevel - OceanDepth *
                (1.0 - CoastAlpha);
            Height = FMath::Lerp(OceanFloor, Height, CoastAlpha);
        }
    }
    return Height;
}

static void ApplyThermalErosion(
    FAvenorGeneratedWorld& Grid,
    int32 Iterations,
    double Strength,
    double TalusAngleDegrees
)
{
    if (Iterations <= 0 || Strength <= 0.0)
    {
        return;
    }
    const double TalusHeight = FMath::Tan(
        FMath::DegreesToRadians(TalusAngleDegrees)
    ) * Grid.CellSize;
    const int32 PairDX[4] = {1, 0, 1, -1};
    const int32 PairDY[4] = {0, 1, 1, 1};
    for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
    {
        TArray<double> Delta;
        Delta.SetNumZeroed(Grid.TerrainHeight.Num());
        for (int32 Y = 1; Y < Grid.Rows - 1; ++Y)
        {
            for (int32 X = 1; X < Grid.Columns - 1; ++X)
            {
                const int32 Cell = Grid.Index(X, Y);
                for (int32 Pair = 0; Pair < 4; ++Pair)
                {
                    const int32 Neighbour = Grid.Index(
                        X + PairDX[Pair], Y + PairDY[Pair]
                    );
                    const double Distance =
                        PairDX[Pair] != 0 && PairDY[Pair] != 0
                        ? Grid.CellSize * FMath::Sqrt(2.0)
                        : Grid.CellSize;
                    const double AllowedDifference =
                        TalusHeight * Distance / Grid.CellSize;
                    const double Difference =
                        Grid.TerrainHeight[Cell] -
                        Grid.TerrainHeight[Neighbour];
                    if (FMath::Abs(Difference) <= AllowedDifference)
                    {
                        continue;
                    }
                    const double Transfer = (
                        FMath::Abs(Difference) - AllowedDifference
                    ) * Strength * 0.25;
                    const int32 High = Difference > 0.0
                        ? Cell : Neighbour;
                    const int32 Low = Difference > 0.0
                        ? Neighbour : Cell;
                    Delta[High] -= Transfer;
                    Delta[Low] += Transfer;
                }
            }
        }
        for (int32 Cell = 0; Cell < Grid.TerrainHeight.Num(); ++Cell)
        {
            Grid.TerrainHeight[Cell] += Delta[Cell];
        }
    }
}

static void FillDepressions(
    FAvenorGeneratedWorld& Grid,
    double Epsilon
)
{
    const int32 Count = Grid.TerrainHeight.Num();
    Grid.FilledHeight = Grid.TerrainHeight;
    Grid.FillParent.Init(INDEX_NONE, Count);
    TArray<bool> Visited;
    Visited.Init(false, Count);
    std::priority_queue<
        FPriorityEntry,
        std::vector<FPriorityEntry>,
        std::greater<FPriorityEntry>
    > Queue;
    for (int32 Y = 0; Y < Grid.Rows; ++Y)
    {
        for (int32 X = 0; X < Grid.Columns; ++X)
        {
            if (!Grid.IsBoundary(X, Y))
            {
                continue;
            }
            const int32 Cell = Grid.Index(X, Y);
            Visited[Cell] = true;
            Queue.push({Grid.FilledHeight[Cell], Cell});
        }
    }
    while (!Queue.empty())
    {
        const FPriorityEntry Entry = Queue.top();
        Queue.pop();
        const int32 X = Entry.Cell % Grid.Columns;
        const int32 Y = Entry.Cell / Grid.Columns;
        for (int32 Direction = 0; Direction < 8; ++Direction)
        {
            const int32 NX = X + DX[Direction];
            const int32 NY = Y + DY[Direction];
            if (!Grid.IsValid(NX, NY))
            {
                continue;
            }
            const int32 Neighbour = Grid.Index(NX, NY);
            if (Visited[Neighbour])
            {
                continue;
            }
            Visited[Neighbour] = true;
            Grid.FillParent[Neighbour] = Entry.Cell;
            Grid.FilledHeight[Neighbour] = FMath::Max(
                Grid.TerrainHeight[Neighbour],
                Entry.Height + Epsilon
            );
            Queue.push({
                Grid.FilledHeight[Neighbour], Neighbour
            });
        }
    }
}

static void BuildFlow(FAvenorGeneratedWorld& Grid)
{
    const int32 Count = Grid.TerrainHeight.Num();
    Grid.Downstream.Init(INDEX_NONE, Count);
    Grid.Accumulation.Init(1.0, Count);
    for (int32 Y = 0; Y < Grid.Rows; ++Y)
    {
        for (int32 X = 0; X < Grid.Columns; ++X)
        {
            const int32 Cell = Grid.Index(X, Y);
            if (Grid.IsBoundary(X, Y))
            {
                continue;
            }
            int32 Best = INDEX_NONE;
            double BestSlope = 0.0;
            for (int32 Direction = 0; Direction < 8; ++Direction)
            {
                const int32 NX = X + DX[Direction];
                const int32 NY = Y + DY[Direction];
                if (!Grid.IsValid(NX, NY))
                {
                    continue;
                }
                const int32 Neighbour = Grid.Index(NX, NY);
                const double Distance =
                    DX[Direction] != 0 && DY[Direction] != 0
                    ? Grid.CellSize * FMath::Sqrt(2.0)
                    : Grid.CellSize;
                const double Slope = (
                    Grid.FilledHeight[Cell] -
                    Grid.FilledHeight[Neighbour]
                ) / Distance;
                if (Slope > BestSlope)
                {
                    BestSlope = Slope;
                    Best = Neighbour;
                }
            }
            Grid.Downstream[Cell] = Best != INDEX_NONE
                ? Best
                : Grid.FillParent[Cell];
        }
    }
    TArray<int32> Order;
    Order.Reserve(Count);
    for (int32 Cell = 0; Cell < Count; ++Cell)
    {
        Order.Add(Cell);
    }
    Order.Sort([&Grid](int32 A, int32 B)
    {
        return Grid.FilledHeight[A] > Grid.FilledHeight[B];
    });
    for (int32 Cell : Order)
    {
        const int32 Downstream = Grid.Downstream[Cell];
        if (Downstream != INDEX_NONE)
        {
            Grid.Accumulation[Downstream] +=
                Grid.Accumulation[Cell];
        }
    }
}

static void ApplyHydraulicErosion(
    FAvenorGeneratedWorld& Grid,
    int32 Iterations,
    double IncisionPerIteration,
    double StreamStartArea,
    double Epsilon
)
{
    if (Iterations <= 0 || IncisionPerIteration <= 0.0)
    {
        return;
    }
    const double CellArea = Grid.CellAreaSquareKilometres();
    for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
    {
        FillDepressions(Grid, Epsilon);
        BuildFlow(Grid);
        TArray<double> Incision;
        Incision.SetNumZeroed(Grid.TerrainHeight.Num());
        for (int32 Cell = 0;
             Cell < Grid.TerrainHeight.Num();
             ++Cell)
        {
            const int32 Downstream = Grid.Downstream[Cell];
            if (Downstream == INDEX_NONE)
            {
                continue;
            }
            const double Area = Grid.Accumulation[Cell] * CellArea;
            const double Drop = FMath::Max(
                0.0,
                Grid.TerrainHeight[Cell] -
                    Grid.TerrainHeight[Downstream]
            );
            const double Distance = FMath::Max(
                1.0,
                FVector2D::Distance(
                    Grid.Position(Cell), Grid.Position(Downstream)
                )
            );
            const double Slope = Drop / Distance;
            const double SteepHeadwaterFactor = FMath::Lerp(
                1.0,
                0.18,
                Smooth01(Slope / 0.22)
            );
            const double EffectiveStart = FMath::Max(
                CellArea,
                StreamStartArea * SteepHeadwaterFactor
            );
            if (Area < EffectiveStart)
            {
                continue;
            }
            const double AreaStrength = FMath::Clamp(
                FMath::Loge(Area / EffectiveStart + 1.0) / 4.5,
                0.0,
                1.0
            );
            const double SlopeStrength = FMath::Clamp(
                FMath::Sqrt(FMath::Max(0.0, Slope) / 0.08),
                0.25,
                1.5
            );
            const double Cut = IncisionPerIteration *
                AreaStrength * SlopeStrength;
            const int32 X = Cell % Grid.Columns;
            const int32 Y = Cell / Grid.Columns;
            const int32 Radius = FMath::Clamp(
                1 + FMath::FloorToInt(FMath::Sqrt(Area) * 0.35),
                1,
                4
            );
            for (int32 OY = -Radius; OY <= Radius; ++OY)
            {
                for (int32 OX = -Radius; OX <= Radius; ++OX)
                {
                    if (!Grid.IsValid(X + OX, Y + OY))
                    {
                        continue;
                    }
                    const double DistanceCells = FMath::Sqrt(
                        static_cast<double>(OX * OX + OY * OY)
                    );
                    if (DistanceCells > Radius + 0.25)
                    {
                        continue;
                    }
                    const double Weight = FMath::Exp(
                        -2.0 * FMath::Square(
                            DistanceCells /
                            FMath::Max(1.0, static_cast<double>(Radius))
                        )
                    );
                    const int32 Target = Grid.Index(X + OX, Y + OY);
                    Incision[Target] = FMath::Max(
                        Incision[Target], Cut * Weight
                    );
                }
            }
        }
        for (int32 Cell = 0;
             Cell < Grid.TerrainHeight.Num();
             ++Cell)
        {
            Grid.TerrainHeight[Cell] -= Incision[Cell];
        }
    }
}

static TArray<FVector> BuildLakeOutline(
    const FAvenorGeneratedWorld& Grid,
    const TArray<int32>& Cells,
    double SurfaceHeight
)
{
    if (Cells.Num() < 2)
    {
        return {};
    }
    TSet<int32> CellSet;
    FVector2D Centre = FVector2D::ZeroVector;
    for (int32 Cell : Cells)
    {
        CellSet.Add(Cell);
        Centre += Grid.Position(Cell);
    }
    Centre /= static_cast<double>(Cells.Num());
    TArray<FVector> Boundary;
    for (int32 Cell : Cells)
    {
        const int32 X = Cell % Grid.Columns;
        const int32 Y = Cell / Grid.Columns;
        bool bBoundary = false;
        for (int32 Direction = 0; Direction < 8; ++Direction)
        {
            if (!Grid.IsValid(X + DX[Direction], Y + DY[Direction]) ||
                !CellSet.Contains(Grid.Index(
                    X + DX[Direction], Y + DY[Direction]
                )))
            {
                bBoundary = true;
                break;
            }
        }
        if (bBoundary)
        {
            const FVector2D FromCentre =
                Grid.Position(Cell) - Centre;
            const FVector2D Expanded = Grid.Position(Cell) +
                FromCentre.GetSafeNormal() * Grid.CellSize * 0.58;
            Boundary.Emplace(
                Expanded.X, Expanded.Y, SurfaceHeight
            );
        }
    }
    Boundary.Sort([Centre](const FVector& A, const FVector& B)
    {
        const double AngleA = FMath::Atan2(
            A.Y - Centre.Y, A.X - Centre.X
        );
        const double AngleB = FMath::Atan2(
            B.Y - Centre.Y, B.X - Centre.X
        );
        return AngleA < AngleB;
    });
    if (Boundary.Num() > 64)
    {
        TArray<FVector> Reduced;
        const int32 TargetCount = 64;
        for (int32 Index = 0; Index < TargetCount; ++Index)
        {
            Reduced.Add(Boundary[
                Index * Boundary.Num() / TargetCount
            ]);
        }
        Boundary = MoveTemp(Reduced);
    }
    return SmoothPolyline(Boundary, true, 2);
}

static void ExtractLakes(
    FAvenorGeneratedWorld& Grid,
    double MinimumCatchmentArea,
    double MinimumDepth,
    double MaximumArea,
    int32 MaximumCount,
    double FalloffWidth
)
{
    Grid.Lakes.Reset();
    Grid.LakeIndexByCell.Init(
        INDEX_NONE, Grid.TerrainHeight.Num()
    );
    if (MaximumCount <= 0)
    {
        return;
    }
    const double CellArea = Grid.CellAreaSquareKilometres();
    TArray<bool> Candidate;
    Candidate.Init(false, Grid.TerrainHeight.Num());
    for (int32 Cell = 0; Cell < Grid.TerrainHeight.Num(); ++Cell)
    {
        Candidate[Cell] =
            Grid.FilledHeight[Cell] - Grid.TerrainHeight[Cell] >=
                MinimumDepth;
    }
    struct FLakeCandidate
    {
        TArray<int32> Cells;
        double CatchmentArea = 0.0;
        double SurfaceHeight = -TNumericLimits<double>::Max();
        double Depth = 0.0;
    };
    TArray<FLakeCandidate> Candidates;
    TArray<bool> Visited;
    Visited.Init(false, Candidate.Num());
    for (int32 Start = 0; Start < Candidate.Num(); ++Start)
    {
        if (!Candidate[Start] || Visited[Start])
        {
            continue;
        }
        FLakeCandidate Basin;
        TArray<int32> Open;
        Open.Add(Start);
        Visited[Start] = true;
        double MinimumTerrain = TNumericLimits<double>::Max();
        while (!Open.IsEmpty())
        {
            const int32 Cell = Open.Pop(EAllowShrinking::No);
            Basin.Cells.Add(Cell);
            Basin.SurfaceHeight = FMath::Max(
                Basin.SurfaceHeight, Grid.FilledHeight[Cell]
            );
            Basin.CatchmentArea = FMath::Max(
                Basin.CatchmentArea,
                Grid.Accumulation[Cell] * CellArea
            );
            MinimumTerrain = FMath::Min(
                MinimumTerrain, Grid.TerrainHeight[Cell]
            );
            const int32 X = Cell % Grid.Columns;
            const int32 Y = Cell / Grid.Columns;
            for (int32 Direction = 0; Direction < 8; ++Direction)
            {
                const int32 NX = X + DX[Direction];
                const int32 NY = Y + DY[Direction];
                if (!Grid.IsValid(NX, NY))
                {
                    continue;
                }
                const int32 Neighbour = Grid.Index(NX, NY);
                if (Candidate[Neighbour] && !Visited[Neighbour])
                {
                    Visited[Neighbour] = true;
                    Open.Add(Neighbour);
                }
            }
        }
        const double Area = Basin.Cells.Num() * CellArea;
        Basin.Depth = Basin.SurfaceHeight - MinimumTerrain;
        if (Basin.CatchmentArea >= MinimumCatchmentArea &&
            Basin.Depth >= MinimumDepth &&
            Area <= MaximumArea &&
            Basin.Cells.Num() >= 2)
        {
            Candidates.Add(MoveTemp(Basin));
        }
    }
    Candidates.Sort([](
        const FLakeCandidate& A,
        const FLakeCandidate& B
    )
    {
        return A.CatchmentArea > B.CatchmentArea;
    });
    const int32 LakeCount = FMath::Min(
        MaximumCount, Candidates.Num()
    );
    for (int32 CandidateIndex = 0;
         CandidateIndex < LakeCount;
         ++CandidateIndex)
    {
        const FLakeCandidate& CandidateLake =
            Candidates[CandidateIndex];
        FAvenorLakeDefinition Lake;
        Lake.SurfaceHeight = CandidateLake.SurfaceHeight;
        Lake.MaximumDepth = CandidateLake.Depth;
        Lake.FalloffWidth = FalloffWidth;
        Lake.CatchmentSquareKilometres =
            CandidateLake.CatchmentArea;
        Lake.Shoreline = BuildLakeOutline(
            Grid,
            CandidateLake.Cells,
            Lake.SurfaceHeight
        );
        if (Lake.Shoreline.Num() < 3)
        {
            continue;
        }
        FBox2D Bounds(ForceInit);
        for (const FVector& Point : Lake.Shoreline)
        {
            Bounds += FVector2D(Point);
        }
        Lake.InfluenceBounds = Bounds.ExpandBy(FalloffWidth);
        const int32 LakeIndex = Grid.Lakes.Num();
        Grid.Lakes.Add(MoveTemp(Lake));
        for (int32 Cell : CandidateLake.Cells)
        {
            Grid.LakeIndexByCell[Cell] = LakeIndex;
        }
    }
}

static double LocalSlope(
    const FAvenorGeneratedWorld& Grid,
    int32 Cell
)
{
    const int32 Downstream = Grid.Downstream[Cell];
    if (Downstream == INDEX_NONE)
    {
        return 0.0;
    }
    const double Distance = FMath::Max(
        1.0,
        FVector2D::Distance(
            Grid.Position(Cell), Grid.Position(Downstream)
        )
    );
    return FMath::Max(
        0.0,
        Grid.TerrainHeight[Cell] -
            Grid.TerrainHeight[Downstream]
    ) / Distance;
}

static void ExtractRivers(
    FAvenorGeneratedWorld& Grid,
    int32 Seed,
    double StreamStartArea,
    double MainRiverArea,
    double HeadwaterWidth,
    double MainRiverWidth,
    double MaximumRiverDepth,
    double HeadwaterValleyWidth,
    double MainValleyWidth,
    double MaximumValleyDepth,
    double MeanderStrength,
    int32 MaximumReaches,
    bool bValleys,
    bool bCanyons,
    double CanyonStartArea
)
{
    Grid.Rivers.Reset();
    const int32 Count = Grid.TerrainHeight.Num();
    const double CellArea = Grid.CellAreaSquareKilometres();
    double AdaptiveStartArea = FMath::Max(
        CellArea, StreamStartArea
    );
    TArray<bool> Channel;
    TArray<int32> UpstreamCount;
    for (int32 Attempt = 0; Attempt < 18; ++Attempt)
    {
        Channel.Init(false, Count);
        UpstreamCount.Init(0, Count);
        for (int32 Cell = 0; Cell < Count; ++Cell)
        {
            const double Slope = LocalSlope(Grid, Cell);
            const double SteepStrength = Smooth01(
                (Slope - 0.035) / 0.20
            );
            const double LocalStart = AdaptiveStartArea *
                FMath::Lerp(1.0, 0.22, SteepStrength);
            Channel[Cell] =
                Grid.Accumulation[Cell] * CellArea >= LocalStart &&
                Grid.Downstream[Cell] != INDEX_NONE;
        }
        // Once a headwater qualifies, preserve its continuous downstream
        // route even when the local slope subsequently becomes gentle.
        for (int32 Cell = 0; Cell < Count; ++Cell)
        {
            if (!Channel[Cell])
            {
                continue;
            }
            int32 Current = Grid.Downstream[Cell];
            for (int32 Guard = 0;
                 Guard < Count && Current != INDEX_NONE;
                 ++Guard)
            {
                if (Channel[Current])
                {
                    break;
                }
                Channel[Current] = true;
                Current = Grid.Downstream[Current];
            }
        }
        for (int32 Cell = 0; Cell < Count; ++Cell)
        {
            const int32 Downstream = Grid.Downstream[Cell];
            if (Channel[Cell] &&
                Downstream != INDEX_NONE &&
                Channel[Downstream])
            {
                ++UpstreamCount[Downstream];
            }
        }
        int32 ReachCount = 0;
        for (int32 Cell = 0; Cell < Count; ++Cell)
        {
            ReachCount += Channel[Cell] &&
                UpstreamCount[Cell] != 1;
        }
        if (ReachCount <= MaximumReaches)
        {
            break;
        }
        AdaptiveStartArea *= 1.28;
    }

    TArray<double> WaterSurface;
    WaterSurface.Init(TNumericLimits<double>::Max(), Count);
    TArray<int32> FlowOrder;
    for (int32 Cell = 0; Cell < Count; ++Cell)
    {
        if (!Channel[Cell])
        {
            continue;
        }
        const double Area = Grid.Accumulation[Cell] * CellArea;
        const double SizeAlpha = FMath::Clamp(
            FMath::Loge(
                Area / FMath::Max(CellArea, StreamStartArea) + 1.0
            ) /
            FMath::Loge(
                MainRiverArea /
                    FMath::Max(CellArea, StreamStartArea) + 2.0
            ),
            0.0,
            1.0
        );
        const double ValleyInset = bValleys
            ? MaximumValleyDepth * (0.08 + 0.42 * SizeAlpha)
            : 100.0;
        const int32 LakeIndex = Grid.LakeIndexByCell.IsValidIndex(Cell)
            ? Grid.LakeIndexByCell[Cell]
            : INDEX_NONE;
        WaterSurface[Cell] =
            LakeIndex != INDEX_NONE && Grid.Lakes.IsValidIndex(LakeIndex)
            ? Grid.Lakes[LakeIndex].SurfaceHeight
            : Grid.TerrainHeight[Cell] - ValleyInset;
        FlowOrder.Add(Cell);
    }
    FlowOrder.Sort([&Grid](int32 A, int32 B)
    {
        return Grid.FilledHeight[A] > Grid.FilledHeight[B];
    });
    for (int32 Cell : FlowOrder)
    {
        const int32 Downstream = Grid.Downstream[Cell];
        if (Downstream != INDEX_NONE && Channel[Downstream])
        {
            WaterSurface[Downstream] = FMath::Min(
                WaterSurface[Downstream],
                WaterSurface[Cell] - 1.0
            );
        }
    }

    const FVector2D WarpOffset(
        static_cast<double>(Seed) * 23.71,
        static_cast<double>(Seed) * -13.43
    );
    for (int32 Start = 0; Start < Count; ++Start)
    {
        if (!Channel[Start] || UpstreamCount[Start] == 1)
        {
            continue;
        }
        TArray<int32> Cells;
        TSet<int32> Seen;
        int32 Current = Start;
        for (int32 Guard = 0; Guard < Count; ++Guard)
        {
            if (Current == INDEX_NONE ||
                Seen.Contains(Current) ||
                !Channel[Current])
            {
                break;
            }
            Seen.Add(Current);
            Cells.Add(Current);
            const int32 Next = Grid.Downstream[Current];
            if (Next == INDEX_NONE || !Channel[Next])
            {
                break;
            }
            Current = Next;
            if (UpstreamCount[Current] != 1)
            {
                Cells.Add(Current);
                break;
            }
        }
        if (Cells.Num() < 2)
        {
            continue;
        }

        FAvenorRiverDefinition River;
        River.DischargeSquareKilometres =
            Grid.Accumulation[Cells.Last()] * CellArea;
        const double SizeAlpha = FMath::Clamp(
            FMath::Loge(
                River.DischargeSquareKilometres /
                    FMath::Max(CellArea, StreamStartArea) + 1.0
            ) /
            FMath::Loge(
                MainRiverArea /
                    FMath::Max(CellArea, StreamStartArea) + 2.0
            ),
            0.0,
            1.0
        );
        River.Width = FMath::Lerp(
            HeadwaterWidth, MainRiverWidth, SizeAlpha
        );
        River.Depth = FMath::Lerp(
            FMath::Min(220.0, MaximumRiverDepth),
            MaximumRiverDepth,
            SizeAlpha
        );
        River.ValleyHalfWidth = bValleys
            ? FMath::Lerp(
                HeadwaterValleyWidth, MainValleyWidth, SizeAlpha
            )
            : FMath::Max(River.Width * 2.0, 2000.0);
        const double AverageSlope = (
            LocalSlope(Grid, Cells[0]) +
            LocalSlope(Grid, Cells.Last())
        ) * 0.5;
        const bool bCanyonReach = bCanyons &&
            River.DischargeSquareKilometres >= CanyonStartArea &&
            AverageSlope >= 0.045;
        if (bCanyonReach)
        {
            River.ValleyHalfWidth *= 0.58;
            River.CrossSectionExponent = 0.62;
        }
        else
        {
            River.CrossSectionExponent = FMath::Lerp(
                1.35, 0.82, Smooth01(AverageSlope / 0.16)
            );
        }

        for (int32 Cell : Cells)
        {
            FVector2D Position = Grid.Position(Cell);
            const FVector2D NoiseInput =
                (Position + WarpOffset) / (Grid.CellSize * 5.0);
            Position += FVector2D(
                FMath::PerlinNoise2D(NoiseInput),
                FMath::PerlinNoise2D(FVector2D(
                    NoiseInput.Y + 19.7,
                    -NoiseInput.X - 7.3
                ))
            ) * Grid.CellSize * 0.38;
            River.Points.Emplace(
                Position.X, Position.Y, WaterSurface[Cell]
            );
        }
        const double StartHeight = River.Points[0].Z;
        const double EndHeight = River.Points.Last().Z;
        River.Points = SimplifyPolyline(
            River.Points, Grid.CellSize * 0.45
        );
        River.Points = ResampleLinear(
            River.Points, Grid.CellSize * 4.5
        );
        const double LowlandFactor = 1.0 - Smooth01(
            AverageSlope / 0.12
        );
        AddBroadMeanders(
            Grid,
            River.Points,
            MeanderStrength * FMath::Lerp(0.28, 1.0, LowlandFactor),
            River.ValleyHalfWidth * 0.65,
            Seed + Start * 101
        );
        EnforceDownhillGrade(
            River.Points, StartHeight, EndHeight
        );
        River.SplinePoints = River.Points;
        River.Points = ResampleCatmullRom(
            River.SplinePoints, Grid.CellSize * 0.55
        );
        EnforceDownhillGrade(
            River.Points, StartHeight, EndHeight
        );
        FBox2D Bounds(ForceInit);
        for (const FVector& Point : River.Points)
        {
            Bounds += FVector2D(Point);
        }
        River.InfluenceBounds = Bounds.ExpandBy(
            River.ValleyHalfWidth
        );
        Grid.Rivers.Add(MoveTemp(River));
    }
    Grid.Rivers.Sort([](
        const FAvenorRiverDefinition& A,
        const FAvenorRiverDefinition& B
    )
    {
        return A.DischargeSquareKilometres >
            B.DischargeSquareKilometres;
    });
}

static TSharedPtr<const FAvenorGeneratedWorld> GenerateWorld(
    const FBox& Bounds,
    EAvenorLongWorldAxis Axis,
    int32 Seed,
    double RequestedCellSize,
    int32 MaximumCells,
    bool bPlains,
    bool bHills,
    bool bMountains,
    bool bMesas,
    bool bValleys,
    bool bCanyons,
    bool bRivers,
    bool bLakes,
    bool bOceans,
    double PlainsCoverage,
    double PlainsRelief,
    double HillsCoverage,
    double HillScale,
    double HillRelief,
    double MountainDensity,
    double MountainLength,
    double MountainWidth,
    double MountainSpacing,
    double MountainRelief,
    double MountainExclusion,
    double MountainEdgeBias,
    double MesaCoverage,
    double MesaScale,
    double MesaRelief,
    int32 MesaTerraces,
    int32 ThermalIterations,
    double ThermalStrength,
    double TalusAngle,
    int32 HydraulicIterations,
    double HydraulicIncision,
    double DrainageEpsilon,
    double StreamStartArea,
    double MainRiverArea,
    double HeadwaterWidth,
    double MainRiverWidth,
    double MaximumRiverDepth,
    double HeadwaterValleyWidth,
    double MainValleyWidth,
    double MaximumValleyDepth,
    double MeanderStrength,
    int32 MaximumRiverReaches,
    double CanyonStartArea,
    double MinimumLakeCatchment,
    double MinimumLakeDepth,
    double MaximumLakeArea,
    int32 MaximumLakeCount,
    double LakeFalloffWidth,
    double EdgeBlendWidth,
    double SeaLevel,
    double OceanDepth,
    double CoastWidth,
    bool bOceanWidthEdges,
    bool bOceanLengthEnds
)
{
    if (!Bounds.IsValid)
    {
        return nullptr;
    }
    TSharedPtr<FAvenorGeneratedWorld> Grid =
        MakeShared<FAvenorGeneratedWorld>();
    Grid->Bounds = Bounds;
    const FVector Size = Bounds.GetSize();
    double CellSize = FMath::Max(5000.0, RequestedCellSize);
    int64 Columns = FMath::Max<int64>(
        2, FMath::CeilToInt(Size.X / CellSize)
    );
    int64 Rows = FMath::Max<int64>(
        2, FMath::CeilToInt(Size.Y / CellSize)
    );
    const int64 SafeMaximumCells = FMath::Max<int64>(
        1000, MaximumCells
    );
    if (Columns * Rows > SafeMaximumCells)
    {
        CellSize *= FMath::Sqrt(
            static_cast<double>(Columns * Rows) /
            static_cast<double>(SafeMaximumCells)
        );
        Columns = FMath::Max<int64>(
            2, FMath::CeilToInt(Size.X / CellSize)
        );
        Rows = FMath::Max<int64>(
            2, FMath::CeilToInt(Size.Y / CellSize)
        );
    }
    Grid->Columns = static_cast<int32>(Columns);
    Grid->Rows = static_cast<int32>(Rows);
    Grid->CellSize = CellSize;
    const int32 CellCount = Grid->Columns * Grid->Rows;
    Grid->TerrainHeight.SetNumUninitialized(CellCount);

    const TArray<FMountainRange> Mountains = CreateMountainRanges(
        Bounds,
        Axis,
        Seed,
        bMountains ? MountainDensity : 0.0,
        MountainLength,
        MountainWidth,
        MountainSpacing,
        MountainRelief,
        MountainExclusion,
        MountainEdgeBias
    );
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        Grid->TerrainHeight[Cell] = EvaluateLandform(
            Grid->Position(Cell),
            Bounds,
            Axis,
            Seed,
            Mountains,
            bPlains,
            bHills,
            bMountains,
            bMesas,
            bOceans,
            PlainsCoverage,
            PlainsRelief,
            HillsCoverage,
            HillScale,
            HillRelief,
            MesaCoverage,
            MesaScale,
            MesaRelief,
            MesaTerraces,
            MountainExclusion,
            EdgeBlendWidth,
            SeaLevel,
            OceanDepth,
            CoastWidth,
            bOceanWidthEdges,
            bOceanLengthEnds
        );
    }

    ApplyThermalErosion(
        *Grid,
        ThermalIterations,
        ThermalStrength,
        TalusAngle
    );
    ApplyHydraulicErosion(
        *Grid,
        HydraulicIterations,
        HydraulicIncision,
        StreamStartArea,
        FMath::Max(0.01, DrainageEpsilon)
    );
    FillDepressions(*Grid, FMath::Max(0.01, DrainageEpsilon));
    BuildFlow(*Grid);

    if (bLakes)
    {
        ExtractLakes(
            *Grid,
            MinimumLakeCatchment,
            MinimumLakeDepth,
            MaximumLakeArea,
            MaximumLakeCount,
            LakeFalloffWidth
        );
    }
    else
    {
        Grid->LakeIndexByCell.Init(INDEX_NONE, CellCount);
    }
    if (bRivers)
    {
        ExtractRivers(
            *Grid,
            Seed,
            StreamStartArea,
            MainRiverArea,
            HeadwaterWidth,
            MainRiverWidth,
            MaximumRiverDepth,
            HeadwaterValleyWidth,
            MainValleyWidth,
            MaximumValleyDepth,
            MeanderStrength,
            MaximumRiverReaches,
            bValleys,
            bCanyons,
            CanyonStartArea
        );
    }

    if (bOceans)
    {
        const double Inset = FMath::Clamp(
            CoastWidth,
            Grid->CellSize,
            FMath::Min(Size.X, Size.Y) * 0.42
        );
        const FVector2D Corners[4] = {
            FVector2D(Bounds.Min.X + Inset, Bounds.Min.Y + Inset),
            FVector2D(Bounds.Max.X - Inset, Bounds.Min.Y + Inset),
            FVector2D(Bounds.Max.X - Inset, Bounds.Max.Y - Inset),
            FVector2D(Bounds.Min.X + Inset, Bounds.Max.Y - Inset)
        };
        constexpr int32 PointsPerSide = 16;
        for (int32 Side = 0; Side < 4; ++Side)
        {
            for (int32 PointIndex = 0;
                 PointIndex < PointsPerSide;
                 ++PointIndex)
            {
                const double Alpha =
                    static_cast<double>(PointIndex) / PointsPerSide;
                const FVector2D Point = FMath::Lerp(
                    Corners[Side], Corners[(Side + 1) % 4], Alpha
                );
                Grid->OceanBoundary.Emplace(
                    Point.X, Point.Y, SeaLevel
                );
            }
        }
        Grid->OceanBoundary = SmoothPolyline(
            Grid->OceanBoundary, true, 1
        );
    }
    return Grid;
}

class FGeneratedTerrainOp final
    : public UE::MeshPartition::IModifierBackgroundOp
{
public:
    explicit FGeneratedTerrainOp(const FName& Name)
        : IModifierBackgroundOp(Name)
    {
    }

    virtual void GetInstancesInBounds(
        const FBox& InBounds,
        TArray<FInstanceInfo>& OutInstances
    ) const override
    {
        AddDefaultInstanceIfIntersects(
            GlobalBounds, InBounds, OutInstances
        );
    }

    virtual void ApplyModifications(
        UE::MeshPartition::FMeshView& MeshView,
        const FTransform3d& MeshTransform,
        const FInstanceInfo& InstanceInfo
    ) const override
    {
        (void)InstanceInfo;
        if (!WorldData)
        {
            return;
        }
        for (int32 Vertex = 0;
             Vertex < MeshView.VertexCount();
             ++Vertex)
        {
            FVector3d WorldPosition = MeshTransform.TransformPosition(
                MeshView.GetVertexPos(Vertex)
            );
            WorldPosition.Z = BaseWorldZ +
                WorldData->SampleTerrainHeight(
                    FVector2D(WorldPosition.X, WorldPosition.Y)
                );
            MeshView.SetVertexPos(
                Vertex,
                MeshTransform.InverseTransformPosition(WorldPosition)
            );
        }
    }

    virtual bool DisableDDCWrite() const override
    {
        return false;
    }

    static FGuid Version()
    {
        return FGuid(TEXT("4dfde02f-c3f5-4bc6-b14c-59d4aa8ac74e"));
    }

    FBox GlobalBounds = FBox(ForceInit);
    double BaseWorldZ = 0.0;
    TSharedPtr<const FAvenorGeneratedWorld> WorldData;
};

template<typename TWaterBodyActor>
static TWaterBodyActor* CreateWaterBody(
    UWorld* World,
    const FString& Label
)
{
    UActorFactory* Factory =
        GEditor->FindActorFactoryForActorClass(
            TWaterBodyActor::StaticClass()
        );
    if (!Factory)
    {
        return nullptr;
    }
    TWaterBodyActor* WaterBody = Cast<TWaterBodyActor>(
        Factory->CreateActor(
            TWaterBodyActor::StaticClass(),
            World->GetCurrentLevel(),
            FTransform::Identity
        )
    );
    if (WaterBody)
    {
        WaterBody->SetActorLabel(Label);
        WaterBody->SetFolderPath(TEXT("Avenor/Generated/Water"));
        WaterBody->Tags.AddUnique(GeneratedWaterTag);
    }
    return WaterBody;
}

static void DetachWaterTerrainModifiers(AActor& WaterBody)
{
    // The unified dataset has already carved the terrain. Native Water actors
    // are visual/physical water only; their automatically-created Mesh
    // Partition modifiers must not deform the same terrain a second time.
    TInlineComponentArray<
        UE::MeshPartition::UModifierComponent*
    > Modifiers;
    WaterBody.GetComponents(Modifiers);
    for (UE::MeshPartition::UModifierComponent* Modifier : Modifiers)
    {
        Modifier->SetAffectedMeshPartition(nullptr);
    }
}

static void ConfigureSpline(
    UWaterSplineComponent& Spline,
    const TArray<FVector>& Points,
    bool bClosed
)
{
    Spline.SetSplinePoints(
        Points,
        ESplineCoordinateSpace::World,
        false
    );
    Spline.SetClosedLoop(bClosed, false);
    for (int32 Index = 0;
         Index < Spline.GetNumberOfSplinePoints();
         ++Index)
    {
        Spline.SetSplinePointType(
            Index,
            ESplinePointType::CurveClamped,
            false
        );
    }
    Spline.UpdateSpline();
}
} // namespace UE::Avenor::WorldGeneration

using namespace UE::Avenor::WorldGeneration;

TArray<FBox> UAvenorGeneratedTerrainModifier::ComputeBounds() const
{
    const AAvenorWorldGenerator* Generator =
        Cast<AAvenorWorldGenerator>(GetOwner());
    const FBox GenerationBounds = Generator
        ? Generator->GetGenerationBounds()
        : FBox(ForceInit);
    return GenerationBounds.IsValid
        ? TArray<FBox>{GenerationBounds}
        : TArray<FBox>{};
}

TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
UAvenorGeneratedTerrainModifier::CreateBackgroundOp(
    UE::MeshPartition::EBuildType InBuildType
) const
{
    (void)InBuildType;
    TSharedPtr<FGeneratedTerrainOp> Op =
        MakeShared<FGeneratedTerrainOp>(GetFName());
    Op->GlobalBounds = ComputeCombinedBounds();
    const AAvenorWorldGenerator* Generator =
        Cast<AAvenorWorldGenerator>(GetOwner());
    if (Generator)
    {
        Op->BaseWorldZ = Generator->GetActorLocation().Z;
        Op->WorldData = Generator->GetOrGenerateWorld();
    }
    return Op;
}

FGuid UAvenorGeneratedTerrainModifier::GetCodeVersionKey() const
{
    return FGeneratedTerrainOp::Version();
}

AAvenorWorldGenerator::AAvenorWorldGenerator()
{
    PrimaryActorTick.bCanEverTick = false;
    SetIsSpatiallyLoaded(false);
    TerrainModifier = CreateDefaultSubobject<
        UAvenorGeneratedTerrainModifier>(TEXT("GeneratedTerrain"));
    SetRootComponent(TerrainModifier);
}

FBox AAvenorWorldGenerator::GetGenerationBounds() const
{
    const UE::Geometry::FAxisAlignedBox3d LocalBounds(
        -WorldCoverage * 0.5,
        WorldCoverage * 0.5
    );
    return FBox(UE::Geometry::FAxisAlignedBox3d(
        LocalBounds, GetActorTransform()
    ));
}

TSharedPtr<const FAvenorGeneratedWorld>
AAvenorWorldGenerator::GetOrGenerateWorld() const
{
    FScopeLock Lock(&GenerationMutex);
    if (!CachedWorld)
    {
        CachedWorld = GenerateWorld(
            GetGenerationBounds(),
            LongWorldAxis,
            WorldSeed,
            GenerationCellSize,
            MaximumGenerationCells,
            bGeneratePlains,
            bGenerateRollingHills,
            bGenerateMountains,
            bGenerateMesas,
            bGenerateValleys,
            bGenerateCanyons,
            bGenerateRivers,
            bGenerateLakes,
            bGenerateOceans,
            PlainsCoverage,
            PlainsRelief,
            RollingHillsCoverage,
            RollingHillsScale,
            RollingHillsRelief,
            MountainRangesPer100Km,
            MountainRangeLength,
            MountainRangeWidth,
            MountainPeakSpacing,
            MountainRelief,
            CentralMountainExclusionHalfWidth,
            MountainEdgeBias,
            MesaCoverage,
            MesaScale,
            MesaRelief,
            MesaTerraceCount,
            ThermalErosionIterations,
            ThermalErosionStrength,
            TalusAngleDegrees,
            HydraulicErosionIterations,
            HydraulicIncisionPerIteration,
            DrainageEpsilon,
            StreamStartAreaSquareKilometres,
            MainRiverAreaSquareKilometres,
            HeadwaterWidth,
            MainRiverWidth,
            MaximumRiverDepth,
            HeadwaterValleyHalfWidth,
            MainValleyHalfWidth,
            MaximumValleyDepth,
            LowlandMeanderStrength,
            MaximumRiverReaches,
            CanyonStartAreaSquareKilometres,
            MinimumLakeCatchmentSquareKilometres,
            MinimumLakeDepth,
            MaximumLakeAreaSquareKilometres,
            MaximumLakeCount,
            LakeShoreFalloffWidth,
            WorldEdgeBlendWidth,
            SeaLevel,
            OceanDepth,
            CoastTransitionWidth,
            bOceanAlongWidthEdges,
            bOceanAtLengthEnds
        );
    }
    return CachedWorld;
}

void AAvenorWorldGenerator::InvalidateGeneratedWorld()
{
    FScopeLock Lock(&GenerationMutex);
    CachedWorld.Reset();
}

double AAvenorWorldGenerator::GetGeneratedHeightAtWorldPosition(
    const FVector2D& WorldPosition
) const
{
    const TSharedPtr<const FAvenorGeneratedWorld> Data =
        GetOrGenerateWorld();
    return Data
        ? GetActorLocation().Z +
            Data->SampleTerrainHeight(WorldPosition)
        : GetActorLocation().Z;
}

void AAvenorWorldGenerator::ClearGeneratedWater()
{
#if WITH_EDITOR
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    TArray<AWaterBody*> ToDelete;
    for (TActorIterator<AWaterBody> It(World); It; ++It)
    {
        if (It->Tags.Contains(GeneratedWaterTag))
        {
            ToDelete.Add(*It);
        }
    }
    for (AWaterBody* Water : ToDelete)
    {
        DetachWaterTerrainModifiers(*Water);
        World->EditorDestroyActor(Water, true);
    }
#endif
}

void AAvenorWorldGenerator::CreateWaterBodies(
    const TSharedPtr<const FAvenorGeneratedWorld>& WorldData
)
{
#if WITH_EDITOR
    UWorld* World = GetWorld();
    UE::MeshPartition::AMeshPartition* MeshPartition =
        Cast<UE::MeshPartition::AMeshPartition>(MeshPartitionActor);
    if (!World || !MeshPartition || !WorldData)
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("Avenor World Generator requires a Mesh Partition Actor.")
        );
        return;
    }

    if (WorldData->OceanBoundary.Num() >= 4)
    {
        AWaterBodyOcean* Ocean = CreateWaterBody<AWaterBodyOcean>(
            World, TEXT("Avenor_Ocean")
        );
        if (Ocean)
        {
            TArray<FVector> Points = WorldData->OceanBoundary;
            for (FVector& Point : Points)
            {
                Point.Z += GetActorLocation().Z;
            }
            ConfigureSpline(
                *Ocean->GetWaterBodyComponent()->GetWaterSpline(),
                Points,
                true
            );
            Ocean->PostEditChange();
            DetachWaterTerrainModifiers(*Ocean);
        }
    }
    for (int32 Index = 0; Index < WorldData->Lakes.Num(); ++Index)
    {
        const FAvenorLakeDefinition& Definition =
            WorldData->Lakes[Index];
        AWaterBodyLake* Lake = CreateWaterBody<AWaterBodyLake>(
            World,
            FString::Printf(TEXT("Avenor_Lake_%02d"), Index + 1)
        );
        if (Lake)
        {
            TArray<FVector> Points = Definition.Shoreline;
            for (FVector& Point : Points)
            {
                Point.Z += GetActorLocation().Z;
            }
            ConfigureSpline(
                *Lake->GetWaterBodyComponent()->GetWaterSpline(),
                Points,
                true
            );
            Lake->PostEditChange();
            DetachWaterTerrainModifiers(*Lake);
        }
    }
    for (int32 Index = 0; Index < WorldData->Rivers.Num(); ++Index)
    {
        const FAvenorRiverDefinition& Definition =
            WorldData->Rivers[Index];
        AWaterBodyRiver* River = CreateWaterBody<AWaterBodyRiver>(
            World,
            FString::Printf(TEXT("Avenor_River_%03d"), Index + 1)
        );
        if (River)
        {
            TArray<FVector> Points =
                Definition.SplinePoints.Num() >= 2
                ? Definition.SplinePoints
                : Definition.Points;
            for (FVector& Point : Points)
            {
                Point.Z += GetActorLocation().Z;
            }
            ConfigureSpline(
                *River->GetWaterBodyComponent()->GetWaterSpline(),
                Points,
                false
            );
            River->PostEditChange();
            DetachWaterTerrainModifiers(*River);
        }
    }
#endif
}

void AAvenorWorldGenerator::RegenerateWorld()
{
#if WITH_EDITOR
    FScopedSlowTask Progress(
        4.0f,
        FText::FromString(
            TEXT("Generating Avenor landforms, erosion and hydrology...")
        )
    );
    Progress.MakeDialog();
    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Generating and eroding landforms"))
    );
    InvalidateGeneratedWorld();
    const TSharedPtr<const FAvenorGeneratedWorld> WorldData =
        GetOrGenerateWorld();
    if (!WorldData)
    {
        return;
    }
    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Replacing generated water"))
    );
    ClearGeneratedWater();
    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Creating matching Water splines"))
    );
    CreateWaterBodies(WorldData);
    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Registering terrain modifier"))
    );
    if (TerrainModifier)
    {
        TerrainModifier->SetAffectedMeshPartition(nullptr);
        TerrainModifier->BP_SetAffectedMegaMesh(
            Cast<UE::MeshPartition::AMeshPartition>(
                MeshPartitionActor
            )
        );
    }
    UE_LOG(
        LogTemp,
        Display,
        TEXT(
            "Avenor generated %d terrain cells at %.0f cm, "
            "%d river reaches and %d lakes."
        ),
        WorldData->TerrainHeight.Num(),
        WorldData->CellSize,
        WorldData->Rivers.Num(),
        WorldData->Lakes.Num()
    );
#endif
}

#if WITH_EDITOR
void AAvenorWorldGenerator::PostEditChangeProperty(
    FPropertyChangedEvent& PropertyChangedEvent
)
{
    InvalidateGeneratedWorld();
    Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif
