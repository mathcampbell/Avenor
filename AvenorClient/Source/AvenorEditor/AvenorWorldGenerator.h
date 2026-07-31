#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MeshPartitionModifierComponent.h"
#include "AvenorWorldGenerator.generated.h"

struct FAvenorGeneratedWorld;

UENUM(BlueprintType)
enum class EAvenorLongWorldAxis : uint8
{
    X UMETA(DisplayName = "X Axis"),
    Y UMETA(DisplayName = "Y Axis")
};

/**
 * Applies the immutable terrain dataset produced by AAvenorWorldGenerator.
 * It contains no procedural rules of its own.
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
 * Deterministic, spine-independent terrain and hydrology generator.
 *
 * One dataset owns the eroded landform, river network, lake basins and ocean
 * boundary. The Mesh Partition modifier and native UE Water splines consume
 * that same dataset, so terrain carving cannot drift away from the water.
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
        "Erosion",
        "Hydrology",
        "Rivers and Valleys",
        "Lakes",
        "Oceans",
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

    UPROPERTY(EditAnywhere, Category = "Avenor World")
    int32 WorldSeed = 1847;

    // Defaults to 100 km long, 20 km wide and 4 km of vertical build bounds.
    UPROPERTY(EditAnywhere, Category = "Coverage",
        meta = (ClampMin = "100000.0", AllowPreserveRatio = true,
            Units = "cm"))
    FVector3d WorldCoverage =
        FVector3d(10000000.0, 2000000.0, 400000.0);

    UPROPERTY(EditAnywhere, Category = "Coverage")
    EAvenorLongWorldAxis LongWorldAxis = EAvenorLongWorldAxis::X;

    UPROPERTY(EditAnywhere, Category = "Coverage",
        meta = (ClampMin = "0.0", Units = "cm"))
    double WorldEdgeBlendWidth = 100000.0;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGeneratePlains = true;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateRollingHills = true;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateMountains = true;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateMesas = false;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateValleys = true;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateCanyons = true;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateRivers = true;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateLakes = true;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bGenerateOceans = false;

    UPROPERTY(EditAnywhere, Category = "Plains",
        meta = (ClampMin = "0.0", ClampMax = "1.0",
            EditCondition = "bGeneratePlains"))
    double PlainsCoverage = 0.52;

    UPROPERTY(EditAnywhere, Category = "Plains",
        meta = (ClampMin = "0.0", EditCondition = "bGeneratePlains",
            Units = "cm"))
    double PlainsRelief = 1800.0;

    UPROPERTY(EditAnywhere, Category = "Rolling Hills",
        meta = (ClampMin = "0.0", ClampMax = "1.0",
            EditCondition = "bGenerateRollingHills"))
    double RollingHillsCoverage = 0.38;

    UPROPERTY(EditAnywhere, Category = "Rolling Hills",
        meta = (ClampMin = "10000.0",
            EditCondition = "bGenerateRollingHills", Units = "cm"))
    double RollingHillsScale = 180000.0;

    UPROPERTY(EditAnywhere, Category = "Rolling Hills",
        meta = (ClampMin = "0.0",
            EditCondition = "bGenerateRollingHills", Units = "cm"))
    double RollingHillsRelief = 12000.0;

    // Number of mountain ranges expected per 100 km of world length.
    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "0.0", ClampMax = "32.0",
            EditCondition = "bGenerateMountains"))
    double MountainRangesPer100Km = 3.0;

    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "10000.0",
            EditCondition = "bGenerateMountains", Units = "cm"))
    double MountainRangeLength = 1600000.0;

    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "10000.0",
            EditCondition = "bGenerateMountains", Units = "cm"))
    double MountainRangeWidth = 450000.0;

    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "10000.0",
            EditCondition = "bGenerateMountains", Units = "cm"))
    double MountainPeakSpacing = 240000.0;

    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "0.0", EditCondition = "bGenerateMountains",
            Units = "cm"))
    double MountainRelief = 95000.0;

    // Mountain centres are kept outside this band around the selected axis.
    // This is not a spine corridor and does not flatten other terrain.
    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "0.0", EditCondition = "bGenerateMountains",
            Units = "cm"))
    double CentralMountainExclusionHalfWidth = 300000.0;

    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "0.0", ClampMax = "1.0",
            EditCondition = "bGenerateMountains"))
    double MountainEdgeBias = 0.72;

    UPROPERTY(EditAnywhere, Category = "Mesas",
        meta = (ClampMin = "0.0", ClampMax = "1.0",
            EditCondition = "bGenerateMesas"))
    double MesaCoverage = 0.08;

    UPROPERTY(EditAnywhere, Category = "Mesas",
        meta = (ClampMin = "10000.0", EditCondition = "bGenerateMesas",
            Units = "cm"))
    double MesaScale = 420000.0;

    UPROPERTY(EditAnywhere, Category = "Mesas",
        meta = (ClampMin = "0.0", EditCondition = "bGenerateMesas",
            Units = "cm"))
    double MesaRelief = 32000.0;

    UPROPERTY(EditAnywhere, Category = "Mesas",
        meta = (ClampMin = "1", ClampMax = "12",
            EditCondition = "bGenerateMesas"))
    int32 MesaTerraceCount = 4;

    UPROPERTY(EditAnywhere, Category = "Erosion",
        meta = (ClampMin = "0", ClampMax = "32"))
    int32 ThermalErosionIterations = 4;

    UPROPERTY(EditAnywhere, Category = "Erosion",
        meta = (ClampMin = "0.0", ClampMax = "1.0"))
    double ThermalErosionStrength = 0.16;

    UPROPERTY(EditAnywhere, Category = "Erosion",
        meta = (ClampMin = "1.0", ClampMax = "80.0"))
    double TalusAngleDegrees = 34.0;

    UPROPERTY(EditAnywhere, Category = "Erosion",
        meta = (ClampMin = "0", ClampMax = "32"))
    int32 HydraulicErosionIterations = 7;

    UPROPERTY(EditAnywhere, Category = "Erosion",
        meta = (ClampMin = "0.0", ClampMax = "5000.0",
            Units = "cm"))
    double HydraulicIncisionPerIteration = 120.0;

    UPROPERTY(EditAnywhere, Category = "Hydrology",
        meta = (ClampMin = "5000.0", ClampMax = "100000.0",
            Units = "cm"))
    double GenerationCellSize = 10000.0;

    UPROPERTY(EditAnywhere, Category = "Hydrology",
        meta = (ClampMin = "0.01", ClampMax = "100.0",
            Units = "cm"))
    double DrainageEpsilon = 1.0;

    UPROPERTY(EditAnywhere, Category = "Rivers and Valleys",
        meta = (ClampMin = "0.01", EditCondition = "bGenerateRivers"))
    double StreamStartAreaSquareKilometres = 0.35;

    UPROPERTY(EditAnywhere, Category = "Rivers and Valleys",
        meta = (ClampMin = "0.01", EditCondition = "bGenerateRivers"))
    double MainRiverAreaSquareKilometres = 8.0;

    UPROPERTY(EditAnywhere, Category = "Rivers and Valleys",
        meta = (ClampMin = "100.0", EditCondition = "bGenerateRivers",
            Units = "cm"))
    double HeadwaterWidth = 500.0;

    UPROPERTY(EditAnywhere, Category = "Rivers and Valleys",
        meta = (ClampMin = "100.0", EditCondition = "bGenerateRivers",
            Units = "cm"))
    double MainRiverWidth = 9000.0;

    UPROPERTY(EditAnywhere, Category = "Rivers and Valleys",
        meta = (ClampMin = "0.0", EditCondition = "bGenerateRivers",
            Units = "cm"))
    double MaximumRiverDepth = 1000.0;

    UPROPERTY(EditAnywhere, Category = "Rivers and Valleys",
        meta = (ClampMin = "100.0", EditCondition = "bGenerateRivers",
            Units = "cm"))
    double HeadwaterValleyHalfWidth = 12000.0;

    UPROPERTY(EditAnywhere, Category = "Rivers and Valleys",
        meta = (ClampMin = "100.0", EditCondition = "bGenerateRivers",
            Units = "cm"))
    double MainValleyHalfWidth = 90000.0;

    UPROPERTY(EditAnywhere, Category = "Rivers and Valleys",
        meta = (ClampMin = "0.0", EditCondition = "bGenerateRivers",
            Units = "cm"))
    double MaximumValleyDepth = 14000.0;

    UPROPERTY(EditAnywhere, Category = "Rivers and Valleys",
        meta = (ClampMin = "0.0", ClampMax = "1.5",
            EditCondition = "bGenerateRivers"))
    double LowlandMeanderStrength = 0.72;

    UPROPERTY(EditAnywhere, Category = "Rivers and Valleys",
        meta = (ClampMin = "1", ClampMax = "512",
            EditCondition = "bGenerateRivers"))
    int32 MaximumRiverReaches = 72;

    UPROPERTY(EditAnywhere, Category = "Rivers and Valleys",
        meta = (ClampMin = "0.01", EditCondition = "bGenerateCanyons"))
    double CanyonStartAreaSquareKilometres = 30.0;

    UPROPERTY(EditAnywhere, Category = "Lakes",
        meta = (ClampMin = "0.01", EditCondition = "bGenerateLakes"))
    double MinimumLakeCatchmentSquareKilometres = 0.2;

    UPROPERTY(EditAnywhere, Category = "Lakes",
        meta = (ClampMin = "1.0", EditCondition = "bGenerateLakes",
            Units = "cm"))
    double MinimumLakeDepth = 250.0;

    UPROPERTY(EditAnywhere, Category = "Lakes",
        meta = (ClampMin = "0.01", EditCondition = "bGenerateLakes"))
    double MaximumLakeAreaSquareKilometres = 35.0;

    UPROPERTY(EditAnywhere, Category = "Lakes",
        meta = (ClampMin = "0", ClampMax = "64",
            EditCondition = "bGenerateLakes"))
    int32 MaximumLakeCount = 10;

    UPROPERTY(EditAnywhere, Category = "Lakes",
        meta = (ClampMin = "100.0", EditCondition = "bGenerateLakes",
            Units = "cm"))
    double LakeShoreFalloffWidth = 24000.0;

    UPROPERTY(EditAnywhere, Category = "Oceans",
        meta = (EditCondition = "bGenerateOceans"))
    double SeaLevel = 0.0;

    UPROPERTY(EditAnywhere, Category = "Oceans",
        meta = (ClampMin = "0.0", EditCondition = "bGenerateOceans",
            Units = "cm"))
    double OceanDepth = 30000.0;

    UPROPERTY(EditAnywhere, Category = "Oceans",
        meta = (ClampMin = "10000.0", EditCondition = "bGenerateOceans",
            Units = "cm"))
    double CoastTransitionWidth = 300000.0;

    UPROPERTY(EditAnywhere, Category = "Oceans",
        meta = (EditCondition = "bGenerateOceans"))
    bool bOceanAlongWidthEdges = true;

    UPROPERTY(EditAnywhere, Category = "Oceans",
        meta = (EditCondition = "bGenerateOceans"))
    bool bOceanAtLengthEnds = false;

    UPROPERTY(EditAnywhere, Category = "Performance",
        meta = (ClampMin = "1000", ClampMax = "4000000"))
    int32 MaximumGenerationCells = 1500000;

    mutable FCriticalSection GenerationMutex;
    mutable TSharedPtr<const FAvenorGeneratedWorld> CachedWorld;
};
