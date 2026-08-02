#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MeshPartitionModifierComponent.h"

#include "AvenorStripTerrainGenerator.generated.h"

struct FAvenorStripData;

UENUM(BlueprintType)
enum class EAvenorStripLongAxis : uint8
{
    X UMETA(DisplayName = "X (east-west)"),
    Y UMETA(DisplayName = "Y (north-south)")
};

/**
 * Mesh Partition modifier used by AAvenorStripTerrainGenerator.
 * The expensive terrain and drainage analysis is cached by the owning actor;
 * this component applies the resulting height and material-weight channels to
 * Mesh Partition vertices during its background build.
 */
UCLASS(ClassGroup = (Avenor), meta = (BlueprintSpawnableComponent))
class AVENOREDITOR_API UAvenorStripTerrainModifier
    : public UE::MeshPartition::UModifierComponent
{
    GENERATED_BODY()

public:
    virtual TArray<FBox> ComputeBounds() const override;

    virtual TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
        CreateBackgroundOp(UE::MeshPartition::EBuildType BuildType) const override;

    virtual FGuid GetCodeVersionKey() const override;
};

/**
 * Editor-only procedural generator for Avenor's continuous strip world.
 *
 * All distances are Unreal centimetres. Drainage-area settings are square
 * kilometres because the analysis grid converts cell area to km2.
 */
UCLASS(BlueprintType)
class AVENOREDITOR_API AAvenorStripTerrainGenerator : public AActor
{
    GENERATED_BODY()

public:
    AAvenorStripTerrainGenerator();

    /**
     * Stage 1: calculate landforms and hydrology, then register the single
     * Mesh Partition modifier. This deliberately creates no Water actors.
     * Wait for the Mesh Partition build to finish before running stage 2.
     */
    UFUNCTION(CallInEditor, Category = "Avenor|Generation",
        meta = (DisplayName = "1. Generate Terrain (No Water)"))
    void GenerateTerrain();

    /**
     * Stage 2: create Water actors from the exact cached river/lake plan used
     * to carve the terrain. This refuses to run until stage 1 has succeeded.
     */
    UFUNCTION(CallInEditor, Category = "Avenor|Generation",
        meta = (DisplayName = "2. Generate Water (After Rebuild)"))
    void GenerateWater();

    UFUNCTION(CallInEditor, Category = "Avenor|Generation")
    void ClearGeneratedWater();

    FBox GetGenerationBounds() const;
    TSharedPtr<const FAvenorStripData> GetOrCreateData() const;

    // ------------------------------------------------------------------
    // Target and world layout
    // ------------------------------------------------------------------

    UPROPERTY(EditAnywhere, Category = "Avenor|Target")
    TObjectPtr<AActor> MeshPartitionActor = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Avenor|Target")
    TObjectPtr<UAvenorStripTerrainModifier> TerrainModifier = nullptr;

    UPROPERTY(EditAnywhere, Category = "Avenor|World", meta = (ClampMin = "10000.0"))
    FVector2D WorldSize = FVector2D(10000000.0, 2000000.0);

    UPROPERTY(EditAnywhere, Category = "Avenor|World")
    EAvenorStripLongAxis LongAxis = EAvenorStripLongAxis::X;

    UPROPERTY(EditAnywhere, Category = "Avenor|World")
    int32 Seed = 1337;

    UPROPERTY(EditAnywhere, Category = "Avenor|World", meta = (ClampMin = "2500.0"))
    double AnalysisCellSize = 10000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|World", meta = (ClampMin = "10000", ClampMax = "2000000"))
    int32 MaximumAnalysisCells = 500000;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Generation")
    FString LastBuildStamp;

    UPROPERTY(VisibleAnywhere, Transient, Category = "Avenor|Generation")
    bool bTerrainPlanReadyForWater = false;

    // ------------------------------------------------------------------
    // Central spine and blended landform zones
    // ------------------------------------------------------------------

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Spine", meta = (ClampMin = "0.0"))
    double SpineValleyDepth = 15000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Spine", meta = (ClampMin = "0.01", ClampMax = "1.0"))
    double SpineWidthFraction = 0.22;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Zones", meta = (ClampMin = "10000.0"))
    double ZoneLength = 2500000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Zones", meta = (ClampMin = "0.0"))
    double MountainZoneWeight = 1.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Zones", meta = (ClampMin = "0.0"))
    double HillZoneWeight = 1.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Zones", meta = (ClampMin = "0.0"))
    double DesertZoneWeight = 0.35;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Zones", meta = (ClampMin = "0.0"))
    double PlainsZoneWeight = 1.25;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Mountains")
    bool bGenerateMountains = true;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Mountains", meta = (EditCondition = "bGenerateMountains", ClampMin = "0.0"))
    double MountainRelief = 220000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Mountains", meta = (EditCondition = "bGenerateMountains", ClampMin = "1000.0"))
    double MountainFeatureScale = 650000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Hills")
    bool bGenerateHills = true;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Hills", meta = (EditCondition = "bGenerateHills", ClampMin = "0.0"))
    double HillsRelief = 65000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Hills", meta = (EditCondition = "bGenerateHills", ClampMin = "1000.0"))
    double HillsScale = 220000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Mesas and Canyons")
    bool bGenerateMesasAndCanyons = false;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Mesas and Canyons", meta = (EditCondition = "bGenerateMesasAndCanyons", ClampMin = "0.0"))
    double MesaRelief = 120000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Mesas and Canyons", meta = (EditCondition = "bGenerateMesasAndCanyons", ClampMin = "1000.0"))
    double MesaScale = 400000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Mesas and Canyons", meta = (EditCondition = "bGenerateMesasAndCanyons", ClampMin = "1", ClampMax = "32"))
    int32 MesaTerraces = 7;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Mesas and Canyons", meta = (EditCondition = "bGenerateMesasAndCanyons", ClampMin = "0.0"))
    double CanyonStartArea = 60.0;

    // ------------------------------------------------------------------
    // Erosion and drainage analysis
    // ------------------------------------------------------------------

    UPROPERTY(EditAnywhere, Category = "Avenor|Erosion", meta = (ClampMin = "0", ClampMax = "100"))
    int32 ThermalErosionIterations = 8;

    UPROPERTY(EditAnywhere, Category = "Avenor|Erosion", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    double ThermalErosionStrength = 0.45;

    UPROPERTY(EditAnywhere, Category = "Avenor|Erosion", meta = (ClampMin = "0.0", ClampMax = "89.0"))
    double TalusAngleDegrees = 38.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Erosion", meta = (ClampMin = "0", ClampMax = "100"))
    int32 StreamPowerIterations = 5;

    UPROPERTY(EditAnywhere, Category = "Avenor|Erosion", meta = (ClampMin = "0.0"))
    double StreamPowerStrength = 1.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Erosion", meta = (ClampMin = "0.0001"))
    double DrainageEpsilon = 1.0;

    // ------------------------------------------------------------------
    // Rivers
    // ------------------------------------------------------------------

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers")
    bool bGenerateRivers = true;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "0.01"))
    double MountainStreamStartArea = 1.5;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "0.01"))
    double LowlandStreamStartArea = 8.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "0.01"))
    double MainRiverArea = 80.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "0.0"))
    double MinimumRiverSystemLength = 150000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "100.0"))
    double HeadwaterWidth = 800.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "100.0"))
    double MainRiverWidth = 12000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "1.0"))
    double MaximumRiverDepth = 1800.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "100.0"))
    double HeadwaterValleyHalfWidth = 10000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "100.0"))
    double MainValleyHalfWidth = 80000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "0.0"))
    double MaximumValleyDepth = 14000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "0.0", ClampMax = "3.0"))
    double LowlandMeanderStrength = 0.75;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "0", ClampMax = "4096"))
    int32 MaximumRiverReaches = 256;

    // ------------------------------------------------------------------
    // Lakes
    // ------------------------------------------------------------------

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Lakes")
    bool bGenerateLakes = true;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Lakes", meta = (EditCondition = "bGenerateLakes", ClampMin = "0.0"))
    double MinimumLakeDepth = 800.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Lakes", meta = (EditCondition = "bGenerateLakes", ClampMin = "1.0"))
    double MinimumLakeBedDepth = 500.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Lakes", meta = (EditCondition = "bGenerateLakes", ClampMin = "1.0"))
    double MaximumLakeBedDepth = 5000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Lakes", meta = (EditCondition = "bGenerateLakes", ClampMin = "0.0"))
    double MaximumLakeArea = 250.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Lakes", meta = (EditCondition = "bGenerateLakes", ClampMin = "0", ClampMax = "1024"))
    int32 MaximumLakeCount = 32;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Lakes", meta = (EditCondition = "bGenerateLakes", ClampMin = "0.0", ClampMax = "0.5"))
    double MaximumLakeCoverageFraction = 0.08;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Lakes", meta = (EditCondition = "bGenerateLakes", ClampMin = "0.0"))
    double LakeBankBlendWidth = 24000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Lakes", meta = (EditCondition = "bGenerateLakes", ClampMin = "500.0"))
    double LakeDepthRampWidth = 7500.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Lakes", meta = (EditCondition = "bGenerateLakes", ClampMin = "0.0"))
    double LakeSurfaceInset = 100.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Lakes", meta = (EditCondition = "bGenerateLakes"))
    bool bGenerateLakeOutflows = true;

    // ------------------------------------------------------------------
    // Ocean (the supplied implementation currently creates only a water
    // boundary; its bathymetry parameters are not yet applied to terrain)
    // ------------------------------------------------------------------

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Ocean")
    bool bGenerateOcean = false;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Ocean", meta = (EditCondition = "bGenerateOcean"))
    double SeaLevel = 0.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Ocean", meta = (EditCondition = "bGenerateOcean", ClampMin = "100.0"))
    double CoastTransitionWidth = 100000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Ocean", meta = (EditCondition = "bGenerateOcean", ClampMin = "0.0"))
    double MinimumOceanDepth = 5000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Ocean", meta = (EditCondition = "bGenerateOcean", ClampMin = "0.0"))
    double MaximumOceanDepth = 50000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Ocean", meta = (EditCondition = "bGenerateOcean"))
    bool bOceanWidthEdges = true;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Ocean", meta = (EditCondition = "bGenerateOcean"))
    bool bOceanLengthEnds = false;

protected:
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    void InvalidateData();
    void CreateWaterActors(const TSharedPtr<const FAvenorStripData>& Data);

    mutable FCriticalSection DataMutex;
    mutable TSharedPtr<const FAvenorStripData> CachedData;
};
