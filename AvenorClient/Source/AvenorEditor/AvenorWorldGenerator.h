#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MeshPartitionModifierComponent.h"
#include "AvenorWorldGenerator.generated.h"

class AAvenorWorldGenerator;
class ASpineGenerator;
struct FAvenorGeneratedWorld;

/**
 * Thin Mesh Partition bridge. It applies the immutable heightfield produced
 * by AAvenorWorldGenerator and performs no independent terrain generation.
 */
UCLASS(NotBlueprintable, meta = (BlueprintSpawnableComponent))
class AVENOREDITOR_API UAvenorGeneratedTerrainModifier final
    : public UE::MeshPartition::UModifierComponent
{
    GENERATED_BODY()

public:
    virtual TArray<FBox> ComputeBounds() const override;
    virtual TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
        CreateBackgroundOp(
            UE::MeshPartition::EBuildType InBuildType
        ) const override;
    virtual FGuid GetCodeVersionKey() const override;
};

/**
 * Complete deterministic terrain and water generator.
 *
 * One generated dataset owns the final terrain heightfield, drainage graph,
 * river reaches and lake shorelines. The attached Mesh Partition modifier
 * applies those heights, while this same actor creates the native UE Water
 * splines. No later system attempts to rediscover the drainage network.
 */
UCLASS(
    PrioritizeCategories = (
        "Avenor World",
        "Coverage",
        "Landforms",
        "Plains",
        "Rolling Hills",
        "Mountains",
        "Mesas",
        "Valleys",
        "Canyons",
        "Hydrology",
        "Rivers",
        "Lakes",
        "Oceans",
        "Waterfalls",
        "Spine Corridor",
        "Performance"
    )
)
class AVENOREDITOR_API AAvenorWorldGenerator final : public AActor
{
    GENERATED_BODY()

public:
    AAvenorWorldGenerator();

    UFUNCTION(CallInEditor, Category = "Avenor World")
    void RegenerateWorld();

    UFUNCTION(CallInEditor, Category = "Avenor World")
    void ClearGeneratedWater();

    UFUNCTION(BlueprintPure, Category = "Avenor World")
    double GetGeneratedHeightAtWorldPosition(
        const FVector2D& WorldPosition
    ) const;

    FBox GetGenerationBounds() const;
    TSharedPtr<const FAvenorGeneratedWorld> GetOrGenerateWorld() const;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(
        FPropertyChangedEvent& PropertyChangedEvent
    ) override;
#endif

private:
    void InvalidateGeneratedWorld();
    void CreateWaterBodies(
        const TSharedPtr<const FAvenorGeneratedWorld>& WorldData
    );

    UPROPERTY(VisibleAnywhere, Category = "Avenor World")
    TObjectPtr<UAvenorGeneratedTerrainModifier> TerrainModifier;

    UPROPERTY(EditInstanceOnly, Category = "Avenor World")
    TObjectPtr<AActor> MeshPartitionActor;

    UPROPERTY(EditInstanceOnly, Category = "Avenor World")
    TObjectPtr<ASpineGenerator> Spine;

    UPROPERTY(EditAnywhere, Category = "Coverage",
        meta = (ClampMin = "100000.0", AllowPreserveRatio = true,
            Units = "cm"))
    FVector3d WorldCoverage =
        FVector3d(1200000.0, 3000000.0, 2000000.0);

    UPROPERTY(EditAnywhere, Category = "Avenor World")
    int32 WorldSeed = 1847;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGeneratePlains = true;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateRollingHills = true;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateMountains = true;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateMesas = false;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateCanyons = false;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateValleys = true;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateRivers = true;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateLakes = true;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateOceans = false;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateWaterfalls = true;

    UPROPERTY(EditAnywhere, Category = "Plains",
        meta = (ClampMin = "0.0", ClampMax = "1.0",
            EditCondition = "bGeneratePlains"))
    double PlainsFrequency = 0.42;

    UPROPERTY(EditAnywhere, Category = "Plains",
        meta = (ClampMin = "0.0", EditCondition = "bGeneratePlains"))
    double PlainsRelief = 2500.0;

    UPROPERTY(EditAnywhere, Category = "Rolling Hills",
        meta = (ClampMin = "0.0", ClampMax = "1.0",
            EditCondition = "bGenerateRollingHills"))
    double RollingHillsFrequency = 0.42;

    UPROPERTY(EditAnywhere, Category = "Rolling Hills",
        meta = (ClampMin = "10000.0",
            EditCondition = "bGenerateRollingHills"))
    double RollingHillsScale = 150000.0;

    UPROPERTY(EditAnywhere, Category = "Rolling Hills",
        meta = (ClampMin = "0.0",
            EditCondition = "bGenerateRollingHills"))
    double RollingHillsRelief = 18000.0;

    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "0", ClampMax = "64",
            EditCondition = "bGenerateMountains"))
    int32 MountainRangeCount = 3;

    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "10000.0",
            EditCondition = "bGenerateMountains", Units = "cm"))
    double MountainRangeLength = 1000000.0;

    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "10000.0",
            EditCondition = "bGenerateMountains", Units = "cm"))
    double MountainRangeWidth = 350000.0;

    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "10000.0",
            EditCondition = "bGenerateMountains", Units = "cm"))
    double MountainPeakSpacing = 220000.0;

    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "0.0", EditCondition = "bGenerateMountains",
            Units = "cm"))
    double MountainRelief = 80000.0;

    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "0.0", EditCondition = "bGenerateMountains",
            Units = "cm"))
    double MountainMinimumSpineDistance = 350000.0;

    UPROPERTY(EditAnywhere, Category = "Mesas",
        meta = (ClampMin = "0.0", ClampMax = "1.0",
            EditCondition = "bGenerateMesas"))
    double MesaFrequency = 0.08;

    UPROPERTY(EditAnywhere, Category = "Mesas",
        meta = (ClampMin = "10000.0", EditCondition = "bGenerateMesas"))
    double MesaScale = 350000.0;

    UPROPERTY(EditAnywhere, Category = "Mesas",
        meta = (ClampMin = "0.0", EditCondition = "bGenerateMesas"))
    double MesaRelief = 28000.0;

    UPROPERTY(EditAnywhere, Category = "Mesas",
        meta = (ClampMin = "1", ClampMax = "12",
            EditCondition = "bGenerateMesas"))
    int32 MesaTerraceCount = 4;

    // Valleys are cut along the calculated drainage network; this never
    // creates decorative trenches which disagree with the river splines.
    UPROPERTY(EditAnywhere, Category = "Valleys",
        meta = (ClampMin = "1.0", EditCondition = "bGenerateValleys"))
    double ValleyStartCatchmentCells = 35.0;

    UPROPERTY(EditAnywhere, Category = "Valleys",
        meta = (ClampMin = "0.0", EditCondition = "bGenerateValleys"))
    double ValleyMaximumDepth = 4500.0;

    UPROPERTY(EditAnywhere, Category = "Valleys",
        meta = (ClampMin = "1", ClampMax = "12",
            EditCondition = "bGenerateValleys"))
    int32 ValleyMaximumHalfWidthCells = 5;

    UPROPERTY(EditAnywhere, Category = "Canyons",
        meta = (ClampMin = "1.0", EditCondition = "bGenerateCanyons"))
    double CanyonStartCatchmentCells = 600.0;

    UPROPERTY(EditAnywhere, Category = "Canyons",
        meta = (ClampMin = "0.0", EditCondition = "bGenerateCanyons"))
    double CanyonMaximumDepth = 18000.0;

    UPROPERTY(EditAnywhere, Category = "Hydrology",
        meta = (ClampMin = "5000.0", ClampMax = "100000.0"))
    double GenerationCellSize = 10000.0;

    UPROPERTY(EditAnywhere, Category = "Performance",
        meta = (ClampMin = "1000", ClampMax = "2000000"))
    int32 MaximumGenerationCells = 500000;

    UPROPERTY(EditAnywhere, Category = "Hydrology",
        meta = (ClampMin = "0.01", ClampMax = "100.0"))
    double DrainageEpsilon = 1.0;

    UPROPERTY(EditAnywhere, Category = "Hydrology",
        meta = (ClampMin = "1", ClampMax = "64"))
    int32 ErosionIterations = 10;

    UPROPERTY(EditAnywhere, Category = "Rivers",
        meta = (ClampMin = "1.0", EditCondition = "bGenerateRivers"))
    double StreamStartCatchmentCells = 24.0;

    UPROPERTY(EditAnywhere, Category = "Rivers",
        meta = (ClampMin = "0.0", EditCondition = "bGenerateRivers",
            Units = "cm"))
    double MaximumRiverDepth = 1200.0;

    UPROPERTY(EditAnywhere, Category = "Rivers",
        meta = (ClampMin = "100.0", EditCondition = "bGenerateRivers",
            Units = "cm"))
    double HeadwaterWidth = 600.0;

    UPROPERTY(EditAnywhere, Category = "Rivers",
        meta = (ClampMin = "100.0", EditCondition = "bGenerateRivers",
            Units = "cm"))
    double MainRiverWidth = 10000.0;

    UPROPERTY(EditAnywhere, Category = "Rivers",
        meta = (ClampMin = "0.0", ClampMax = "1.0",
            EditCondition = "bGenerateRivers"))
    double LowlandMeanderStrength = 0.75;

    UPROPERTY(EditAnywhere, Category = "Rivers",
        meta = (ClampMin = "1", ClampMax = "512",
            EditCondition = "bGenerateRivers"))
    int32 MaximumRiverReaches = 45;

    UPROPERTY(EditAnywhere, Category = "Lakes",
        meta = (ClampMin = "1.0", EditCondition = "bGenerateLakes"))
    double MinimumLakeCatchmentCells = 60.0;

    UPROPERTY(EditAnywhere, Category = "Lakes",
        meta = (ClampMin = "1.0", EditCondition = "bGenerateLakes"))
    double MinimumLakeDepth = 200.0;

    UPROPERTY(EditAnywhere, Category = "Lakes",
        meta = (ClampMin = "0.1", EditCondition = "bGenerateLakes"))
    double CatchmentRunoffDepth = 50.0;

    UPROPERTY(EditAnywhere, Category = "Lakes",
        meta = (ClampMin = "0.01", EditCondition = "bGenerateLakes"))
    double MaximumLakeAreaSquareKilometres = 25.0;

    UPROPERTY(EditAnywhere, Category = "Lakes",
        meta = (ClampMin = "0", ClampMax = "64",
            EditCondition = "bGenerateLakes"))
    int32 MaximumLakeCount = 8;

    UPROPERTY(EditAnywhere, Category = "Oceans",
        meta = (ClampMin = "10000.0", EditCondition = "bGenerateOceans"))
    double CoastTransitionWidth = 300000.0;

    UPROPERTY(EditAnywhere, Category = "Oceans",
        meta = (ClampMin = "0.0", EditCondition = "bGenerateOceans"))
    double OceanDepth = 30000.0;

    UPROPERTY(EditAnywhere, Category = "Oceans",
        meta = (EditCondition = "bGenerateOceans"))
    double SeaLevel = 0.0;

    UPROPERTY(EditAnywhere, Category = "Waterfalls",
        meta = (ClampMin = "0.0", EditCondition = "bGenerateWaterfalls"))
    double MinimumWaterfallDrop = 1200.0;

    UPROPERTY(EditAnywhere, Category = "Spine Corridor",
        meta = (ClampMin = "0.0"))
    double GentleCorridorHalfWidth = 100000.0;

    UPROPERTY(EditAnywhere, Category = "Spine Corridor",
        meta = (ClampMin = "0.0"))
    double FullRoughnessDistance = 300000.0;

    UPROPERTY(EditAnywhere, Category = "Spine Corridor",
        meta = (ClampMin = "0.0", ClampMax = "1.0"))
    double CorridorRoughnessFraction = 0.18;

    UPROPERTY(EditAnywhere, Category = "Coverage",
        meta = (ClampMin = "0.0"))
    double WorldEdgeBlendWidth = 50000.0;

    UPROPERTY(EditAnywhere, Category = "Rivers",
        meta = (ClampMin = "0.0"))
    double RiverFalloffWidth = 15000.0;

    UPROPERTY(EditAnywhere, Category = "Lakes",
        meta = (ClampMin = "0.0"))
    double LakeFalloffWidth = 30000.0;

    mutable FCriticalSection GenerationMutex;
    mutable TSharedPtr<const FAvenorGeneratedWorld> CachedWorld;
};
