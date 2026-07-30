#pragma once

#include "CoreMinimal.h"
#include "MeshPartitionModifierComponent.h"
#include "AvenorTerrainRefinementModifier.generated.h"

class UAvenorTerrainModifier;
struct FAvenorTerrainAnalysisData;

struct FAvenorRiverReach
{
    TArray<FVector> Points;
    double DischargeCells = 0.0;
};

struct FAvenorLakeBasin
{
    TArray<FVector> Shoreline;
    double SurfaceHeight = 0.0;
    int32 CellCount = 0;
};

/**
 * Stacked Mesh Terrain modifier for bounded, deterministic macro erosion.
 *
 * The base modifier owns the initial geography. This component samples it to
 * a coarse grid, solves drainage, performs limited thermal/stream erosion and
 * applies only the resulting height delta to the mesh produced below it.
 */
UCLASS(
    PrioritizeCategories = (
        "Modifier",
        "Avenor",
        "Analysis Grid",
        "Erosion",
        "Drainage"
    ),
    meta = (BlueprintSpawnableComponent)
)
class AVENOREDITOR_API UAvenorTerrainRefinementModifier
    : public UE::MeshPartition::UModifierComponent
{
    GENERATED_BODY()

public:
    UAvenorTerrainRefinementModifier();

    UFUNCTION(BlueprintPure, Category = "Avenor|Terrain Analysis")
    double EvaluateRefinedHeightAtWorldPosition(
        const FVector2D& WorldPosition
    ) const;

    UFUNCTION(BlueprintPure, Category = "Avenor|Terrain Analysis")
    double EvaluateFlowAccumulationAtWorldPosition(
        const FVector2D& WorldPosition
    ) const;

    UFUNCTION(BlueprintPure, Category = "Avenor|Terrain Analysis")
    double EvaluateLakeFillDepthAtWorldPosition(
        const FVector2D& WorldPosition
    ) const;

    UFUNCTION(BlueprintPure, Category = "Avenor|Terrain Analysis")
    FBox GetAnalysisWorldBounds() const;

    // Returns vector water features extracted from the exact cached drainage
    // graph used to carve this modifier's terrain.
    bool GetHydrologyFeatures(
        int32 MaximumRiverReaches,
        int32 MaximumLakes,
        double RiverSurfaceInset,
        TArray<FAvenorRiverReach>& OutRivers,
        TArray<FAvenorLakeBasin>& OutLakes
    ) const;

    virtual TArray<FBox> ComputeBounds() const override;
    virtual TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
        CreateBackgroundOp(
            UE::MeshPartition::EBuildType InBuildType
        ) const override;
    virtual FGuid GetCodeVersionKey() const override;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(
        FPropertyChangedEvent& PropertyChangedEvent
    ) override;
#endif

private:
    UAvenorTerrainModifier* ResolveBaseTerrain() const;
    TSharedPtr<const FAvenorTerrainAnalysisData>
        GetOrBuildAnalysis() const;
    void InvalidateAnalysis();

    UPROPERTY(EditInstanceOnly, Category = "Avenor|References")
    TObjectPtr<AActor> BaseTerrainModifierActor;

    // 100 m is 200,000 cells for a 20 x 100 km world.
    UPROPERTY(EditAnywhere, Category = "Analysis Grid",
        meta = (ClampMin = "5000.0", ClampMax = "100000.0"))
    double AnalysisCellSize = 10000.0;

    // Safety limit, independent of physical world dimensions.
    UPROPERTY(EditAnywhere, Category = "Analysis Grid",
        meta = (ClampMin = "1000", ClampMax = "2000000"))
    int32 MaximumAnalysisCells = 500000;

    UPROPERTY(EditAnywhere, Category = "Drainage",
        meta = (ClampMin = "0.01", ClampMax = "100.0"))
    double DrainageEpsilon = 1.0;

    UPROPERTY(EditAnywhere, Category = "Erosion")
    bool bEnableThermalErosion = false;

    UPROPERTY(EditAnywhere, Category = "Erosion",
        meta = (ClampMin = "0", ClampMax = "12",
            EditCondition = "bEnableThermalErosion"))
    int32 ThermalErosionIterations = 3;

    UPROPERTY(EditAnywhere, Category = "Erosion",
        meta = (ClampMin = "0.0",
            EditCondition = "bEnableThermalErosion"))
    double TalusHeight = 9000.0;

    UPROPERTY(EditAnywhere, Category = "Erosion",
        meta = (ClampMin = "0.0", ClampMax = "1.0",
            EditCondition = "bEnableThermalErosion"))
    double ThermalErosionStrength = 0.18;

    UPROPERTY(EditAnywhere, Category = "Drainage")
    bool bEnableStreamIncision = true;

    UPROPERTY(EditAnywhere, Category = "Drainage",
        meta = (ClampMin = "1.0",
            EditCondition = "bEnableStreamIncision"))
    double StreamStartAreaCells = 20.0;

    UPROPERTY(EditAnywhere, Category = "Drainage",
        meta = (ClampMin = "0.0",
            EditCondition = "bEnableStreamIncision"))
    double MaximumStreamIncision = 6000.0;

    UPROPERTY(EditAnywhere, Category = "Drainage",
        meta = (ClampMin = "0.0"))
    double MinimumLakeFillDepth = 1000.0;

    UPROPERTY(EditAnywhere, Category = "Drainage",
        meta = (ClampMin = "0.0"))
    double LakeBedDepth = 2500.0;

    UPROPERTY(EditAnywhere, Category = "Drainage")
    bool bEnableFloodplains = true;

    UPROPERTY(EditAnywhere, Category = "Drainage",
        meta = (ClampMin = "1.0",
            EditCondition = "bEnableFloodplains"))
    double FloodplainStartAreaCells = 80.0;

    UPROPERTY(EditAnywhere, Category = "Drainage",
        meta = (ClampMin = "0.0", ClampMax = "1.0",
            EditCondition = "bEnableFloodplains"))
    double FloodplainSmoothing = 0.35;

    mutable FCriticalSection AnalysisMutex;
    mutable TSharedPtr<const FAvenorTerrainAnalysisData> CachedAnalysis;
};
