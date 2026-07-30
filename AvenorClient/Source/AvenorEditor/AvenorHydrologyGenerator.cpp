#include "AvenorHydrologyGenerator.h"

#include "AvenorTerrainModifier.h"
#include "AvenorTerrainRefinementModifier.h"
#include "ActorFactories/ActorFactory.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "MeshPartition.h"
#include "MeshPartitionEditorComponent.h"
#include "MeshPartitionModifierComponent.h"
#include "Misc/ScopedSlowTask.h"
#include "UObject/UObjectGlobals.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterSplineComponent.h"

#include <queue>
#include <vector>

namespace UE::Avenor::Hydrology
{
static const FName GeneratedWaterTag(TEXT("AvenorGeneratedWater"));

struct FGrid
{
    FBox Bounds;
    int32 Columns = 0;
    int32 Rows = 0;
    double CellSize = 10000.0;
    TArray<double> TerrainHeight;
    TArray<double> FilledHeight;
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
};

struct FQueueEntry
{
    double Height = 0.0;
    int32 Index = INDEX_NONE;

    bool operator>(const FQueueEntry& Other) const
    {
        return Height > Other.Height;
    }
};

static void VisitBoundary(
    FGrid& Grid,
    TArray<bool>& Visited,
    std::priority_queue<
        FQueueEntry,
        std::vector<FQueueEntry>,
        std::greater<FQueueEntry>
    >& Queue,
    int32 X,
    int32 Y
)
{
    const int32 Index = Grid.Index(X, Y);
    if (Visited[Index])
    {
        return;
    }
    Visited[Index] = true;
    Grid.FilledHeight[Index] = Grid.TerrainHeight[Index];
    Queue.push({Grid.FilledHeight[Index], Index});
}

static void FillDepressions(FGrid& Grid, double Epsilon)
{
    const int32 Count = Grid.Columns * Grid.Rows;
    Grid.FilledHeight = Grid.TerrainHeight;
    TArray<bool> Visited;
    Visited.Init(false, Count);

    std::priority_queue<
        FQueueEntry,
        std::vector<FQueueEntry>,
        std::greater<FQueueEntry>
    > Queue;

    for (int32 X = 0; X < Grid.Columns; ++X)
    {
        VisitBoundary(Grid, Visited, Queue, X, 0);
        VisitBoundary(Grid, Visited, Queue, X, Grid.Rows - 1);
    }
    for (int32 Y = 0; Y < Grid.Rows; ++Y)
    {
        VisitBoundary(Grid, Visited, Queue, 0, Y);
        VisitBoundary(Grid, Visited, Queue, Grid.Columns - 1, Y);
    }

    static constexpr int32 DX[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static constexpr int32 DY[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

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

            const int32 Neighbour = Grid.Index(X, Y);
            if (Visited[Neighbour])
            {
                continue;
            }
            Visited[Neighbour] = true;
            Grid.FilledHeight[Neighbour] = FMath::Max(
                Grid.TerrainHeight[Neighbour],
                Current.Height + Epsilon
            );
            Queue.push({Grid.FilledHeight[Neighbour], Neighbour});
        }
    }
}

static void BuildFlow(FGrid& Grid)
{
    const int32 Count = Grid.Columns * Grid.Rows;
    Grid.Downstream.Init(INDEX_NONE, Count);
    Grid.Accumulation.Init(1.0, Count);

    static constexpr int32 DX[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static constexpr int32 DY[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

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
    for (int32 Index = 0; Index < Count; ++Index)
    {
        Order.Add(Index);
    }
    Order.Sort([&Grid](int32 A, int32 B)
    {
        return Grid.FilledHeight[A] > Grid.FilledHeight[B];
    });

    for (int32 Index : Order)
    {
        const int32 Downstream = Grid.Downstream[Index];
        if (Downstream != INDEX_NONE)
        {
            Grid.Accumulation[Downstream] += Grid.Accumulation[Index];
        }
    }
}

static TArray<int32> SelectLakeCells(
    const FGrid& Grid,
    int32 RequestedCount,
    double MinimumFillDepth,
    double MinimumSeparation
)
{
    TArray<bool> HasQualifyingUpstream;
    HasQualifyingUpstream.Init(false, Grid.Downstream.Num());
    for (int32 Upstream = 0;
         Upstream < Grid.Downstream.Num();
         ++Upstream)
    {
        const int32 Downstream = Grid.Downstream[Upstream];
        if (Downstream != INDEX_NONE &&
            Grid.Accumulation[Upstream] >= MinimumCatchmentCells)
        {
            HasQualifyingUpstream[Downstream] = true;
        }
    }

    TArray<int32> Candidates;
    for (int32 Index = 0; Index < Grid.TerrainHeight.Num(); ++Index)
    {
        const int32 X = Index % Grid.Columns;
        const int32 Y = Index / Grid.Columns;
        if (!Grid.IsBoundary(X, Y) &&
            Grid.FilledHeight[Index] - Grid.TerrainHeight[Index] >=
                MinimumFillDepth)
        {
            Candidates.Add(Index);
        }
    }

    Candidates.Sort([&Grid](int32 A, int32 B)
    {
        const double DepthA =
            Grid.FilledHeight[A] - Grid.TerrainHeight[A];
        const double DepthB =
            Grid.FilledHeight[B] - Grid.TerrainHeight[B];
        if (!FMath::IsNearlyEqual(DepthA, DepthB))
        {
            return DepthA > DepthB;
        }
        return Grid.Accumulation[A] > Grid.Accumulation[B];
    });

    TArray<int32> Selected;
    for (int32 Candidate : Candidates)
    {
        bool bSeparated = true;
        for (int32 Existing : Selected)
        {
            if (FVector2D::Distance(
                    Grid.Position(Candidate),
                    Grid.Position(Existing)
                ) < MinimumSeparation)
            {
                bSeparated = false;
                break;
            }
        }
        if (bSeparated)
        {
            Selected.Add(Candidate);
            if (Selected.Num() >= RequestedCount)
            {
                break;
            }
        }
    }
    return Selected;
}

static TArray<int32> SelectRiverSources(
    const FGrid& Grid,
    int32 RequestedCount,
    double MinimumCatchmentCells,
    double MinimumSeparation
)
{
    TArray<int32> Candidates;
    for (int32 Cell = 0; Cell < Grid.Accumulation.Num(); ++Cell)
    {
        const int32 X = Cell % Grid.Columns;
        const int32 Y = Cell / Grid.Columns;
        if (Grid.IsBoundary(X, Y) ||
            Grid.Accumulation[Cell] < MinimumCatchmentCells)
        {
            continue;
        }

        if (!HasQualifyingUpstream[Cell])
        {
            Candidates.Add(Cell);
        }
    }
    Candidates.Sort([&Grid](int32 A, int32 B)
    {
        return Grid.Accumulation[A] > Grid.Accumulation[B];
    });

    TArray<int32> Selected;
    for (int32 Candidate : Candidates)
    {
        bool bSeparated = true;
        for (int32 Existing : Selected)
        {
            if (FVector2D::Distance(
                    Grid.Position(Candidate),
                    Grid.Position(Existing)
                ) < MinimumSeparation)
            {
                bSeparated = false;
                break;
            }
        }
        if (bSeparated)
        {
            Selected.Add(Candidate);
            if (Selected.Num() >= RequestedCount)
            {
                break;
            }
        }
    }
    return Selected;
}

static FVector2D FindLakeShore(
    const FGrid& Grid,
    int32 LakeCell,
    double Angle,
    double MaximumRadius,
    double MinimumFillDepth
)
{
    const FVector2D Centre = Grid.Position(LakeCell);
    const FVector2D Direction(FMath::Cos(Angle), FMath::Sin(Angle));
    const double Step = Grid.CellSize * 0.5;
    FVector2D LastInside =
        Centre + Direction * FMath::Min(Step, MaximumRadius);
    for (double Distance = Step;
         Distance <= MaximumRadius;
         Distance += Step)
    {
        const FVector2D Point = Centre + Direction * Distance;
        const int32 X = FMath::FloorToInt(
            (Point.X - Grid.Bounds.Min.X) / Grid.CellSize
        );
        const int32 Y = FMath::FloorToInt(
            (Point.Y - Grid.Bounds.Min.Y) / Grid.CellSize
        );
        if (!Grid.IsValid(X, Y))
        {
            break;
        }
        const int32 Cell = Grid.Index(X, Y);
        const double FillDepth =
            Grid.FilledHeight[Cell] - Grid.TerrainHeight[Cell];
        if (FillDepth < MinimumFillDepth)
        {
            break;
        }
        LastInside = Point;
    }
    return LastInside;
}

static TArray<int32> TraceRiver(const FGrid& Grid, int32 Source)
{
    TArray<int32> Path;
    TSet<int32> Visited;
    int32 Current = Source;
    const int32 MaximumSteps = Grid.Columns * Grid.Rows;

    for (int32 Step = 0;
         Step < MaximumSteps && Current != INDEX_NONE;
         ++Step)
    {
        if (Visited.Contains(Current))
        {
            break;
        }
        Visited.Add(Current);
        Path.Add(Current);

        const int32 X = Current % Grid.Columns;
        const int32 Y = Current / Grid.Columns;
        if (Grid.IsBoundary(X, Y))
        {
            break;
        }
        Current = Grid.Downstream[Current];
    }
    return Path;
}

template<typename TWaterBodyActor>
static TWaterBodyActor* CreateWaterBody(
    UWorld* World,
    UE::MeshPartition::AMeshPartition* MeshPartition,
    const FString& Label,
    const TCHAR* ModifierClassPath,
    double FalloffWidth
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

    UWaterBodyComponent* WaterComponent =
        WaterBody->GetWaterBodyComponent();
    WaterComponent->WaterHeightmapSettings.FalloffSettings.FalloffMode =
        EWaterBrushFalloffMode::Width;
    WaterComponent->WaterHeightmapSettings.FalloffSettings.FalloffWidth =
        FalloffWidth;
    UClass* ModifierClass =
        LoadClass<UE::MeshPartition::UModifierComponent>(
            nullptr,
            ModifierClassPath
        );
    if (!ModifierClass)
    {
        World->EditorDestroyActor(WaterBody, true);
        return nullptr;
    }

    UE::MeshPartition::UModifierComponent* Modifier =
        NewObject<UE::MeshPartition::UModifierComponent>(
        WaterBody,
        ModifierClass,
        ModifierClass->GetFName()
    );
    Modifier->SetAffectedMeshPartition(MeshPartition);
    WaterBody->AddInstanceComponent(Modifier);
    Modifier->AttachToComponent(
        WaterComponent,
        FAttachmentTransformRules::KeepWorldTransform
    );
    Modifier->RegisterComponent();
    return WaterBody;
}
} // namespace UE::Avenor::Hydrology

using namespace UE::Avenor::Hydrology;

AAvenorHydrologyGenerator::AAvenorHydrologyGenerator()
{
    PrimaryActorTick.bCanEverTick = false;
    SetIsSpatiallyLoaded(false);
}

UAvenorTerrainModifier*
AAvenorHydrologyGenerator::ResolveTerrainModifier() const
{
    return TerrainModifierActor
        ? TerrainModifierActor->FindComponentByClass<
            UAvenorTerrainModifier>()
        : nullptr;
}

UAvenorTerrainRefinementModifier*
AAvenorHydrologyGenerator::ResolveRefinementModifier() const
{
    return RefinementModifierActor
        ? RefinementModifierActor->FindComponentByClass<
            UAvenorTerrainRefinementModifier>()
        : nullptr;
}

void AAvenorHydrologyGenerator::ClearGeneratedHydrology()
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
    for (AWaterBody* WaterBody : ToDelete)
    {
        World->EditorDestroyActor(WaterBody, true);
    }
#endif
}

void AAvenorHydrologyGenerator::RegenerateHydrology()
{
#if WITH_EDITOR
    UAvenorTerrainModifier* Terrain = ResolveTerrainModifier();
    UAvenorTerrainRefinementModifier* Refinement =
        ResolveRefinementModifier();
    UE::MeshPartition::AMeshPartition* MeshPartition =
        Cast<UE::MeshPartition::AMeshPartition>(MeshPartitionActor);
    UWorld* World = GetWorld();
    if (!Terrain || !Refinement || !MeshPartition || !World)
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT(
                "Avenor hydrology requires Base Terrain, Refinement, and "
                "Mesh Partition Actor references."
            )
        );
        return;
    }

    const FBox Bounds = Refinement->GetAnalysisWorldBounds();
    if (!Bounds.IsValid)
    {
        UE_LOG(LogTemp, Error, TEXT("Avenor terrain bounds are invalid."));
        return;
    }

    FScopedSlowTask Progress(
        4.0f,
        FText::FromString(TEXT("Generating Avenor hydrology..."))
    );
    Progress.MakeDialog();

    FGrid Grid;
    Grid.Bounds = Bounds;
    Grid.CellSize = FMath::Max(2500.0, HydrologyCellSize);
    Grid.Columns = FMath::Max(
        3,
        FMath::FloorToInt(Bounds.GetSize().X / Grid.CellSize)
    );
    Grid.Rows = FMath::Max(
        3,
        FMath::FloorToInt(Bounds.GetSize().Y / Grid.CellSize)
    );
    const int32 CellCount = Grid.Columns * Grid.Rows;
    Grid.TerrainHeight.SetNumUninitialized(CellCount);

    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Sampling base terrain"))
    );
    for (int32 Index = 0; Index < CellCount; ++Index)
    {
        Grid.TerrainHeight[Index] =
            Refinement->EvaluateRefinedHeightAtWorldPosition(
                Grid.Position(Index)
            );
    }

    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Solving drainage"))
    );
    FillDepressions(Grid, FMath::Max(0.01, DrainageEpsilon));
    BuildFlow(Grid);

    const TArray<int32> LakeCells = SelectLakeCells(
        Grid,
        LakeCount,
        MinimumLakeFillDepth,
        LakeRadius * 2.5
    );
    const TArray<int32> RiverSources = SelectRiverSources(
        Grid,
        RiverCount,
        MinimumRiverCatchmentCells,
        Grid.CellSize * 8.0
    );

    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Replacing generated water actors"))
    );
    ClearGeneratedHydrology();

    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Creating native water bodies"))
    );
    int32 CreatedLakes = 0;
    int32 CreatedRivers = 0;
    for (int32 LakeIndex = 0;
         LakeIndex < LakeCells.Num();
         ++LakeIndex)
    {
        const int32 LakeCell = LakeCells[LakeIndex];
        const double SurfaceZ = Grid.FilledHeight[LakeCell];

        AWaterBodyLake* Lake = CreateWaterBody<AWaterBodyLake>(
            World,
            MeshPartition,
            FString::Printf(TEXT("Avenor_Lake_%02d"), LakeIndex + 1),
            TEXT("/Script/MeshPartitionWater.LakeModifier"),
            LakeRadius * 0.35
        );
        if (Lake)
        {
            TArray<FVector> LakePoints;
            constexpr int32 PointCount = 24;
            for (int32 PointIndex = 0;
                 PointIndex < PointCount;
                 ++PointIndex)
            {
                const double Angle =
                    2.0 * PI * static_cast<double>(PointIndex) /
                    static_cast<double>(PointCount);
                const FVector2D Shore = FindLakeShore(
                    Grid,
                    LakeCell,
                    Angle,
                    LakeRadius,
                    MinimumLakeFillDepth * 0.25
                );
                LakePoints.Emplace(
                    Shore.X,
                    Shore.Y,
                    SurfaceZ
                );
            }
            UWaterSplineComponent* Spline =
                Lake->GetWaterBodyComponent()->GetWaterSpline();
            Spline->SetSplinePoints(
                LakePoints,
                ESplineCoordinateSpace::World,
                false
            );
            Spline->SetClosedLoop(true, true);
            Lake->PostEditChange();
            ++CreatedLakes;
        }
    }

    for (int32 RiverIndex = 0;
         RiverIndex < RiverSources.Num();
         ++RiverIndex)
    {
        const int32 SourceCell = RiverSources[RiverIndex];
        const TArray<int32> RiverCells =
            TraceRiver(Grid, SourceCell);
        if (RiverCells.Num() < 2)
        {
            continue;
        }

        AWaterBodyRiver* River = CreateWaterBody<AWaterBodyRiver>(
            World,
            MeshPartition,
            FString::Printf(TEXT("Avenor_River_%02d"), RiverIndex + 1),
            TEXT("/Script/MeshPartitionWater.RiverModifier"),
            Grid.CellSize * 0.75
        );
        if (!River)
        {
            continue;
        }

        TArray<FVector> RiverPoints;
        double PreviousWaterZ =
            Grid.FilledHeight[SourceCell] + DrainageEpsilon;
        const int32 Stride = FMath::Max(1, RiverSplineStride);
        for (int32 PathIndex = 0;
             PathIndex < RiverCells.Num();
             PathIndex += Stride)
        {
            const int32 Cell = RiverCells[PathIndex];
            const FVector2D Position = Grid.Position(Cell);
            const double WaterZ = FMath::Min(
                PreviousWaterZ - DrainageEpsilon,
                Grid.FilledHeight[Cell] + DrainageEpsilon
            );
            RiverPoints.Emplace(Position.X, Position.Y, WaterZ);
            PreviousWaterZ = WaterZ;
        }
        const int32 LastCell = RiverCells.Last();
        const FVector2D LastPosition = Grid.Position(LastCell);
        if (RiverPoints.IsEmpty() ||
            !FVector2D(
                RiverPoints.Last().X,
                RiverPoints.Last().Y
            ).Equals(LastPosition, 1.0))
        {
            RiverPoints.Emplace(
                LastPosition.X,
                LastPosition.Y,
                FMath::Min(
                    PreviousWaterZ - DrainageEpsilon,
                    Grid.FilledHeight[LastCell] + DrainageEpsilon
                )
            );
        }

        UWaterSplineComponent* RiverSpline =
            River->GetWaterBodyComponent()->GetWaterSpline();
        RiverSpline->SetSplinePoints(
            RiverPoints,
            ESplineCoordinateSpace::World,
            false
        );
        RiverSpline->SetClosedLoop(false, false);
        for (int32 PointIndex = 0;
             PointIndex < RiverSpline->GetNumberOfSplinePoints();
             ++PointIndex)
        {
            RiverSpline->SetSplinePointType(
                PointIndex,
                ESplinePointType::CurveClamped,
                false
            );
        }
        RiverSpline->UpdateSpline();
        River->PostEditChange();
        ++CreatedRivers;
    }

    if (UE::MeshPartition::UMeshPartitionEditorComponent*
            EditorComponent =
                Cast<UE::MeshPartition::UMeshPartitionEditorComponent>(
                    MeshPartition->GetMeshPartitionComponent()
                ))
    {
        EditorComponent->OnModifierAssigned();
    }

    bool bHasWaterZone = false;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (It->GetClass()->GetName() == TEXT("WaterZone"))
        {
            bHasWaterZone = true;
            break;
        }
    }
    if (!bHasWaterZone)
    {
        UE_LOG(
            LogTemp,
            Warning,
            TEXT(
                "Water Body actors were generated, but the level has no "
                "Water Zone. Add a Water Zone covering the terrain to render "
                "their water surfaces."
            )
        );
    }

    UE_LOG(
        LogTemp,
        Display,
        TEXT("Avenor hydrology created %d lakes and %d rivers."),
        CreatedLakes,
        CreatedRivers
    );
#endif
}
