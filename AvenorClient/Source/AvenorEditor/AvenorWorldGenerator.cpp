#include "AvenorWorldGenerator.h"

#include "SpineGenerator.h"

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
    double DischargeCells = 0.0;
    double Width = 600.0;
    double Depth = 250.0;
    bool bContainsWaterfall = false;
    FBox2D InfluenceBounds = FBox2D(ForceInit);
};

struct FAvenorLakeDefinition
{
    TArray<FVector> Shoreline;
    double SurfaceHeight = 0.0;
    double Volume = 0.0;
    int32 CellCount = 0;
};

struct FAvenorGeneratedWorld
{
    FBox Bounds;
    int32 Columns = 0;
    int32 Rows = 0;
    double CellSize = 10000.0;
    TArray<double> BaseHeight;
    TArray<double> Height;
    TArray<double> FilledHeight;
    TArray<double> Accumulation;
    TArray<int32> Downstream;
    TArray<int32> DrainParent;
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
        return X == 0 || Y == 0 || X == Columns - 1 || Y == Rows - 1;
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

    double Sample(
        const TArray<double>& Values,
        const FVector2D& PositionValue
    ) const
    {
        if (Values.IsEmpty() || Columns < 2 || Rows < 2)
        {
            return 0.0;
        }
        const double GridX =
            (PositionValue.X - Bounds.Min.X) / CellSize - 0.5;
        const double GridY =
            (PositionValue.Y - Bounds.Min.Y) / CellSize - 0.5;
        const int32 X0 = FMath::Clamp(
            FMath::FloorToInt(GridX), 0, Columns - 1
        );
        const int32 Y0 = FMath::Clamp(
            FMath::FloorToInt(GridY), 0, Rows - 1
        );
        const int32 X1 = FMath::Min(X0 + 1, Columns - 1);
        const int32 Y1 = FMath::Min(Y0 + 1, Rows - 1);
        const double AlphaX = FMath::Clamp(GridX - X0, 0.0, 1.0);
        const double AlphaY = FMath::Clamp(GridY - Y0, 0.0, 1.0);
        return FMath::Lerp(
            FMath::Lerp(
                Values[Index(X0, Y0)],
                Values[Index(X1, Y0)],
                AlphaX
            ),
            FMath::Lerp(
                Values[Index(X0, Y1)],
                Values[Index(X1, Y1)],
                AlphaX
            ),
            AlphaY
        );
    }

    // The same curved river definitions used for UE Water splines cut the
    // final sub-cell channel. The coarse drainage grid selects the route, but
    // it never leaves a second, straight channel underneath the spline.
    double SampleTerrainHeight(const FVector2D& Point) const
    {
        double Result = Sample(Height, Point);
        for (const FAvenorRiverDefinition& River : Rivers)
        {
            if (!River.InfluenceBounds.IsInside(Point))
            {
                continue;
            }
            for (int32 Index = 0;
                 Index + 1 < River.Points.Num();
                 ++Index)
            {
                const FVector& A = River.Points[Index];
                const FVector& B = River.Points[Index + 1];
                const FVector2D Segment(
                    B.X - A.X,
                    B.Y - A.Y
                );
                const double LengthSquared = Segment.SizeSquared();
                const double Alpha =
                    LengthSquared > UE_DOUBLE_KINDA_SMALL_NUMBER
                    ? FMath::Clamp(
                        FVector2D::DotProduct(
                            Point - FVector2D(A),
                            Segment
                        ) / LengthSquared,
                        0.0,
                        1.0
                    )
                    : 0.0;
                const FVector2D Closest =
                    FVector2D(A) + Segment * Alpha;
                const double Distance =
                    FVector2D::Distance(Point, Closest);
                const double HalfBedWidth =
                    FMath::Max(100.0, River.Width * 0.5);
                const double BankWidth =
                    FMath::Max(CellSize * 0.35, River.Width);
                if (Distance >= HalfBedWidth + BankWidth)
                {
                    continue;
                }
                const double SurfaceHeight =
                    FMath::Lerp(A.Z, B.Z, Alpha);
                const double BedHeight =
                    SurfaceHeight - River.Depth;
                const double BankAlpha = FMath::Clamp(
                    (Distance - HalfBedWidth) / BankWidth,
                    0.0,
                    1.0
                );
                const double SmoothBankAlpha =
                    BankAlpha * BankAlpha *
                    (3.0 - 2.0 * BankAlpha);
                const double Weight = Distance <= HalfBedWidth
                    ? 1.0
                    : 1.0 - SmoothBankAlpha;
                Result = FMath::Min(
                    Result,
                    FMath::Lerp(Result, BedHeight, Weight)
                );
            }
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
    double Phase = 0.0;
    double Relief = 0.0;
};

struct FQueueEntry
{
    double Height = 0.0;
    int32 Cell = INDEX_NONE;

    bool operator>(const FQueueEntry& Other) const
    {
        return Height > Other.Height;
    }
};

static double Smooth01(double Value)
{
    const double T = FMath::Clamp(Value, 0.0, 1.0);
    return T * T * (3.0 - 2.0 * T);
}

static double Quintic01(double Value)
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

static double DistanceToSegment(
    const FVector2D& Point,
    const FVector2D& A,
    const FVector2D& B
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
    return FVector2D::Distance(Point, A + Segment * Alpha);
}

static double DistanceToPolyline(
    const FVector2D& Point,
    const TArray<FVector2D>& Polyline
)
{
    double Best = TNumericLimits<double>::Max();
    for (int32 Index = 0; Index + 1 < Polyline.Num(); ++Index)
    {
        Best = FMath::Min(
            Best,
            DistanceToSegment(
                Point,
                Polyline[Index],
                Polyline[Index + 1]
            )
        );
    }
    return Best;
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
        TArray<FVector> Result;
        if (!bClosed)
        {
            Result.Add(Points[0]);
        }
        const int32 SegmentCount =
            bClosed ? Points.Num() : Points.Num() - 1;
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

static TArray<FVector2D> BuildSpinePoints(
    const AAvenorWorldGenerator& Generator,
    const FBox& Bounds,
    const ASpineGenerator* Spine,
    double CellSize
)
{
    TArray<FVector2D> Points;
    if (Spine)
    {
        const double HalfLength = Bounds.GetSize().X * 0.5;
        const double Step = FMath::Max(10000.0, CellSize * 2.0);
        for (double Chainage = -HalfLength;
             Chainage < HalfLength;
             Chainage += Step)
        {
            const FVector Point =
                Spine->GetSpineLocationAtChainage(Chainage);
            Points.Emplace(Point.X, Point.Y);
        }
        const FVector End =
            Spine->GetSpineLocationAtChainage(HalfLength);
        Points.Emplace(End.X, End.Y);
    }
    else
    {
        const FVector Centre = Generator.GetActorLocation();
        Points.Emplace(Bounds.Min.X, Centre.Y);
        Points.Emplace(Bounds.Max.X, Centre.Y);
    }
    return Points;
}

static double EvaluateBaseLandform(
    const AAvenorWorldGenerator& Generator,
    const FVector2D& Point,
    const FBox& Bounds,
    const TArray<FVector2D>& SpinePoints,
    const TArray<FMountainRange>& Mountains,
    int32 Seed,
    bool bPlains,
    bool bHills,
    bool bMesas,
    bool bOceans,
    double PlainsFrequency,
    double PlainRelief,
    double HillsFrequency,
    double HillScale,
    double HillRelief,
    double MesaFrequency,
    double MesaScale,
    double MesaRelief,
    int32 MesaTerraces,
    double GentleHalfWidth,
    double FullRoughnessDistance,
    double CorridorFraction,
    double CoastWidth,
    double OceanDepth,
    double EdgeBlendWidth
)
{
    const FVector2D Offset(
        static_cast<double>(Seed) * 13.17,
        static_cast<double>(Seed) * -7.91
    );
    const double SpineDistance = DistanceToPolyline(Point, SpinePoints);
    const double RoughnessTransition = Smooth01(
        (SpineDistance - GentleHalfWidth) /
        FMath::Max(1.0, FullRoughnessDistance - GentleHalfWidth)
    );
    const double Roughness = FMath::Lerp(
        CorridorFraction,
        1.0,
        RoughnessTransition
    );

    const double RegionScale = FMath::Max(300000.0, HillScale * 4.5);
    const double Selector =
        0.5 + 0.5 * Noise(Point, RegionScale, Offset * 5.37);
    const double PlainEnd = bPlains
        ? FMath::Clamp(PlainsFrequency, 0.0, 1.0)
        : 0.0;
    const double HillEnd = bHills
        ? FMath::Clamp(PlainEnd + HillsFrequency, PlainEnd, 1.0)
        : PlainEnd;
    const double MesaStart = FMath::Clamp(
        1.0 - (bMesas ? MesaFrequency : 0.0),
        HillEnd,
        1.0
    );
    const double Blend = 0.04;
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
    const double RemainingMask = FMath::Max(
        0.0,
        1.0 - PlainMask - HillMask - MesaMask
    );

    const FVector2D Warp(
        Noise(Point, HillScale * 2.1, Offset * 0.37),
        Noise(Point, HillScale * 2.1, Offset * 0.61)
    );
    const FVector2D Warped = Point + Warp * HillScale * 0.55;
    const double Regional =
        Noise(Warped, RegionScale, Offset) * PlainRelief * 1.4;
    const double Broad =
        Noise(Warped, HillScale, Offset * 1.73);
    const double Medium =
        Noise(Warped, HillScale * 0.43, Offset * 2.41);
    const double Ridge =
        1.0 - FMath::Abs(
            Noise(Warped, HillScale * 0.72, Offset * 3.19)
        );
    const double PlainHeight = Broad * PlainRelief;
    const double HillHeight =
        (Broad * 0.62 + Medium * 0.26 +
            (Ridge * 2.0 - 1.0) * 0.12) * HillRelief;
    const int32 Terraces = FMath::Max(1, MesaTerraces);
    const double MesaNoise =
        0.5 + 0.5 * Noise(Warped, MesaScale, Offset * 6.71);
    const double MesaHeight =
        FMath::FloorToDouble(MesaNoise * Terraces) /
        static_cast<double>(Terraces) * MesaRelief;

    double Height = Regional + Roughness * (
        PlainHeight * PlainMask +
        HillHeight * (HillMask + RemainingMask) +
        MesaHeight * MesaMask
    );

    for (const FMountainRange& Mountain : Mountains)
    {
        const FVector2D Delta = Point - Mountain.Centre;
        const double Along =
            FVector2D::DotProduct(Delta, Mountain.Along);
        const double Across =
            FVector2D::DotProduct(Delta, Mountain.Across);
        const double RangeMeander =
            FMath::Sin(
                Along / FMath::Max(1.0, Mountain.PeakSpacing) * PI +
                Mountain.Phase
            ) * Mountain.HalfWidth * 0.22;
        const double X = Along / FMath::Max(1.0, Mountain.HalfLength);
        const double Y = (Across - RangeMeander) /
            FMath::Max(1.0, Mountain.HalfWidth);
        const double Ellipse = FMath::Sqrt(X * X + Y * Y);
        if (Ellipse >= 1.0)
        {
            continue;
        }
        const double Envelope =
            FMath::Pow(Quintic01(1.0 - Ellipse), 0.72);
        const double PeakTrain = 0.52 + 0.48 * FMath::Pow(
            0.5 + 0.5 * FMath::Cos(
                Along / FMath::Max(1.0, Mountain.PeakSpacing) *
                    2.0 * PI +
                Mountain.Phase
            ),
            1.7
        );
        const double Detail = 0.72 + 0.28 * (
            1.0 - FMath::Abs(
                Noise(Point, HillScale * 0.55, Offset * 4.17)
            )
        );
        Height += Mountain.Relief * Envelope * PeakTrain * Detail *
            RoughnessTransition;
    }

    const FVector BoundsCentre = Bounds.GetCenter();
    const FVector BoundsExtent = Bounds.GetExtent();
    const FVector2D Centre(BoundsCentre.X, BoundsCentre.Y);
    const FVector2D Extent(BoundsExtent.X, BoundsExtent.Y);
    const FVector2D FromCentre = Point - Centre;
    const double EdgeDistance = FMath::Min(
        Extent.X - FMath::Abs(FromCentre.X),
        Extent.Y - FMath::Abs(FromCentre.Y)
    );
    if (bOceans)
    {
        const double BrokenCoast = EdgeDistance +
            Noise(Point, RegionScale, Offset * 9.41) * CoastWidth * 0.8;
        const double OceanMask = 1.0 - Smooth01(
            BrokenCoast / FMath::Max(1.0, CoastWidth)
        );
        Height -= OceanMask * OceanDepth;
    }
    else if (EdgeBlendWidth > 0.0)
    {
        Height *= Smooth01(EdgeDistance / EdgeBlendWidth);
    }
    return Height;
}

static void FillDepressions(
    FAvenorGeneratedWorld& Grid,
    double Epsilon
)
{
    const int32 Count = Grid.Height.Num();
    Grid.FilledHeight = Grid.Height;
    Grid.DrainParent.Init(INDEX_NONE, Count);
    TArray<bool> Visited;
    Visited.Init(false, Count);
    std::priority_queue<
        FQueueEntry,
        std::vector<FQueueEntry>,
        std::greater<FQueueEntry>
    > Queue;

    auto AddBoundary = [&](int32 X, int32 Y)
    {
        const int32 Cell = Grid.Index(X, Y);
        if (!Visited[Cell])
        {
            Visited[Cell] = true;
            Queue.push({Grid.FilledHeight[Cell], Cell});
        }
    };
    for (int32 X = 0; X < Grid.Columns; ++X)
    {
        AddBoundary(X, 0);
        AddBoundary(X, Grid.Rows - 1);
    }
    for (int32 Y = 0; Y < Grid.Rows; ++Y)
    {
        AddBoundary(0, Y);
        AddBoundary(Grid.Columns - 1, Y);
    }

    while (!Queue.empty())
    {
        const FQueueEntry Current = Queue.top();
        Queue.pop();
        const int32 X = Current.Cell % Grid.Columns;
        const int32 Y = Current.Cell / Grid.Columns;
        for (int32 Direction = 0; Direction < 8; ++Direction)
        {
            const int32 NX = X + DX[Direction];
            const int32 NY = Y + DY[Direction];
            if (!Grid.IsValid(NX, NY))
            {
                continue;
            }
            const int32 Cell = Grid.Index(NX, NY);
            if (Visited[Cell])
            {
                continue;
            }
            Visited[Cell] = true;
            Grid.DrainParent[Cell] = Current.Cell;
            Grid.FilledHeight[Cell] = FMath::Max(
                Grid.Height[Cell],
                Current.Height + Epsilon
            );
            Queue.push({Grid.FilledHeight[Cell], Cell});
        }
    }
}

static void BuildFlow(FAvenorGeneratedWorld& Grid)
{
    const int32 Count = Grid.Height.Num();
    Grid.Downstream.Init(INDEX_NONE, Count);
    Grid.Accumulation.Init(1.0, Count);
    for (int32 Y = 1; Y < Grid.Rows - 1; ++Y)
    {
        for (int32 X = 1; X < Grid.Columns - 1; ++X)
        {
            const int32 Cell = Grid.Index(X, Y);
            double BestSlope = 0.0;
            int32 Best = INDEX_NONE;
            for (int32 Direction = 0; Direction < 8; ++Direction)
            {
                const int32 Neighbour = Grid.Index(
                    X + DX[Direction],
                    Y + DY[Direction]
                );
                const double Distance = Grid.CellSize *
                    ((DX[Direction] != 0 && DY[Direction] != 0)
                        ? 1.4142135623730951
                        : 1.0);
                const double Slope =
                    (Grid.FilledHeight[Cell] -
                        Grid.FilledHeight[Neighbour]) / Distance;
                if (Slope > BestSlope)
                {
                    BestSlope = Slope;
                    Best = Neighbour;
                }
            }
            if (Best == INDEX_NONE)
            {
                Best = Grid.DrainParent[Cell];
            }
            Grid.Downstream[Cell] = Best;
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
            Grid.Accumulation[Downstream] += Grid.Accumulation[Cell];
        }
    }
}

static void CarveDrainage(
    FAvenorGeneratedWorld& Grid,
    bool bRivers,
    bool bValleys,
    bool bCanyons,
    double StreamStart,
    double MaximumRiverDepth,
    double ValleyStart,
    double MaximumValleyDepth,
    int32 MaximumValleyHalfWidth,
    double CanyonStart,
    double MaximumCanyonDepth,
    int32 Iterations
)
{
    if (!bRivers && !bValleys && !bCanyons)
    {
        return;
    }
    const int32 SafeIterations = FMath::Max(1, Iterations);
    for (int32 Iteration = 0; Iteration < SafeIterations; ++Iteration)
    {
        FillDepressions(Grid, 1.0);
        BuildFlow(Grid);
        TArray<double> Delta;
        Delta.SetNumZeroed(Grid.Height.Num());
        for (int32 Y = 1; Y < Grid.Rows - 1; ++Y)
        {
            for (int32 X = 1; X < Grid.Columns - 1; ++X)
            {
                const int32 Cell = Grid.Index(X, Y);
                const double Area = Grid.Accumulation[Cell];
                double Incision = 0.0;
                int32 Radius = 1;
                if (bRivers && Area >= StreamStart)
                {
                    const double Strength = FMath::Clamp(
                        FMath::Loge(Area / StreamStart + 1.0) / 5.0,
                        0.0,
                        1.0
                    );
                    Incision += MaximumRiverDepth * Strength /
                        SafeIterations;
                    Radius = FMath::Clamp(
                        1 + FMath::FloorToInt(
                            FMath::Sqrt(Area / StreamStart) * 0.12
                        ),
                        1,
                        3
                    );
                }
                if (bValleys && Area >= ValleyStart)
                {
                    const double Strength = FMath::Clamp(
                        FMath::Loge(Area / ValleyStart + 1.0) / 5.0,
                        0.0,
                        1.0
                    );
                    Incision += MaximumValleyDepth * Strength /
                        SafeIterations;
                    Radius = FMath::Max(
                        Radius,
                        FMath::Clamp(
                            2 + FMath::FloorToInt(
                                FMath::Sqrt(Area / ValleyStart) * 0.18
                            ),
                            2,
                            FMath::Max(2, MaximumValleyHalfWidth)
                        )
                    );
                }
                if (bCanyons && Area >= CanyonStart)
                {
                    const double Strength = FMath::Clamp(
                        FMath::Loge(Area / CanyonStart + 1.0) / 4.0,
                        0.0,
                        1.0
                    );
                    Incision += MaximumCanyonDepth * Strength /
                        SafeIterations;
                    Radius = FMath::Max(Radius, 2);
                }
                if (Incision <= 0.0)
                {
                    continue;
                }
                for (int32 OY = -Radius; OY <= Radius; ++OY)
                {
                    for (int32 OX = -Radius; OX <= Radius; ++OX)
                    {
                        const double Distance = FMath::Sqrt(
                            static_cast<double>(OX * OX + OY * OY)
                        );
                        if (Distance > Radius + 0.25)
                        {
                            continue;
                        }
                        const int32 AffectedX = X + OX;
                        const int32 AffectedY = Y + OY;
                        if (!Grid.IsValid(AffectedX, AffectedY))
                        {
                            continue;
                        }
                        const int32 Affected =
                            Grid.Index(AffectedX, AffectedY);
                        const double Falloff = FMath::Square(
                            FMath::Clamp(
                                1.0 - Distance / (Radius + 0.5),
                                0.0,
                                1.0
                            )
                        );
                        Delta[Affected] = FMath::Min(
                            Delta[Affected],
                            -Incision * Falloff
                        );
                    }
                }
            }
        }
        for (int32 Cell = 0; Cell < Grid.Height.Num(); ++Cell)
        {
            Grid.Height[Cell] += Delta[Cell];
        }
    }
    FillDepressions(Grid, 1.0);
    BuildFlow(Grid);
}

static void AddMeanders(
    const FAvenorGeneratedWorld& Grid,
    TArray<FVector>& Points,
    double Strength
)
{
    if (Points.Num() < 3 || Strength <= 0.0)
    {
        return;
    }
    const double Phase = FMath::Fmod(
        FMath::Abs(Points[0].X * 0.000017 +
            Points[0].Y * 0.000031),
        2.0 * PI
    );
    for (int32 Index = 1; Index < Points.Num() - 1; ++Index)
    {
        const FVector2D Previous(Points[Index - 1]);
        const FVector2D Next(Points[Index + 1]);
        const FVector2D Direction =
            (Next - Previous).GetSafeNormal();
        const double Run = FMath::Max(
            1.0,
            FVector2D::Distance(Previous, Next)
        );
        const double Slope = FMath::Abs(
            Points[Index - 1].Z - Points[Index + 1].Z
        ) / Run;
        const double Flatness = FMath::Lerp(
            0.35,
            1.0,
            1.0 - FMath::SmoothStep(0.002, 0.02, Slope)
        );
        const FVector2D Normal(-Direction.Y, Direction.X);
        const double EndWeight = FMath::Sin(
            PI * static_cast<double>(Index) /
            static_cast<double>(Points.Num() - 1)
        );
        // Two deterministic wavelengths avoid the regular single-sine look.
        // Steep headwaters remain comparatively direct; broad lowland reaches
        // receive the full lateral migration.
        const double MeanderSignal =
            FMath::Sin(Phase + Index * 0.55) * 0.76 +
            FMath::Sin(Phase * 1.73 + Index * 1.27) * 0.24;
        const double Offset =
            MeanderSignal * Grid.CellSize * Strength *
            Flatness * EndWeight;
        Points[Index].X += Normal.X * Offset;
        Points[Index].Y += Normal.Y * Offset;
    }
}

static TArray<FVector> ResampleRiverCentreline(
    const TArray<FVector>& Input,
    double Spacing
)
{
    TArray<FVector> Result;
    if (Input.Num() < 2)
    {
        return Input;
    }
    Result.Add(Input[0]);
    const double SafeSpacing = FMath::Max(100.0, Spacing);
    for (int32 Index = 0; Index + 1 < Input.Num(); ++Index)
    {
        const FVector& A = Input[Index];
        const FVector& B = Input[Index + 1];
        const double SegmentLength =
            FVector2D::Distance(FVector2D(A), FVector2D(B));
        const int32 Steps = FMath::Max(
            2,
            FMath::CeilToInt(SegmentLength / SafeSpacing)
        );
        for (int32 Step = 1; Step <= Steps; ++Step)
        {
            Result.Add(FMath::Lerp(
                A,
                B,
                static_cast<double>(Step) / Steps
            ));
        }
    }
    return Result;
}

static void ExtractRivers(
    FAvenorGeneratedWorld& Grid,
    double StreamStart,
    double HeadwaterWidth,
    double MainWidth,
    double MaximumDepth,
    double MeanderStrength,
    int32 MaximumReaches,
    bool bWaterfalls,
    double MinimumWaterfallDrop
)
{
    Grid.Rivers.Reset();
    const int32 Count = Grid.Height.Num();
    TArray<bool> Channel;
    TArray<int32> UpstreamCount;
    double Threshold = FMath::Max(1.0, StreamStart);
    for (int32 Attempt = 0; Attempt < 24; ++Attempt)
    {
        Channel.Init(false, Count);
        UpstreamCount.Init(0, Count);
        for (int32 Cell = 0; Cell < Count; ++Cell)
        {
            Channel[Cell] =
                Grid.Accumulation[Cell] >= Threshold &&
                Grid.Downstream[Cell] != INDEX_NONE;
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
        Threshold *= 1.3;
    }

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
            if (Current != Start && UpstreamCount[Current] != 1)
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
        River.DischargeCells = Grid.Accumulation[Cells.Last()];
        const double WidthAlpha = FMath::Clamp(
            FMath::Loge(
                River.DischargeCells / FMath::Max(1.0, StreamStart) +
                1.0
            ) / 6.0,
            0.0,
            1.0
        );
        River.Width = FMath::Lerp(
            HeadwaterWidth,
            MainWidth,
            WidthAlpha
        );
        River.Depth = FMath::Lerp(
            FMath::Min(250.0, MaximumDepth),
            MaximumDepth,
            WidthAlpha
        );
        double PreviousSurface = TNumericLimits<double>::Max();
        for (int32 Cell : Cells)
        {
            const FVector2D XY = Grid.Position(Cell);
            const double LocalDepth = FMath::Lerp(
                80.0,
                350.0,
                WidthAlpha
            );
            const double Surface = FMath::Min(
                PreviousSurface - 1.0,
                Grid.Height[Cell] + LocalDepth
            );
            if (bWaterfalls &&
                PreviousSurface < TNumericLimits<double>::Max() &&
                PreviousSurface - Surface >= MinimumWaterfallDrop)
            {
                River.bContainsWaterfall = true;
            }
            River.Points.Emplace(XY.X, XY.Y, Surface);
            PreviousSurface = Surface;
        }
        River.Points = ResampleRiverCentreline(
            River.Points,
            Grid.CellSize * 0.25
        );
        AddMeanders(Grid, River.Points, MeanderStrength);
        River.Points = SmoothPolyline(River.Points, false, 2);
        PreviousSurface = TNumericLimits<double>::Max();
        for (FVector& Point : River.Points)
        {
            Point.Z = FMath::Min(
                PreviousSurface - 1.0,
                Grid.Sample(Grid.Height, FVector2D(Point)) + 100.0
            );
            PreviousSurface = Point.Z;
        }
        FBox2D CentrelineBounds(ForceInit);
        for (const FVector& Point : River.Points)
        {
            CentrelineBounds += FVector2D(Point);
        }
        River.InfluenceBounds = CentrelineBounds.ExpandBy(
            FMath::Max(Grid.CellSize * 0.35, River.Width) +
            River.Width * 0.5
        );
        Grid.Rivers.Add(MoveTemp(River));
    }
    Grid.Rivers.Sort([](
        const FAvenorRiverDefinition& A,
        const FAvenorRiverDefinition& B
    )
    {
        return A.DischargeCells > B.DischargeCells;
    });
}

struct FBoundaryEdge
{
    FIntPoint Start;
    FIntPoint End;
    FVector2D Position = FVector2D::ZeroVector;
    bool bUsed = false;
};

static TArray<FVector> TraceLakeBoundary(
    const FAvenorGeneratedWorld& Grid,
    const TSet<int32>& WetCells,
    double SurfaceHeight
)
{
    TArray<FBoundaryEdge> Edges;
    auto Add = [&](
        int32 Cell,
        int32 OutsideX,
        int32 OutsideY,
        const FIntPoint& Start,
        const FIntPoint& End)
    {
        FVector2D Position;
        if (Grid.IsValid(OutsideX, OutsideY))
        {
            const int32 Outside = Grid.Index(OutsideX, OutsideY);
            const double InsideHeight = Grid.Height[Cell];
            const double OutsideHeight = Grid.Height[Outside];
            const double Denominator = OutsideHeight - InsideHeight;
            const double Alpha = FMath::Abs(Denominator) > 1.0
                ? FMath::Clamp(
                    (SurfaceHeight - InsideHeight) / Denominator,
                    0.05,
                    0.95
                )
                : 0.5;
            Position = FMath::Lerp(
                Grid.Position(Cell),
                Grid.Position(Outside),
                Alpha
            );
        }
        else
        {
            Position = FVector2D(
                Grid.Bounds.Min.X +
                    (Start.X + End.X) * 0.5 * Grid.CellSize,
                Grid.Bounds.Min.Y +
                    (Start.Y + End.Y) * 0.5 * Grid.CellSize
            );
        }
        Edges.Add({Start, End, Position, false});
    };

    for (int32 Cell : WetCells)
    {
        const int32 X = Cell % Grid.Columns;
        const int32 Y = Cell / Grid.Columns;
        if (Y == 0 || !WetCells.Contains(Grid.Index(X, Y - 1)))
        {
            Add(Cell, X, Y - 1, FIntPoint(X, Y), FIntPoint(X + 1, Y));
        }
        if (X == Grid.Columns - 1 ||
            !WetCells.Contains(Grid.Index(X + 1, Y)))
        {
            Add(
                Cell,
                X + 1,
                Y,
                FIntPoint(X + 1, Y),
                FIntPoint(X + 1, Y + 1)
            );
        }
        if (Y == Grid.Rows - 1 ||
            !WetCells.Contains(Grid.Index(X, Y + 1)))
        {
            Add(
                Cell,
                X,
                Y + 1,
                FIntPoint(X + 1, Y + 1),
                FIntPoint(X, Y + 1)
            );
        }
        if (X == 0 || !WetCells.Contains(Grid.Index(X - 1, Y)))
        {
            Add(
                Cell,
                X - 1,
                Y,
                FIntPoint(X, Y + 1),
                FIntPoint(X, Y)
            );
        }
    }

    TArray<FVector> Largest;
    for (int32 StartIndex = 0;
         StartIndex < Edges.Num();
         ++StartIndex)
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
            FBoundaryEdge& Edge = Edges[EdgeIndex];
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
            for (int32 Candidate = 0;
                 Candidate < Edges.Num();
                 ++Candidate)
            {
                if (!Edges[Candidate].bUsed &&
                    Edges[Candidate].Start == Edge.End)
                {
                    EdgeIndex = Candidate;
                    break;
                }
            }
            if (EdgeIndex == INDEX_NONE)
            {
                Loop.Reset();
                break;
            }
        }
        if (Loop.Num() > Largest.Num())
        {
            Largest = MoveTemp(Loop);
        }
    }
    return SmoothPolyline(Largest, true, 2);
}

static void ExtractLakes(
    FAvenorGeneratedWorld& Grid,
    double MinimumCatchment,
    double MinimumDepth,
    double RunoffDepth,
    double MaximumAreaSquareKilometres,
    int32 MaximumLakes
)
{
    Grid.Lakes.Reset();
    const int32 Count = Grid.Height.Num();
    TArray<bool> Visited;
    Visited.Init(false, Count);
    static constexpr int32 DX4[4] = {-1, 1, 0, 0};
    static constexpr int32 DY4[4] = {0, 0, -1, 1};
    const double CellArea = Grid.CellSize * Grid.CellSize;

    for (int32 Seed = 0; Seed < Count; ++Seed)
    {
        if (Visited[Seed] ||
            Grid.FilledHeight[Seed] - Grid.Height[Seed] < 1.0)
        {
            continue;
        }
        TSet<int32> Basin;
        TArray<int32> Queue;
        Queue.Add(Seed);
        Visited[Seed] = true;
        bool bBoundary = false;
        double SpillHeight = TNumericLimits<double>::Lowest();
        double FloorHeight = TNumericLimits<double>::Max();
        double Catchment = 0.0;
        for (int32 Head = 0; Head < Queue.Num(); ++Head)
        {
            const int32 Cell = Queue[Head];
            Basin.Add(Cell);
            SpillHeight = FMath::Max(
                SpillHeight,
                Grid.FilledHeight[Cell]
            );
            FloorHeight = FMath::Min(FloorHeight, Grid.Height[Cell]);
            Catchment = FMath::Max(
                Catchment,
                Grid.Accumulation[Cell]
            );
            const int32 X = Cell % Grid.Columns;
            const int32 Y = Cell / Grid.Columns;
            bBoundary |= Grid.IsBoundary(X, Y);
            for (int32 Direction = 0; Direction < 4; ++Direction)
            {
                const int32 NX = X + DX4[Direction];
                const int32 NY = Y + DY4[Direction];
                if (!Grid.IsValid(NX, NY))
                {
                    continue;
                }
                const int32 Neighbour = Grid.Index(NX, NY);
                if (!Visited[Neighbour] &&
                    Grid.FilledHeight[Neighbour] -
                        Grid.Height[Neighbour] >= 1.0)
                {
                    Visited[Neighbour] = true;
                    Queue.Add(Neighbour);
                }
            }
        }
        if (bBoundary || Basin.Num() < 4 ||
            Catchment < MinimumCatchment ||
            SpillHeight - FloorHeight < MinimumDepth)
        {
            continue;
        }

        double Capacity = 0.0;
        for (int32 Cell : Basin)
        {
            Capacity += FMath::Max(
                0.0,
                SpillHeight - Grid.Height[Cell]
            ) * CellArea;
        }
        const double AvailableVolume = FMath::Min(
            Capacity,
            Catchment * CellArea * RunoffDepth
        );
        double Low = FloorHeight;
        double High = SpillHeight;
        for (int32 Iteration = 0; Iteration < 32; ++Iteration)
        {
            const double Candidate = (Low + High) * 0.5;
            double Volume = 0.0;
            for (int32 Cell : Basin)
            {
                Volume += FMath::Max(
                    0.0,
                    Candidate - Grid.Height[Cell]
                ) * CellArea;
            }
            if (Volume < AvailableVolume)
            {
                Low = Candidate;
            }
            else
            {
                High = Candidate;
            }
        }
        const double Surface = (Low + High) * 0.5;
        if (Surface - FloorHeight < MinimumDepth)
        {
            continue;
        }
        TSet<int32> Wet;
        int32 MinimumX = Grid.Columns;
        int32 MinimumY = Grid.Rows;
        int32 MaximumX = 0;
        int32 MaximumY = 0;
        for (int32 Cell : Basin)
        {
            if (Grid.Height[Cell] < Surface - 1.0)
            {
                Wet.Add(Cell);
                const int32 X = Cell % Grid.Columns;
                const int32 Y = Cell / Grid.Columns;
                MinimumX = FMath::Min(MinimumX, X);
                MinimumY = FMath::Min(MinimumY, Y);
                MaximumX = FMath::Max(MaximumX, X);
                MaximumY = FMath::Max(MaximumY, Y);
            }
        }
        const double AreaSquareKilometres =
            Wet.Num() * CellArea / 10000000000.0;
        const int32 WidthCells = MaximumX - MinimumX + 1;
        const int32 HeightCells = MaximumY - MinimumY + 1;
        const int32 ShortAxis = FMath::Min(WidthCells, HeightCells);
        const int32 LongAxis = FMath::Max(WidthCells, HeightCells);
        const double AspectRatio = static_cast<double>(LongAxis) /
            FMath::Max(1, ShortAxis);
        const double FootprintOccupancy =
            static_cast<double>(Wet.Num()) /
            FMath::Max(1, WidthCells * HeightCells);
        if (Wet.Num() < 4 ||
            AreaSquareKilometres > MaximumAreaSquareKilometres ||
            ShortAxis < 3 ||
            AspectRatio > 4.0 ||
            FootprintOccupancy < 0.18)
        {
            continue;
        }

        FAvenorLakeDefinition Lake;
        Lake.SurfaceHeight = Surface;
        Lake.Volume = AvailableVolume;
        Lake.CellCount = Wet.Num();
        Lake.Shoreline = TraceLakeBoundary(Grid, Wet, Surface);
        if (Lake.Shoreline.Num() >= 4)
        {
            Grid.Lakes.Add(MoveTemp(Lake));
        }
    }
    Grid.Lakes.Sort([](
        const FAvenorLakeDefinition& A,
        const FAvenorLakeDefinition& B
    )
    {
        return A.Volume > B.Volume;
    });
    if (Grid.Lakes.Num() > MaximumLakes)
    {
        Grid.Lakes.SetNum(MaximumLakes);
    }
}

static TSharedPtr<const FAvenorGeneratedWorld> GenerateWorld(
    const AAvenorWorldGenerator& Generator,
    const FBox& Bounds,
    const ASpineGenerator* Spine,
    int32 Seed,
    double CellSize,
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
    bool bWaterfalls,
    double PlainsFrequency,
    double PlainRelief,
    double HillsFrequency,
    double HillScale,
    double HillRelief,
    int32 MountainCount,
    double MountainLength,
    double MountainWidth,
    double MountainSpacing,
    double MountainRelief,
    double MountainSpineDistance,
    double MesaFrequency,
    double MesaScale,
    double MesaRelief,
    int32 MesaTerraces,
    double ValleyStart,
    double ValleyDepth,
    int32 ValleyHalfWidth,
    double CanyonStart,
    double CanyonDepth,
    int32 ErosionIterations,
    double StreamStart,
    double RiverDepth,
    double HeadwaterWidth,
    double MainWidth,
    double Meander,
    int32 MaximumRivers,
    double LakeCatchment,
    double LakeDepth,
    double RunoffDepth,
    double MaximumLakeArea,
    int32 MaximumLakes,
    double CoastWidth,
    double OceanDepth,
    double SeaLevel,
    double GentleHalfWidth,
    double FullRoughnessDistance,
    double CorridorFraction,
    double EdgeBlendWidth,
    double DrainageEpsilon,
    double MinimumWaterfallDrop
)
{
    if (!Bounds.IsValid)
    {
        return nullptr;
    }
    TSharedPtr<FAvenorGeneratedWorld> Grid =
        MakeShared<FAvenorGeneratedWorld>();
    Grid->Bounds = Bounds;
    Grid->CellSize = FMath::Max(5000.0, CellSize);
    Grid->Columns = FMath::Max(
        3,
        FMath::CeilToInt(Bounds.GetSize().X / Grid->CellSize)
    );
    Grid->Rows = FMath::Max(
        3,
        FMath::CeilToInt(Bounds.GetSize().Y / Grid->CellSize)
    );
    const int64 CellCount64 =
        static_cast<int64>(Grid->Columns) * Grid->Rows;
    if (CellCount64 > MaximumCells)
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT(
                "Avenor world generation requires %lld cells; limit is %d."
            ),
            CellCount64,
            MaximumCells
        );
        return nullptr;
    }

    const TArray<FVector2D> SpinePoints = BuildSpinePoints(
        Generator,
        Bounds,
        Spine,
        Grid->CellSize
    );
    FRandomStream Random(Seed);
    TArray<FMountainRange> Mountains;
    for (int32 Index = 0;
         Index < (bMountains ? MountainCount : 0);
         ++Index)
    {
        FVector2D Centre;
        for (int32 Attempt = 0; Attempt < 64; ++Attempt)
        {
            Centre = FVector2D(
                Random.FRandRange(Bounds.Min.X, Bounds.Max.X),
                Random.FRandRange(Bounds.Min.Y, Bounds.Max.Y)
            );
            if (DistanceToPolyline(Centre, SpinePoints) >=
                MountainSpineDistance)
            {
                break;
            }
        }
        const double Angle = Random.FRandRange(-PI, PI);
        FMountainRange Range;
        Range.Centre = Centre;
        Range.Along = FVector2D(
            FMath::Cos(Angle),
            FMath::Sin(Angle)
        );
        Range.Across = FVector2D(-Range.Along.Y, Range.Along.X);
        Range.HalfLength =
            MountainLength * Random.FRandRange(0.38, 0.62);
        Range.HalfWidth =
            MountainWidth * Random.FRandRange(0.38, 0.68);
        Range.PeakSpacing =
            MountainSpacing * Random.FRandRange(0.8, 1.2);
        Range.Phase = Random.FRandRange(-PI, PI);
        Range.Relief =
            MountainRelief * Random.FRandRange(0.75, 1.25);
        Mountains.Add(Range);
    }

    const int32 CellCount = static_cast<int32>(CellCount64);
    Grid->BaseHeight.SetNumUninitialized(CellCount);
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        Grid->BaseHeight[Cell] = EvaluateBaseLandform(
            Generator,
            Grid->Position(Cell),
            Bounds,
            SpinePoints,
            Mountains,
            Seed,
            bPlains,
            bHills,
            bMesas,
            bOceans,
            PlainsFrequency,
            PlainRelief,
            HillsFrequency,
            HillScale,
            HillRelief,
            MesaFrequency,
            MesaScale,
            MesaRelief,
            MesaTerraces,
            GentleHalfWidth,
            FullRoughnessDistance,
            CorridorFraction,
            CoastWidth,
            OceanDepth,
            EdgeBlendWidth
        );
    }
    Grid->Height = Grid->BaseHeight;
    CarveDrainage(
        *Grid,
        false,
        bValleys,
        bCanyons,
        StreamStart,
        RiverDepth,
        ValleyStart,
        ValleyDepth,
        ValleyHalfWidth,
        CanyonStart,
        CanyonDepth,
        ErosionIterations
    );
    FillDepressions(*Grid, FMath::Max(0.01, DrainageEpsilon));
    BuildFlow(*Grid);
    if (bLakes)
    {
        ExtractLakes(
            *Grid,
            LakeCatchment,
            LakeDepth,
            RunoffDepth,
            MaximumLakeArea,
            MaximumLakes
        );
    }
    if (bRivers)
    {
        ExtractRivers(
            *Grid,
            StreamStart,
            HeadwaterWidth,
            MainWidth,
            RiverDepth,
            Meander,
            MaximumRivers,
            bWaterfalls,
            MinimumWaterfallDrop
        );
    }
    if (bOceans)
    {
        const FVector Extent = Bounds.GetExtent();
        const double Inset = FMath::Clamp(
            CoastWidth * 0.5,
            Grid->CellSize,
            FMath::Min(Extent.X, Extent.Y) * 0.4
        );
        const FVector2D Corners[4] = {
            FVector2D(Bounds.Min.X + Inset, Bounds.Min.Y + Inset),
            FVector2D(Bounds.Max.X - Inset, Bounds.Min.Y + Inset),
            FVector2D(Bounds.Max.X - Inset, Bounds.Max.Y - Inset),
            FVector2D(Bounds.Min.X + Inset, Bounds.Max.Y - Inset)
        };
        constexpr int32 PointsPerSide = 12;
        for (int32 Side = 0; Side < 4; ++Side)
        {
            for (int32 PointIndex = 0;
                 PointIndex < PointsPerSide;
                 ++PointIndex)
            {
                const double Alpha =
                    static_cast<double>(PointIndex) / PointsPerSide;
                const FVector2D Point = FMath::Lerp(
                    Corners[Side],
                    Corners[(Side + 1) % 4],
                    Alpha
                );
                Grid->OceanBoundary.Emplace(
                    Point.X,
                    Point.Y,
                    SeaLevel
                );
            }
        }
        Grid->OceanBoundary = SmoothPolyline(
            Grid->OceanBoundary,
            true,
            1
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
            GlobalBounds,
            InBounds,
            OutInstances
        );
    }

    virtual void ApplyModifications(
        UE::MeshPartition::FMeshView& MeshView,
        const FTransform3d& MeshTransform,
        const FInstanceInfo& InstanceInfo
    ) const override
    {
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
        return FGuid(TEXT("9d77144f-2b0f-4ae1-bbc9-66c25d88a6da"));
    }

    FBox GlobalBounds;
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
    if (!WaterBody)
    {
        return nullptr;
    }
    WaterBody->SetActorLabel(Label);
    WaterBody->SetFolderPath(TEXT("Avenor/Generated/Water"));
    WaterBody->Tags.AddUnique(GeneratedWaterTag);
    // The unified generator has already carved this exact shoreline/channel
    // into its heightfield. Attaching MeshPartitionWater modifiers here would
    // apply a second deformation and raise lake margins into platforms.
    return WaterBody;
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
        LocalBounds,
        GetActorTransform()
    ));
}

TSharedPtr<const FAvenorGeneratedWorld>
AAvenorWorldGenerator::GetOrGenerateWorld() const
{
    FScopeLock Lock(&GenerationMutex);
    if (!CachedWorld)
    {
        CachedWorld = GenerateWorld(
            *this,
            GetGenerationBounds(),
            Spine,
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
            bGenerateWaterfalls,
            PlainsFrequency,
            PlainsRelief,
            RollingHillsFrequency,
            RollingHillsScale,
            RollingHillsRelief,
            MountainRangeCount,
            MountainRangeLength,
            MountainRangeWidth,
            MountainPeakSpacing,
            MountainRelief,
            MountainMinimumSpineDistance,
            MesaFrequency,
            MesaScale,
            MesaRelief,
            MesaTerraceCount,
            ValleyStartCatchmentCells,
            ValleyMaximumDepth,
            ValleyMaximumHalfWidthCells,
            CanyonStartCatchmentCells,
            CanyonMaximumDepth,
            ErosionIterations,
            StreamStartCatchmentCells,
            MaximumRiverDepth,
            HeadwaterWidth,
            MainRiverWidth,
            LowlandMeanderStrength,
            MaximumRiverReaches,
            MinimumLakeCatchmentCells,
            MinimumLakeDepth,
            CatchmentRunoffDepth,
            MaximumLakeAreaSquareKilometres,
            MaximumLakeCount,
            CoastTransitionWidth,
            OceanDepth,
            SeaLevel,
            GentleCorridorHalfWidth,
            FullRoughnessDistance,
            CorridorRoughnessFraction,
            WorldEdgeBlendWidth,
            DrainageEpsilon,
            MinimumWaterfallDrop
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
        TInlineComponentArray<
            UE::MeshPartition::UModifierComponent*
        > Modifiers;
        Water->GetComponents(Modifiers);
        for (UE::MeshPartition::UModifierComponent* Modifier : Modifiers)
        {
            Modifier->SetAffectedMeshPartition(nullptr);
        }
    }
    for (AWaterBody* Water : ToDelete)
    {
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
            TEXT(
                "Avenor World Generator requires a Mesh Partition Actor."
            )
        );
        return;
    }

    if (WorldData->OceanBoundary.Num() >= 4)
    {
        AWaterBodyOcean* Ocean = CreateWaterBody<AWaterBodyOcean>(
            World,
            TEXT("Avenor_Ocean")
        );
        if (Ocean)
        {
            TArray<FVector> WorldBoundary = WorldData->OceanBoundary;
            for (FVector& Point : WorldBoundary)
            {
                Point.Z += GetActorLocation().Z;
            }
            ConfigureSpline(
                *Ocean->GetWaterBodyComponent()->GetWaterSpline(),
                WorldBoundary,
                true
            );
            Ocean->PostEditChange();
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
            TArray<FVector> WorldShoreline = Definition.Shoreline;
            for (FVector& Point : WorldShoreline)
            {
                Point.Z += GetActorLocation().Z;
            }
            ConfigureSpline(
                *Lake->GetWaterBodyComponent()->GetWaterSpline(),
                WorldShoreline,
                true
            );
            Lake->PostEditChange();
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
            TArray<FVector> WorldPoints = Definition.Points;
            for (FVector& Point : WorldPoints)
            {
                Point.Z += GetActorLocation().Z;
            }
            ConfigureSpline(
                *River->GetWaterBodyComponent()->GetWaterSpline(),
                WorldPoints,
                false
            );
            River->PostEditChange();
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
            TEXT("Generating coherent Avenor terrain and water...")
        )
    );
    Progress.MakeDialog();
    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Generating landforms"))
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
        FText::FromString(TEXT("Creating water splines"))
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
            "Avenor generated %d terrain cells, %d river reaches and "
            "%d lakes from one world dataset."
        ),
        WorldData->Height.Num(),
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
