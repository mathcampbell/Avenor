#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MeshPartitionModifierComponent.h"

#include "AvenorStripTerrainGenerator.generated.h"

struct FAvenorStripData;
class UProceduralMeshComponent;

UENUM(BlueprintType)
enum class EAvenorStripLongAxis : uint8
{
    X UMETA(DisplayName = "X (east-west)"),
    Y UMETA(DisplayName = "Y (north-south)")
};

/** The few broad landform decisions that should be art-directed. */
USTRUCT(BlueprintType)
struct FAvenorLandformSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bMountains = true;

    UPROPERTY(EditAnywhere, Category = "Landforms", meta = (EditCondition = "bMountains", Units = "cm", ClampMin = "25000.0"))
    double MountainHeight = 300000.0;

    UPROPERTY(EditAnywhere, Category = "Landforms", meta = (EditCondition = "bMountains", ClampMin = "0.0", ClampMax = "12.0"))
    double MountainRangesPer100Km = 3.0;

    UPROPERTY(EditAnywhere, Category = "Landforms", meta = (EditCondition = "bMountains", Units = "cm", ClampMin = "0.0"))
    double MountainClearanceFromSpine = 150000.0;

    UPROPERTY(EditAnywhere, Category = "Landforms")
    bool bHills = true;

    UPROPERTY(EditAnywhere, Category = "Landforms", meta = (EditCondition = "bHills", Units = "cm", ClampMin = "0.0"))
    double HillHeight = 65000.0;

    UPROPERTY(EditAnywhere, Category = "Landforms", meta = (EditCondition = "bHills", Units = "cm", ClampMin = "10000.0"))
    double HillSize = 220000.0;
};

/** Erosion is deliberately one coherent process, rather than a panel of unrelated coefficients. */
USTRUCT(BlueprintType)
struct FAvenorErosionSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, Category = "Erosion", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    double Strength = 0.65;

    UPROPERTY(EditAnywhere, Category = "Erosion", meta = (ClampMin = "1", ClampMax = "24"))
    int32 Passes = 8;

    UPROPERTY(EditAnywhere, Category = "Erosion", meta = (Units = "cm", ClampMin = "2500.0", ClampMax = "50000.0", ToolTip = "Analysis spacing only. Native water modifiers provide the final detailed banks and beds."))
    double AnalysisSpacing = 10000.0;
};

/** Controls feature selection. Native Water Body settings perform the final carving. */
USTRUCT(BlueprintType)
struct FAvenorHydrologySettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, Category = "Hydrology")
    bool bRivers = true;

    UPROPERTY(EditAnywhere, Category = "Hydrology", meta = (EditCondition = "bRivers", ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Higher values admit smaller catchments and therefore produce more tributaries."))
    double RiverDensity = 0.65;

    UPROPERTY(EditAnywhere, Category = "Hydrology", meta = (EditCondition = "bRivers", Units = "cm", ClampMin = "10000.0"))
    double MinimumRiverLength = 75000.0;

    UPROPERTY(EditAnywhere, Category = "Hydrology")
    bool bLakes = true;

    UPROPERTY(EditAnywhere, Category = "Hydrology", meta = (EditCondition = "bLakes", ClampMin = "0", ClampMax = "128"))
    int32 MaximumLakes = 12;

    UPROPERTY(EditAnywhere, Category = "Hydrology", meta = (EditCondition = "bLakes", Units = "cm", ClampMin = "100.0"))
    double MinimumLakeDepression = 1500.0;
};

/** Values copied onto every generated Water Body and consumed by MeshPartitionWater. */
USTRUCT(BlueprintType)
struct FAvenorWaterTerrainSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, Category = "Water Terrain", meta = (ClampMin = "0.1", ClampMax = "5.0"))
    double RiverWidthScale = 1.0;

    UPROPERTY(EditAnywhere, Category = "Water Terrain", meta = (ClampMin = "0.1", ClampMax = "5.0"))
    double RiverDepthScale = 1.0;

    UPROPERTY(EditAnywhere, Category = "Water Terrain", meta = (Units = "cm", ClampMin = "100.0"))
    double RiverBankWidth = 12000.0;

    UPROPERTY(EditAnywhere, Category = "Water Terrain", meta = (Units = "cm", ClampMin = "100.0"))
    double LakeBedDepth = 3000.0;

    UPROPERTY(EditAnywhere, Category = "Water Terrain", meta = (Units = "cm", ClampMin = "100.0"))
    double LakeShoreWidth = 24000.0;

    UPROPERTY(EditAnywhere, Category = "Water Terrain", meta = (ClampMin = "0", ClampMax = "16", ToolTip = "Native Water Body heightmap blur radius. Small values soften triangulation without washing out the bank."))
    int32 BlurRadius = 2;

    UPROPERTY(EditAnywhere, Category = "Water Terrain", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Adds restrained native curl noise to shore and bank edges."))
    double EdgeRoughness = 0.15;

    UPROPERTY(EditAnywhere, Category = "Water Terrain|Weight Channels")
    FName RiverBedWeight = TEXT("RiverBed");

    UPROPERTY(EditAnywhere, Category = "Water Terrain|Weight Channels")
    FName RiverBankWeight = TEXT("RiverBank");

    UPROPERTY(EditAnywhere, Category = "Water Terrain|Weight Channels")
    FName LakeBedWeight = TEXT("LakeBed");

    UPROPERTY(EditAnywhere, Category = "Water Terrain|Weight Channels")
    FName LakeShoreWeight = TEXT("LakeShore");
};

UCLASS(ClassGroup = (Avenor), meta = (BlueprintSpawnableComponent))
class AVENOREDITOR_API UAvenorStripTerrainModifier : public UE::MeshPartition::UModifierComponent
{
    GENERATED_BODY()

public:
    virtual TArray<FBox> ComputeBounds() const override;
    virtual TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
        CreateBackgroundOp(UE::MeshPartition::EBuildType BuildType) const override;
    virtual FGuid GetCodeVersionKey() const override;
};

/**
 * Editor-only strip-world builder.
 *
 * Pipeline: broad landforms -> erosion -> drainage analysis -> Water Body
 * splines -> native Mesh Partition water modifiers. Only the broad eroded
 * surface is written by UAvenorStripTerrainModifier; water is never carved
 * twice.
 */
UCLASS(BlueprintType)
class AVENOREDITOR_API AAvenorStripTerrainGenerator : public AActor
{
    GENERATED_BODY()

public:
    AAvenorStripTerrainGenerator();

    UFUNCTION(CallInEditor, Category = "Avenor", meta = (DisplayName = "Generate Complete World"))
    void GenerateCompleteWorld();

    UFUNCTION(CallInEditor, Category = "Avenor", meta = (DisplayName = "Generate Fast Preview"))
    void GenerateFastPreview();

    UFUNCTION(CallInEditor, Category = "Avenor", meta = (DisplayName = "Clear Generated World"))
    void ClearGeneratedWorld();

    // Kept as C++ entry points for existing callers; intentionally hidden
    // from Details so there is one unambiguous production workflow.
    void GenerateTerrain();
    void GenerateRefinementSplines();
    void GenerateWater();
    void RegenerateAndRefreshTerrain();
    void RefreshMeshTerrainInPlace();
    void ClearFastPreview();
    void ClearGeneratedWater();
    void ClearGeneratedRefinementSplines();

    FBox GetGenerationBounds() const;
    TSharedPtr<const FAvenorStripData> GetOrCreateData() const;

    UPROPERTY(EditAnywhere, Category = "Avenor|World")
    TObjectPtr<AActor> MeshPartitionActor = nullptr;

    UPROPERTY(EditAnywhere, Category = "Avenor|World", meta = (Units = "cm", ClampMin = "10000.0"))
    FVector2D WorldSize = FVector2D(10000000.0, 2000000.0);

    UPROPERTY(EditAnywhere, Category = "Avenor|World")
    EAvenorStripLongAxis LongAxis = EAvenorStripLongAxis::X;

    UPROPERTY(EditAnywhere, Category = "Avenor|World")
    int32 Seed = 1337;

    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain", meta = (ShowOnlyInnerProperties))
    FAvenorLandformSettings Landforms;

    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain", meta = (ShowOnlyInnerProperties))
    FAvenorErosionSettings Erosion;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology", meta = (ShowOnlyInnerProperties))
    FAvenorHydrologySettings Hydrology;

    UPROPERTY(EditAnywhere, Category = "Avenor|Water", meta = (ShowOnlyInnerProperties))
    FAvenorWaterTerrainSettings WaterTerrain;

    UPROPERTY(EditAnywhere, Category = "Avenor|Preview", meta = (Units = "cm"))
    FVector2D PreviewCentreOffset = FVector2D::ZeroVector;

    UPROPERTY(EditAnywhere, Category = "Avenor|Preview", meta = (Units = "cm", ClampMin = "10000.0"))
    FVector2D PreviewSize = FVector2D(200000.0, 200000.0);

    UPROPERTY(EditAnywhere, Category = "Avenor|Preview", meta = (Units = "cm", ClampMin = "500.0", ClampMax = "25000.0"))
    double PreviewVertexSpacing = 2500.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Preview", meta = (Units = "cm"))
    double PreviewDisplayOffsetZ = 500000.0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Avenor|Status")
    TObjectPtr<UAvenorStripTerrainModifier> TerrainModifier = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Avenor|Status")
    TObjectPtr<UProceduralMeshComponent> FastPreviewMesh = nullptr;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Status")
    FString LastBuildStamp;

    UPROPERTY(VisibleAnywhere, Transient, Category = "Avenor|Status")
    bool bTerrainPlanReadyForWater = false;

    UPROPERTY(VisibleAnywhere, Transient, Category = "Avenor|Status")
    bool bRefinementPlanReadyForWater = false;

    // Internal resolved constants. These are deliberately not UPROPERTYs:
    // they are implementation detail, not a second user-facing settings UI.
    double AnalysisCellSize = 10000.0;
    int32 MaximumAnalysisCells = 500000;
    double ZoneLength = 2500000.0;
    double MountainZoneWeight = 1.0;
    double HillZoneWeight = 1.0;
    double DesertZoneWeight = 0.0;
    double PlainsZoneWeight = 1.25;
    bool bGenerateMountains = true;
    double MountainRelief = 300000.0;
    double MountainRangesPer100Km = 3.0;
    double MountainRangeLength = 1800000.0;
    double MountainRangeWidth = 550000.0;
    double MountainPeakSpacing = 260000.0;
    double MountainExclusionHalfWidth = 150000.0;
    bool bGenerateHills = true;
    double HillsRelief = 65000.0;
    double HillsScale = 220000.0;
    bool bGenerateMesasAndCanyons = false;
    double MesaScale = 400000.0;
    double ErosionResistanceStrength = 0.0;
    double CanyonStartArea = 60.0;
    int32 ThermalErosionIterations = 8;
    double ThermalErosionStrength = 0.45;
    double TalusAngleDegrees = 38.0;
    int32 StreamPowerIterations = 5;
    double StreamPowerStrength = 1.0;
    double DrainageEpsilon = 1.0;
    bool bGenerateRivers = true;
    double MountainStreamStartArea = 0.6;
    double LowlandStreamStartArea = 4.0;
    double MainRiverArea = 80.0;
    double MinimumRiverSystemLength = 75000.0;
    double HeadwaterWidth = 800.0;
    double MainRiverWidth = 12000.0;
    double MaximumRiverDepth = 1800.0;
    double RiverChannelSteepness = 2.2;
    double RefinementEdgeLengthHeadwater = 150.0;
    double RefinementEdgeLengthMainRiver = 200.0;
    double RefinementEdgeLengthCanyon = 100.0;
    double RefinementEdgeLengthLakeShore = 150.0;
    double RefinementCoverageMargin = 2500.0;
    double RefinementMaximumCanyonRadius = 30000.0;
    int32 RefinementMaxTessellationLevel = 6;
    double HeadwaterValleyHalfWidth = 10000.0;
    double MainValleyHalfWidth = 80000.0;
    double MaximumValleyDepth = 14000.0;
    double LowlandMeanderStrength = 0.75;
    double FeatureSplinePointSpacing = 500.0;
    int32 MaximumRiverReaches = 256;
    bool bGenerateLakes = true;
    double MinimumLakeDepth = 1500.0;
    double MinimumLakeBedDepth = 500.0;
    double MaximumLakeBedDepth = 5000.0;
    double MaximumLakeArea = 250.0;
    int32 MaximumLakeCount = 12;
    double MaximumLakeCoverageFraction = 0.03;
    double LakeBankBlendWidth = 24000.0;
    double LakeDepthRampWidth = 7500.0;
    double LakeSurfaceInset = 0.0;
    bool bGenerateLakeOutflows = true;
    bool bGenerateOcean = false;
    double SeaLevel = 0.0;
    double CoastTransitionWidth = 100000.0;
    double MinimumOceanDepth = 5000.0;
    double MaximumOceanDepth = 50000.0;
    bool bOceanWidthEdges = true;
    bool bOceanLengthEnds = false;

protected:
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    void ResolveSettings();
    void InvalidateData();
    void CreateWaterActors(const TSharedPtr<const FAvenorStripData>& Data);
    bool BindModifiersAndRefresh(bool bShowFailureDialog);

    mutable FCriticalSection DataMutex;
    mutable TSharedPtr<const FAvenorStripData> CachedData;
    bool bDeferMeshRefresh = false;
};
