#include "AvenorTerrainRefinementModifier.h"

#include "AvenorTerrainModifier.h"

#include <queue>
#include <vector>

struct FAvenorTerrainAnalysisData
{
    FBox Bounds;
    int32 Columns = 0;
    int32 Rows = 0;
    double CellSize = 25000.0;
    TArray<double> BaseHeight;
    TArray<double> FilledHeight;
    TArray<double> RefinedHeight;
    TArray<double> Accumulation;
    TArray<int32> Downstream;
    TArray<int32> DrainParent;

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

    FVector2D Position(int32 IndexValue) const
    {
        const int32 X = IndexValue % Columns;
        const int32 Y = IndexValue / Columns;
        return FVector2D(
            Bounds.Min.X + (static_cast<double>(X) + 0.5) * CellSize,
            Bounds.Min.Y + (static_cast<double>(Y) + 0.5) * CellSize
        );
    }

    double Sample(const TArray<double>& Values, const FVector2D& Point) const
    {
        if (Values.IsEmpty() || Columns < 2 || Rows < 2)
        {
            return 0.0;
        }

        const double GridX =
            (Point.X - Bounds.Min.X) / CellSize - 0.5;
        const double GridY =
            (Point.Y - Bounds.Min.Y) / CellSize - 0.5;
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
};

namespace UE::Avenor::TerrainRefinement
{
struct FQueueEntry
{
    double Height = 0.0;
    int32 Index = INDEX_NONE;

    bool operator>(const FQueueEntry& Other) const
    {
        return Height > Other.Height;
    }
};

struct FSettings
{
    double CellSize = 25000.0;
    int32 MaximumCells = 500000;
    double DrainageEpsilon = 1.0;
    bool bThermalErosion = false;
    int32 ThermalIterations = 3;
    double TalusHeight = 9000.0;
    double ThermalStrength = 0.18;
    bool bStreamIncision = true;
    double StreamStartArea = 20.0;
    double MaximumIncision = 6000.0;
    double MinimumLakeFillDepth = 1000.0;
    double LakeBedDepth = 2500.0;
    bool bFloodplains = true;
    double FloodplainStartArea = 80.0;
    double FloodplainSmoothing = 0.35;
};

static constexpr int32 DX[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
static constexpr int32 DY[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

static void FillDepressions(
    FAvenorTerrainAnalysisData& Grid,
    double Epsilon
)
{
    const int32 Count = Grid.Columns * Grid.Rows;
    Grid.FilledHeight = Grid.BaseHeight;
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
        const int32 CurrentX = Current.Index % Grid.Columns;
        const int32 CurrentY = Current.Index / Grid.Columns;
        for (int32 Direction = 0; Direction < 8; ++Direction)
        {
            const int32 X = CurrentX + DX[Direction];
            const int32 Y = CurrentY + DY[Direction];
            if (!Grid.IsValid(X, Y))
            {
                continue;
            }
            const int32 Cell = Grid.Index(X, Y);
            if (Visited[Cell])
            {
                continue;
            }
            Visited[Cell] = true;
            Grid.DrainParent[Cell] = Current.Index;
            Grid.FilledHeight[Cell] = FMath::Max(
                Grid.BaseHeight[Cell],
                Current.Height + Epsilon
            );
            Queue.push({Grid.FilledHeight[Cell], Cell});
        }
    }
}

static void BuildFlow(FAvenorTerrainAnalysisData& Grid)
{
    const int32 Count = Grid.Columns * Grid.Rows;
    Grid.Downstream.Init(INDEX_NONE, Count);
    Grid.Accumulation.Init(1.0, Count);
    for (int32 Y = 0; Y < Grid.Rows; ++Y)
    {
        for (int32 X = 0; X < Grid.Columns; ++X)
        {
            if (Grid.IsBoundary(X, Y))
            {
                continue;
            }
            const int32 Current = Grid.Index(X, Y);
            double BestHeight = Grid.BaseHeight[Current];
            int32 Best = Grid.DrainParent.IsValidIndex(Current)
                ? Grid.DrainParent[Current]
                : INDEX_NONE;
            for (int32 Direction = 0; Direction < 8; ++Direction)
            {
                const int32 NX = X + DX[Direction];
                const int32 NY = Y + DY[Direction];
                if (!Grid.IsValid(NX, NY))
                {
                    continue;
                }
                const int32 Neighbour = Grid.Index(NX, NY);
                if (Grid.BaseHeight[Neighbour] < BestHeight &&
                    Grid.FilledHeight[Neighbour] <
                        Grid.FilledHeight[Current])
                {
                    BestHeight = Grid.BaseHeight[Neighbour];
                    Best = Neighbour;
                }
            }
            Grid.Downstream[Current] = Best;
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

static void ApplyThermalErosion(
    FAvenorTerrainAnalysisData& Grid,
    const FSettings& Settings
)
{
    if (!Settings.bThermalErosion ||
        Settings.ThermalIterations <= 0 ||
        Settings.ThermalStrength <= 0.0)
    {
        return;
    }

    TArray<double> Delta;
    Delta.SetNumZeroed(Grid.RefinedHeight.Num());
    for (int32 Iteration = 0;
         Iteration < Settings.ThermalIterations;
         ++Iteration)
    {
        FMemory::Memzero(
            Delta.GetData(),
            Delta.Num() * sizeof(double)
        );
        for (int32 Y = 1; Y < Grid.Rows - 1; ++Y)
        {
            for (int32 X = 1; X < Grid.Columns - 1; ++X)
            {
                const int32 Cell = Grid.Index(X, Y);
                double GreatestDrop = Settings.TalusHeight;
                int32 Lowest = INDEX_NONE;
                for (int32 Direction = 0; Direction < 8; ++Direction)
                {
                    const int32 Neighbour = Grid.Index(
                        X + DX[Direction],
                        Y + DY[Direction]
                    );
                    const double Drop =
                        Grid.RefinedHeight[Cell] -
                        Grid.RefinedHeight[Neighbour];
                    if (Drop > GreatestDrop)
                    {
                        GreatestDrop = Drop;
                        Lowest = Neighbour;
                    }
                }
                if (Lowest != INDEX_NONE)
                {
                    const double Transfer =
                        (GreatestDrop - Settings.TalusHeight) *
                        Settings.ThermalStrength;
                    Delta[Cell] -= Transfer;
                    Delta[Lowest] += Transfer;
                }
            }
        }
        for (int32 Cell = 0; Cell < Delta.Num(); ++Cell)
        {
            Grid.RefinedHeight[Cell] += Delta[Cell];
        }
    }
}

static void ApplyDrainageRefinement(
    FAvenorTerrainAnalysisData& Grid,
    const FSettings& Settings
)
{
    TArray<double> ChannelDelta;
    ChannelDelta.SetNumZeroed(Grid.RefinedHeight.Num());
    for (int32 Y = 1; Y < Grid.Rows - 1; ++Y)
    {
        for (int32 X = 1; X < Grid.Columns - 1; ++X)
        {
            const int32 Cell = Grid.Index(X, Y);
            const double Area = Grid.Accumulation[Cell];
            if (Settings.bStreamIncision &&
                Area >= Settings.StreamStartArea)
            {
                const double Strength = FMath::Clamp(
                    FMath::Loge(
                        Area / Settings.StreamStartArea + 1.0
                    ) / 5.0,
                    0.0,
                    1.0
                );
                const double Incision =
                    Settings.MaximumIncision * Strength;
                const int32 Radius = FMath::Clamp(
                    FMath::FloorToInt(
                        FMath::Sqrt(
                            Area / Settings.StreamStartArea
                        ) * 0.45
                    ),
                    0,
                    3
                );
                for (int32 OffsetY = -Radius;
                     OffsetY <= Radius;
                     ++OffsetY)
                {
                    for (int32 OffsetX = -Radius;
                         OffsetX <= Radius;
                         ++OffsetX)
                    {
                        const double Distance = FMath::Sqrt(
                            static_cast<double>(
                                OffsetX * OffsetX +
                                OffsetY * OffsetY
                            )
                        );
                        if (Distance > Radius + 0.25)
                        {
                            continue;
                        }
                        const int32 AffectedX = X + OffsetX;
                        const int32 AffectedY = Y + OffsetY;
                        if (!Grid.IsValid(AffectedX, AffectedY))
                        {
                            continue;
                        }
                        const int32 Affected = Grid.Index(
                            AffectedX,
                            AffectedY
                        );
                        const double Falloff = Radius > 0
                            ? 1.0 - Distance / (Radius + 0.5)
                            : 1.0;
                        ChannelDelta[Affected] = FMath::Min(
                            ChannelDelta[Affected],
                            -Incision * Falloff
                        );
                    }
                }
            }
        }
    }
    for (int32 Cell = 0; Cell < Grid.RefinedHeight.Num(); ++Cell)
    {
        Grid.RefinedHeight[Cell] += ChannelDelta[Cell];
        const double FillDepth =
            Grid.FilledHeight[Cell] - Grid.BaseHeight[Cell];
        if (FillDepth >= Settings.MinimumLakeFillDepth)
        {
            Grid.RefinedHeight[Cell] = FMath::Min(
                Grid.RefinedHeight[Cell],
                Grid.FilledHeight[Cell] - Settings.LakeBedDepth
            );
        }
    }

    if (!Settings.bFloodplains)
    {
        return;
    }
    TArray<double> Smoothed = Grid.RefinedHeight;
    for (int32 Y = 1; Y < Grid.Rows - 1; ++Y)
    {
        for (int32 X = 1; X < Grid.Columns - 1; ++X)
        {
            const int32 Cell = Grid.Index(X, Y);
            const double Area = Grid.Accumulation[Cell];
            if (Settings.bFloodplains &&
                Area >= Settings.FloodplainStartArea)
            {
                double NeighbourAverage = 0.0;
                for (int32 Direction = 0; Direction < 8; ++Direction)
                {
                    NeighbourAverage += Grid.RefinedHeight[
                        Grid.Index(
                            X + DX[Direction],
                            Y + DY[Direction]
                        )
                    ];
                }
                NeighbourAverage /= 8.0;
                const double AreaWeight = FMath::Clamp(
                    FMath::Loge(
                        Area / Settings.FloodplainStartArea + 1.0
                    ) / 4.0,
                    0.0,
                    1.0
                );
                Smoothed[Cell] = FMath::Lerp(
                    Grid.RefinedHeight[Cell],
                    FMath::Min(
                        Grid.RefinedHeight[Cell],
                        NeighbourAverage
                    ),
                    Settings.FloodplainSmoothing * AreaWeight
                );
            }
        }
    }
    Grid.RefinedHeight = MoveTemp(Smoothed);
}

static TSharedPtr<const FAvenorTerrainAnalysisData> BuildAnalysis(
    const UAvenorTerrainModifier& BaseTerrain,
    const FSettings& Settings
)
{
    const FBox Bounds = BaseTerrain.GetTerrainWorldBounds();
    if (!Bounds.IsValid)
    {
        return nullptr;
    }

    TSharedPtr<FAvenorTerrainAnalysisData> Grid =
        MakeShared<FAvenorTerrainAnalysisData>();
    Grid->Bounds = Bounds;
    Grid->CellSize = FMath::Max(5000.0, Settings.CellSize);
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
    if (CellCount64 > Settings.MaximumCells)
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT(
                "Avenor refinement requires %lld cells; safety limit is %d. "
                "Increase Analysis Cell Size or Maximum Analysis Cells."
            ),
            CellCount64,
            Settings.MaximumCells
        );
        return nullptr;
    }

    const int32 CellCount = static_cast<int32>(CellCount64);
    Grid->BaseHeight.SetNumUninitialized(CellCount);
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        Grid->BaseHeight[Cell] =
            BaseTerrain.EvaluateBaseHeightAtWorldPosition(
                Grid->Position(Cell)
            );
    }
    Grid->RefinedHeight = Grid->BaseHeight;
    FillDepressions(*Grid, FMath::Max(0.01, Settings.DrainageEpsilon));
    BuildFlow(*Grid);
    ApplyThermalErosion(*Grid, Settings);
    ApplyDrainageRefinement(*Grid, Settings);
    return Grid;
}

struct FBoundaryEdge
{
    FIntPoint Start;
    FIntPoint End;
    bool bUsed = false;
};

static double SignedArea(const TArray<FIntPoint>& Loop)
{
    double Area = 0.0;
    for (int32 Index = 0; Index < Loop.Num(); ++Index)
    {
        const FIntPoint& A = Loop[Index];
        const FIntPoint& B = Loop[(Index + 1) % Loop.Num()];
        Area += static_cast<double>(A.X) * B.Y -
            static_cast<double>(B.X) * A.Y;
    }
    return Area * 0.5;
}

static TArray<FIntPoint> TraceLargestBoundary(
    const FAvenorTerrainAnalysisData& Grid,
    const TSet<int32>& Basin
)
{
    TArray<FBoundaryEdge> Edges;
    auto AddEdge = [&Edges](
        const FIntPoint& Start,
        const FIntPoint& End
    )
    {
        Edges.Add({Start, End, false});
    };

    for (int32 Cell : Basin)
    {
        const int32 X = Cell % Grid.Columns;
        const int32 Y = Cell / Grid.Columns;
        if (Y == 0 || !Basin.Contains(Grid.Index(X, Y - 1)))
        {
            AddEdge(FIntPoint(X, Y), FIntPoint(X + 1, Y));
        }
        if (X == Grid.Columns - 1 ||
            !Basin.Contains(Grid.Index(X + 1, Y)))
        {
            AddEdge(
                FIntPoint(X + 1, Y),
                FIntPoint(X + 1, Y + 1)
            );
        }
        if (Y == Grid.Rows - 1 ||
            !Basin.Contains(Grid.Index(X, Y + 1)))
        {
            AddEdge(
                FIntPoint(X + 1, Y + 1),
                FIntPoint(X, Y + 1)
            );
        }
        if (X == 0 || !Basin.Contains(Grid.Index(X - 1, Y)))
        {
            AddEdge(FIntPoint(X, Y + 1), FIntPoint(X, Y));
        }
    }

    TArray<FIntPoint> Largest;
    double LargestArea = 0.0;
    for (int32 StartEdge = 0;
         StartEdge < Edges.Num();
         ++StartEdge)
    {
        if (Edges[StartEdge].bUsed)
        {
            continue;
        }

        TArray<FIntPoint> Loop;
        int32 EdgeIndex = StartEdge;
        const FIntPoint First = Edges[StartEdge].Start;
        for (int32 Guard = 0; Guard <= Edges.Num(); ++Guard)
        {
            FBoundaryEdge& Edge = Edges[EdgeIndex];
            if (Edge.bUsed)
            {
                break;
            }
            Edge.bUsed = true;
            Loop.Add(Edge.Start);
            const FIntPoint NextStart = Edge.End;
            if (NextStart == First)
            {
                break;
            }

            EdgeIndex = INDEX_NONE;
            for (int32 Candidate = 0;
                 Candidate < Edges.Num();
                 ++Candidate)
            {
                if (!Edges[Candidate].bUsed &&
                    Edges[Candidate].Start == NextStart)
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

        const double Area = FMath::Abs(SignedArea(Loop));
        if (Loop.Num() >= 4 && Area > LargestArea)
        {
            LargestArea = Area;
            Largest = MoveTemp(Loop);
        }
    }
    return Largest;
}

static void ExtractRiverReaches(
    const FAvenorTerrainAnalysisData& Grid,
    double StreamThreshold,
    int32 MaximumReaches,
    double SurfaceInset,
    TArray<FAvenorRiverReach>& OutRivers
)
{
    const int32 Count = Grid.Downstream.Num();
    TArray<bool> IsChannel;
    IsChannel.Init(false, Count);
    TArray<int32> UpstreamCount;
    UpstreamCount.Init(0, Count);
    for (int32 Cell = 0; Cell < Count; ++Cell)
    {
        IsChannel[Cell] =
            Grid.Accumulation[Cell] >= StreamThreshold &&
            Grid.Downstream[Cell] != INDEX_NONE;
    }
    for (int32 Cell = 0; Cell < Count; ++Cell)
    {
        const int32 Downstream = Grid.Downstream[Cell];
        if (IsChannel[Cell] &&
            Downstream != INDEX_NONE &&
            IsChannel[Downstream])
        {
            ++UpstreamCount[Downstream];
        }
    }

    for (int32 Start = 0; Start < Count; ++Start)
    {
        if (!IsChannel[Start] || UpstreamCount[Start] == 1)
        {
            continue;
        }
        int32 Current = Start;
        TArray<int32> Cells;
        TSet<int32> Seen;
        for (int32 Guard = 0; Guard < Count; ++Guard)
        {
            if (Current == INDEX_NONE ||
                Seen.Contains(Current) ||
                !IsChannel[Current])
            {
                break;
            }
            Seen.Add(Current);
            Cells.Add(Current);
            const int32 Next = Grid.Downstream[Current];
            if (Next == INDEX_NONE || !IsChannel[Next])
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

        FAvenorRiverReach Reach;
        Reach.DischargeCells = Grid.Accumulation[Cells.Last()];
        Reach.Points.Reserve(Cells.Num());
        double PreviousZ = TNumericLimits<double>::Max();
        for (int32 Cell : Cells)
        {
            const FVector2D XY = Grid.Position(Cell);
            // The refined height is the carved bed. The water surface follows
            // the pre-incision landform, inset slightly, and is constrained
            // to run downhill along the same drainage reach.
            const double Z = FMath::Min(
                PreviousZ - 1.0,
                Grid.BaseHeight[Cell] - SurfaceInset
            );
            Reach.Points.Emplace(XY.X, XY.Y, Z);
            PreviousZ = Z;
        }
        OutRivers.Add(MoveTemp(Reach));
    }

    OutRivers.Sort([](
        const FAvenorRiverReach& A,
        const FAvenorRiverReach& B
    )
    {
        return A.DischargeCells > B.DischargeCells;
    });
    if (MaximumReaches >= 0 && OutRivers.Num() > MaximumReaches)
    {
        OutRivers.SetNum(MaximumReaches);
    }
}

static void ExtractLakeBasins(
    const FAvenorTerrainAnalysisData& Grid,
    double MinimumFillDepth,
    int32 MaximumLakes,
    TArray<FAvenorLakeBasin>& OutLakes
)
{
    const int32 Count = Grid.BaseHeight.Num();
    TArray<bool> Visited;
    Visited.Init(false, Count);
    static constexpr int32 DX4[4] = {-1, 1, 0, 0};
    static constexpr int32 DY4[4] = {0, 0, -1, 1};

    for (int32 Seed = 0; Seed < Count; ++Seed)
    {
        if (Visited[Seed] ||
            Grid.FilledHeight[Seed] - Grid.BaseHeight[Seed] <
                MinimumFillDepth)
        {
            continue;
        }

        TSet<int32> Basin;
        TArray<int32> Queue;
        Queue.Add(Seed);
        Visited[Seed] = true;
        bool bTouchesBoundary = false;
        double SurfaceHeight = TNumericLimits<double>::Lowest();
        for (int32 Head = 0; Head < Queue.Num(); ++Head)
        {
            const int32 Cell = Queue[Head];
            Basin.Add(Cell);
            // Priority-flood raises a depression to its spill surface. Use
            // the highest filled elevation in the connected depression, not
            // its lowest floor.
            SurfaceHeight = FMath::Max(
                SurfaceHeight,
                Grid.FilledHeight[Cell]
            );
            const int32 X = Cell % Grid.Columns;
            const int32 Y = Cell / Grid.Columns;
            bTouchesBoundary |= Grid.IsBoundary(X, Y);
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
                        Grid.BaseHeight[Neighbour] >=
                            MinimumFillDepth)
                {
                    Visited[Neighbour] = true;
                    Queue.Add(Neighbour);
                }
            }
        }
        if (bTouchesBoundary || Basin.Num() < 4)
        {
            continue;
        }

        const TArray<FIntPoint> Boundary =
            TraceLargestBoundary(Grid, Basin);
        if (Boundary.Num() < 4)
        {
            continue;
        }

        FAvenorLakeBasin Lake;
        Lake.SurfaceHeight = SurfaceHeight;
        Lake.CellCount = Basin.Num();
        const int32 Stride = FMath::Max(1, Boundary.Num() / 96);
        for (int32 Index = 0;
             Index < Boundary.Num();
             Index += Stride)
        {
            const FIntPoint Corner = Boundary[Index];
            Lake.Shoreline.Emplace(
                Grid.Bounds.Min.X + Corner.X * Grid.CellSize,
                Grid.Bounds.Min.Y + Corner.Y * Grid.CellSize,
                SurfaceHeight
            );
        }
        OutLakes.Add(MoveTemp(Lake));
    }

    OutLakes.Sort([](
        const FAvenorLakeBasin& A,
        const FAvenorLakeBasin& B
    )
    {
        return A.CellCount > B.CellCount;
    });
    if (MaximumLakes >= 0 && OutLakes.Num() > MaximumLakes)
    {
        OutLakes.SetNum(MaximumLakes);
    }
}

class FBackgroundOp final
    : public UE::MeshPartition::IModifierBackgroundOp
{
public:
    explicit FBackgroundOp(const FName& OperationName)
        : IModifierBackgroundOp(OperationName)
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
        if (!Analysis)
        {
            return;
        }
        for (int32 VertexIndex = 0;
             VertexIndex < MeshView.VertexCount();
             ++VertexIndex)
        {
            FVector3d WorldPosition = MeshTransform.TransformPosition(
                MeshView.GetVertexPos(VertexIndex)
            );
            const FVector2D Point(WorldPosition.X, WorldPosition.Y);
            const double Delta =
                Analysis->Sample(Analysis->RefinedHeight, Point) -
                Analysis->Sample(Analysis->BaseHeight, Point);
            const double EdgeDistance = FMath::Min(
                FMath::Min(
                    Point.X - Analysis->Bounds.Min.X,
                    Analysis->Bounds.Max.X - Point.X
                ),
                FMath::Min(
                    Point.Y - Analysis->Bounds.Min.Y,
                    Analysis->Bounds.Max.Y - Point.Y
                )
            );
            const double EdgeWeight = FMath::SmoothStep(
                0.0,
                Analysis->CellSize * 2.0,
                FMath::Max(0.0, EdgeDistance)
            );
            WorldPosition.Z += Delta * EdgeWeight;
            MeshView.SetVertexPos(
                VertexIndex,
                MeshTransform.InverseTransformPosition(WorldPosition)
            );
        }
    }

    static FGuid CodeVersion()
    {
        static const FGuid Version(
            TEXT("52c542a7-314f-45ae-a304-d0080d9ddcf4")
        );
        return Version;
    }

    virtual bool DisableDDCWrite() const override
    {
        return false;
    }

    FBox GlobalBounds;
    TSharedPtr<const FAvenorTerrainAnalysisData> Analysis;
};
} // namespace UE::Avenor::TerrainRefinement

using namespace UE::Avenor::TerrainRefinement;

UAvenorTerrainRefinementModifier::UAvenorTerrainRefinementModifier()
{
}

UAvenorTerrainModifier*
UAvenorTerrainRefinementModifier::ResolveBaseTerrain() const
{
    return BaseTerrainModifierActor
        ? BaseTerrainModifierActor->FindComponentByClass<
            UAvenorTerrainModifier>()
        : nullptr;
}

TSharedPtr<const FAvenorTerrainAnalysisData>
UAvenorTerrainRefinementModifier::GetOrBuildAnalysis() const
{
    FScopeLock Lock(&AnalysisMutex);
    if (CachedAnalysis)
    {
        return CachedAnalysis;
    }
    const UAvenorTerrainModifier* BaseTerrain = ResolveBaseTerrain();
    if (!BaseTerrain)
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT(
                "Avenor refinement requires Base Terrain Modifier Actor."
            )
        );
        return nullptr;
    }

    FSettings Settings;
    Settings.CellSize = AnalysisCellSize;
    Settings.MaximumCells = MaximumAnalysisCells;
    Settings.DrainageEpsilon = DrainageEpsilon;
    Settings.bThermalErosion = bEnableThermalErosion;
    Settings.ThermalIterations = ThermalErosionIterations;
    Settings.TalusHeight = TalusHeight;
    Settings.ThermalStrength = ThermalErosionStrength;
    Settings.bStreamIncision = bEnableStreamIncision;
    Settings.StreamStartArea = StreamStartAreaCells;
    Settings.MaximumIncision = MaximumStreamIncision;
    Settings.MinimumLakeFillDepth = MinimumLakeFillDepth;
    Settings.LakeBedDepth = LakeBedDepth;
    Settings.bFloodplains = bEnableFloodplains;
    Settings.FloodplainStartArea = FloodplainStartAreaCells;
    Settings.FloodplainSmoothing = FloodplainSmoothing;
    CachedAnalysis = BuildAnalysis(*BaseTerrain, Settings);
    return CachedAnalysis;
}

void UAvenorTerrainRefinementModifier::InvalidateAnalysis()
{
    FScopeLock Lock(&AnalysisMutex);
    CachedAnalysis.Reset();
}

double
UAvenorTerrainRefinementModifier::EvaluateRefinedHeightAtWorldPosition(
    const FVector2D& WorldPosition
) const
{
    const TSharedPtr<const FAvenorTerrainAnalysisData> Analysis =
        GetOrBuildAnalysis();
    return Analysis
        ? Analysis->Sample(Analysis->RefinedHeight, WorldPosition)
        : 0.0;
}

double
UAvenorTerrainRefinementModifier::EvaluateFlowAccumulationAtWorldPosition(
    const FVector2D& WorldPosition
) const
{
    const TSharedPtr<const FAvenorTerrainAnalysisData> Analysis =
        GetOrBuildAnalysis();
    return Analysis
        ? Analysis->Sample(Analysis->Accumulation, WorldPosition)
        : 0.0;
}

double
UAvenorTerrainRefinementModifier::EvaluateLakeFillDepthAtWorldPosition(
    const FVector2D& WorldPosition
) const
{
    const TSharedPtr<const FAvenorTerrainAnalysisData> Analysis =
        GetOrBuildAnalysis();
    return Analysis
        ? FMath::Max(
            0.0,
            Analysis->Sample(Analysis->FilledHeight, WorldPosition) -
            Analysis->Sample(Analysis->BaseHeight, WorldPosition)
        )
        : 0.0;
}

bool UAvenorTerrainRefinementModifier::GetHydrologyFeatures(
    int32 MaximumRiverReaches,
    int32 MaximumLakes,
    double RiverSurfaceInset,
    TArray<FAvenorRiverReach>& OutRivers,
    TArray<FAvenorLakeBasin>& OutLakes
) const
{
    OutRivers.Reset();
    OutLakes.Reset();
    const TSharedPtr<const FAvenorTerrainAnalysisData> Analysis =
        GetOrBuildAnalysis();
    if (!Analysis)
    {
        return false;
    }

    ExtractRiverReaches(
        *Analysis,
        StreamStartAreaCells,
        MaximumRiverReaches,
        FMath::Max(1.0, RiverSurfaceInset),
        OutRivers
    );
    ExtractLakeBasins(
        *Analysis,
        MinimumLakeFillDepth,
        MaximumLakes,
        OutLakes
    );
    return true;
}

FBox UAvenorTerrainRefinementModifier::GetAnalysisWorldBounds() const
{
    const UAvenorTerrainModifier* BaseTerrain = ResolveBaseTerrain();
    return BaseTerrain ? BaseTerrain->GetTerrainWorldBounds() : FBox(ForceInit);
}

TArray<FBox> UAvenorTerrainRefinementModifier::ComputeBounds() const
{
    const FBox AnalysisBounds = GetAnalysisWorldBounds();
    return AnalysisBounds.IsValid
        ? TArray<FBox>{AnalysisBounds}
        : TArray<FBox>{};
}

TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
UAvenorTerrainRefinementModifier::CreateBackgroundOp(
    UE::MeshPartition::EBuildType InBuildType
) const
{
    (void)InBuildType;
    TSharedPtr<FBackgroundOp> Op =
        MakeShared<FBackgroundOp>(GetFName());
    Op->GlobalBounds = ComputeCombinedBounds();
    Op->Analysis = GetOrBuildAnalysis();
    return Op;
}

FGuid UAvenorTerrainRefinementModifier::GetCodeVersionKey() const
{
    return FBackgroundOp::CodeVersion();
}

#if WITH_EDITOR
void UAvenorTerrainRefinementModifier::PostEditChangeProperty(
    FPropertyChangedEvent& PropertyChangedEvent
)
{
    InvalidateAnalysis();
    Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif
