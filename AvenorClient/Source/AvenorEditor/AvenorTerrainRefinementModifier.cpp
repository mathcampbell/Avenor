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
    bool bThermalErosion = true;
    int32 ThermalIterations = 3;
    double TalusHeight = 9000.0;
    double ThermalStrength = 0.18;
    bool bStreamIncision = true;
    double StreamStartArea = 20.0;
    double MaximumIncision = 6000.0;
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
            double BestHeight = Grid.FilledHeight[Current];
            int32 Best = INDEX_NONE;
            for (int32 Direction = 0; Direction < 8; ++Direction)
            {
                const int32 NX = X + DX[Direction];
                const int32 NY = Y + DY[Direction];
                if (!Grid.IsValid(NX, NY))
                {
                    continue;
                }
                const int32 Neighbour = Grid.Index(NX, NY);
                if (Grid.FilledHeight[Neighbour] < BestHeight)
                {
                    BestHeight = Grid.FilledHeight[Neighbour];
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
                Grid.RefinedHeight[Cell] -=
                    Settings.MaximumIncision * Strength;
            }
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
            WorldPosition.Z += Delta;
            MeshView.SetVertexPos(
                VertexIndex,
                MeshTransform.InverseTransformPosition(WorldPosition)
            );
        }
    }

    static FGuid CodeVersion()
    {
        static const FGuid Version(
            TEXT("efda14e1-34dd-4e33-a455-4e37f015551b")
        );
        return Version;
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

FBox UAvenorTerrainRefinementModifier::GetAnalysisWorldBounds() const
{
    const UAvenorTerrainModifier* BaseTerrain = ResolveBaseTerrain();
    return BaseTerrain ? BaseTerrain->GetTerrainWorldBounds() : FBox(ForceInit);
}

TArray<FBox> UAvenorTerrainRefinementModifier::ComputeBounds() const
{
    const FBox Bounds = GetAnalysisWorldBounds();
    return Bounds.IsValid ? TArray<FBox>{Bounds} : TArray<FBox>{};
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
