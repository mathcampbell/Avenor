#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MeshPartitionModifierComponent.h"

#include "AvenorStripTerrainGenerator.generated.h"

struct FAvenorStripData;
class UProceduralMeshComponent;
class UAvenorTerrainData;

UENUM(BlueprintType)
enum class EAvenorStripLongAxis : uint8
{
    X UMETA(DisplayName = "X (east-west)"),
    Y UMETA(DisplayName = "Y (north-south)")
};

/** Broad controls for the continuous structural terrain field. */
USTRUCT(BlueprintType)
struct FAvenorLandformSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, Category = "Landforms", meta = (Units = "cm", ClampMin = "25000.0", ClampMax = "600000.0", ToolTip = "Overall vertical relief available to the structural terrain system. Mountains, uplands, basins and rifts all emerge from this same field."))
    double ReliefHeight = 300000.0;

    UPROPERTY(EditAnywhere, Category = "Landforms", meta = (Units = "cm", ClampMin = "0.0", ClampMax = "5000000.0", ToolTip = "Typical scale of major ridges, uplands, basins and rift structures. This is not a mountain size control. Set to 0 to auto-derive this from the world's short-axis extent (roughly half the short axis, so multiple provinces fit across the width) - this keeps feature scale sensible as you resize a test world. Enter an explicit value to pin a specific scale regardless of world size."))
    double StructuralScale = 0.0;

    UPROPERTY(EditAnywhere, Category = "Landforms", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Controls how strongly broad uplift and compression become rugged high-relief terrain. Lower values favour older rolling terrain; higher values favour active rugged relief."))
    double TectonicActivity = 0.65;

    UPROPERTY(EditAnywhere, Category = "Landforms", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Strength of elongated subsidence and extension structures. These become rift valleys, fault-bounded basins and major lowland corridors."))
    double RiftStrength = 0.45;
};

/**
 * Low-frequency climate controls. The climate simulation itself always runs
 * on the existing analysis grid, so enabling it does not add another
 * high-resolution world simulation - only the baked texture maps below can
 * be rasterized at an independent, much finer resolution than that grid.
 */
USTRUCT(BlueprintType)
struct FAvenorClimateSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, Category = "Climate")
    bool bEnabled = true;

    UPROPERTY(EditAnywhere, Category = "Climate", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Middle point of the world's temperature range. 0 is cold and 1 is hot. Broad climate regions vary around this value and elevation cools the local result automatically."))
    double Temperature = 0.5;

    UPROPERTY(EditAnywhere, Category = "Climate", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Middle point of the world's precipitation range. Broad climate regions vary around this value before terrain drainage and proximity to generated water are applied."))
    double Moisture = 0.5;

    UPROPERTY(EditAnywhere, Category = "Climate", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Controls how strongly successive broad climate regions differ. The default is deliberately high enough for a long Avenor strip to contain clearly different wet, dry, cold and warm regions without abrupt biome bands."))
    double RegionalVariation = 0.72;

    UPROPERTY(EditAnywhere, Category = "Climate", meta = (Units = "cm", ClampMin = "0.0", ClampMax = "3000000.0", ToolTip = "Characteristic length of a broad climate region along the world's long axis. Values are smoothly interpolated, so this is not a hard biome boundary or grid size. Set to 0 to auto-derive this as roughly 1/6 of the world's long-axis length (about 6-7 climate regions across the world regardless of size), so a short test world still shows real climate variety instead of only 1-2 anchors. Enter an explicit value to pin a specific region length regardless of world size."))
    double RegionSpacing = 0.0;

    UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "Climate", meta = (Units = "cm", ClampMin = "10000.0", ToolTip = "Distance over which rivers and lakes increase local habitat moisture."))
    double WaterInfluenceDistance = 100000.0;

    UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "Climate", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    double WaterMoistureBoost = 0.35;

    UPROPERTY(EditAnywhere, Category = "Climate", meta = (ToolTip = "Compresses unusually large climate changes into a short proof-of-concept map. Useful for short test worlds; normally leave disabled for a full-length world."))
    bool bShowcaseClimateCompression = false;

    UPROPERTY(EditAnywhere, Category = "Climate", meta = (ClampMin = "0.0", ClampMax = "360.0", Units = "deg", ToolTip = "Compass direction the prevailing wind blows TOWARD (0 = +X world axis, 90 = +Y), independent of the strip's long axis. Moisture is depleted on the far (lee) side of ridges that face into this wind and boosted on the near (windward) side."))
    double PrevailingWindDirectionDegrees = 0.0;

    UPROPERTY(EditAnywhere, Category = "Climate", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "How strongly mountains block moisture on their lee side and enhance it on their windward side. 0 disables rain shadows entirely, leaving moisture purely a function of the broad climate regions and elevation."))
    double RainShadowStrength = 0.6;

    UPROPERTY(EditAnywhere, Category = "Climate", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Regional temperature and moisture are otherwise fully independent random walks along the strip, so a hot region and a dry region reaching their extremes at the same location is mostly coincidence - true desert (which needs both at once) can end up rare or absent even with high Regional Variation. This nudges moisture down as temperature rises above its midpoint and up as it falls below, the same hot-is-typically-dry/cold-is-typically-wetter correlation real climates show. 0 restores full independence."))
    double ContinentalDryness = 0.45;

    UPROPERTY(EditAnywhere, Category = "Climate", meta = (ClampMin = "0.0", ClampMax = "1.5", ToolTip = "How strongly climbing toward the top of the world's relief budget cools local temperature. Scales with elevation as a fraction of Relief Height, not an absolute metre count, so it stays meaningful regardless of that setting."))
    double ElevationLapseStrength = 0.62;

    UPROPERTY(EditAnywhere, Category = "Climate", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Extra cooling for exposed mountain terrain (wind exposure, bare rock, thin soil), independent of literal elevation - lets a rocky mountain shoulder or saddle read as cold without needing to be the actual summit."))
    double MountainExposureCooling = 0.24;

    UPROPERTY(EditAnywhere, Category = "Climate", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Same idea as Mountain Exposure Cooling but for hill terrain, at a gentler default strength."))
    double HillExposureCooling = 0.09;

    UPROPERTY(EditAnywhere, Category = "Climate", meta = (Units = "cm", ClampMin = "100.0", ClampMax = "100000.0", ToolTip = "World size of one texel in the baked biome/climate/terrain-filter/water-surface texture maps, independent of Analysis Spacing. These maps are rasterized by resampling the analysis grid (smoothly for continuous fields, exactly for water/shoreline distance), so this can go much finer than the analysis grid without repeating the erosion simulation at that resolution - it is comparatively cheap. Each output texture is still capped to a safe maximum dimension, so an unreasonably fine value here is automatically coarsened per texture (per-region tiles can hold much finer detail than the single whole-world overview map)."))
    double MapTexelSize = 5000.0;
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

    UPROPERTY(EditAnywhere, Category = "Erosion", meta = (ClampMin = "10000", ClampMax = "20000000", ToolTip = "Hard cap on analysis cells, regardless of world size or Analysis Spacing. If exceeded, spacing is silently coarsened to fit (actual cell size shows in Last Build Stamp) - this is the real ceiling on baked detail. Cost is roughly linear in this number and quadratic in resolution for a fixed world size (halving cell size is about 4x the cells). 500,000 (default) is safe for quick iteration; a few million is a reasonable 'hero' bake on a capable PC - watch RAM/bake time before pushing higher. Mesh Partition's own vertex density, which can differ per platform, decides how much of this becomes visible geometry - it is not limited by this value."))
    int32 MaximumAnalysisCells = 500000;
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

    UPROPERTY(EditAnywhere, Category = "Hydrology", meta = (EditCondition = "bRivers", ClampMin = "0", ClampMax = "32", ToolTip = "Small standalone lakes placed along non-canyon river reaches that cross real desert terrain - visual variety rather than hydrological realism (a real oasis is normally spring-fed, not surface drainage). Never placed on a canyon reach, since a canyon already has its river visibly present. 0 disables oases."))
    int32 MaximumDesertOases = 4;

    UPROPERTY(EditAnywhere, Category = "Hydrology", meta = (EditCondition = "bRivers", Units = "cm", ClampMin = "1000.0", ClampMax = "50000.0", ToolTip = "Radius of each desert oasis lake."))
    double DesertOasisRadius = 8000.0;
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

    UPROPERTY(EditAnywhere, Category = "Water Terrain", meta = (Units = "cm", ClampMin = "100.0", ClampMax = "20000.0", ToolTip = "Visible material bank width baked directly from the final river splines. Independent of the much broader terrain-carving and ecological riverbank influence."))
    double MaterialRiverBankWidth = 3000.0;

    UPROPERTY(EditAnywhere, Category = "Water Terrain", meta = (Units = "cm", ClampMin = "100.0"))
    double LakeBedDepth = 3000.0;

    UPROPERTY(EditAnywhere, Category = "Water Terrain", meta = (Units = "cm", ClampMin = "100.0"))
    double LakeShoreWidth = 24000.0;

    UPROPERTY(EditAnywhere, Category = "Water Terrain", meta = (Units = "cm", ClampMin = "100.0", ClampMax = "50000.0", ToolTip = "Visible material shore width written directly to the LakeShore Mesh Partition channel from the baked lake polygon. Independent of the broader lake terrain-carving transition."))
    double MaterialLakeShoreWidth = 6000.0;

    UPROPERTY(EditAnywhere, Category = "Water Terrain", meta = (Units = "cm", ClampMin = "0.0", ClampMax = "10000.0", ToolTip = "Lowers generated lake surfaces below the detected basin shoreline. 200 cm exposes roughly two metres of natural bank without changing the lake outline."))
    double LakeSurfaceInset = 200.0;

    UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "Water Terrain", meta = (Units = "cm", ClampMin = "0.0", ClampMax = "10000.0", ToolTip = "Generator-wide dry margin between rendered water and the start of the broad terrain ramp. Applied to every generated river and lake modifier."))
    double DryBankWidth = 300.0;

    UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "Water Terrain", meta = (ClampMin = "0", ClampMax = "16", ToolTip = "Native Water Body heightmap blur radius. Small values soften triangulation without washing out the bank."))
    int32 BlurRadius = 2;

    UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "Water Terrain", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Adds restrained native curl noise to shore and bank edges."))
    double EdgeRoughness = 0.15;

    FName RiverBedWeight = TEXT("RiverBed");

    FName RiverBankWeight = TEXT("RiverBank");

    FName LakeBedWeight = TEXT("LakeBed");

    FName LakeShoreWeight = TEXT("LakeShore");
};

/** Controls local remeshing around water without changing hydrology. */
USTRUCT(BlueprintType)
struct FAvenorTerrainRefinementSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, Category = "Refinement", meta = (Units = "cm", ClampMin = "100.0", ToolTip = "Target edge length around narrow headwater channels. Three metres retains several vertices across the default channel without production-scale triangle counts."))
    double HeadwaterEdgeLength = 300.0;

    UPROPERTY(EditAnywhere, Category = "Refinement", meta = (Units = "cm", ClampMin = "100.0", ToolTip = "Target edge length around broad main-river channels."))
    double MainRiverEdgeLength = 500.0;

    UPROPERTY(EditAnywhere, Category = "Refinement", meta = (Units = "cm", ClampMin = "100.0", ToolTip = "Target edge length for canyon walls and rims."))
    double CanyonEdgeLength = 300.0;

    UPROPERTY(EditAnywhere, Category = "Refinement", meta = (Units = "cm", ClampMin = "100.0", ToolTip = "Target edge length around lake shorelines."))
    double LakeShoreEdgeLength = 400.0;

    UPROPERTY(EditAnywhere, Category = "Refinement", meta = (Units = "cm", ClampMin = "0.0", ToolTip = "Extra refined ground beyond the visible channel or shoreline. Keep this modest because its area dominates triangle count."))
    double CoverageMargin = 2000.0;

    UPROPERTY(EditAnywhere, Category = "Refinement", meta = (ClampMin = "1", ClampMax = "6", ToolTip = "Maximum local subdivision depth. Four is the production default; use three for quick iteration builds."))
    int32 MaximumTessellationLevel = 4;
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
 * Final-priority material-channel pass. It runs after all refinement/remesh
 * modifiers so newly-created water-edge vertices receive hydrology weights.
 */
UCLASS(ClassGroup = (Avenor), meta = (BlueprintSpawnableComponent))
class AVENOREDITOR_API UAvenorHydrologyChannelModifier
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

    UFUNCTION(CallInEditor, Category = "Avenor|Bake", meta = (DisplayName = "Generate and Bake Geography", ToolTip = "Generates and bakes terrain, then rebuilds the Mesh Partition mesh. Does NOT create rivers/lakes - press Regenerate Water from Baked Data afterwards, once the mesh rebuild has finished in the editor."))
    void GenerateCompleteWorld();

    UFUNCTION(CallInEditor, Category = "Avenor|Bake", meta = (DisplayName = "Rebuild World from Baked Data", ToolTip = "Rebuilds the terrain mesh from the already-baked data. Does NOT create rivers/lakes - press Regenerate Water from Baked Data afterwards, once the mesh rebuild has finished in the editor."))
    void RebuildWorldFromBakedData();

    UFUNCTION(CallInEditor, Category = "Avenor|Bake", meta = (DisplayName = "Regenerate Water from Baked Data", ToolTip = "Prepares rivers/lakes and queues a Mesh Partition mesh rebuild for them, but does NOT raycast or spawn water yet. Once the mesh rebuild has finished in the editor, press Project Water Onto Terrain."))
    void RegenerateWaterFromBakedData();

    UFUNCTION(CallInEditor, Category = "Avenor|Bake", meta = (DisplayName = "Project Water Onto Terrain", ToolTip = "Raycasts the prepared rivers/lakes onto the current terrain mesh and spawns the water actors. Press this only after Regenerate Water from Baked Data has run AND the Mesh Partition mesh rebuild it queued has finished in the editor - pressing it too early will raycast against a stale or still-building mesh and miss every point."))
    void ProjectWaterOntoTerrain();

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

    UPROPERTY(EditAnywhere, Category = "Avenor|Baked Data", meta = (ToolTip = "Authoritative saved geography. Generate and Bake creates this automatically when unassigned."))
    TSoftObjectPtr<UAvenorTerrainData> BakedTerrainData;

    int32 BakedChunkCellSize = 128;

    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain", meta = (ShowOnlyInnerProperties))
    FAvenorLandformSettings Landforms;

    UPROPERTY(EditAnywhere, Category = "Avenor|Climate", meta = (ShowOnlyInnerProperties))
    FAvenorClimateSettings Climate;

    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain", meta = (ShowOnlyInnerProperties))
    FAvenorErosionSettings Erosion;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology", meta = (ShowOnlyInnerProperties))
    FAvenorHydrologySettings Hydrology;

    UPROPERTY(EditAnywhere, Category = "Avenor|Water", meta = (ShowOnlyInnerProperties))
    FAvenorWaterTerrainSettings WaterTerrain;

    UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "Avenor|Refinement", meta = (ShowOnlyInnerProperties))
    FAvenorTerrainRefinementSettings Refinement;

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

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Avenor|Status")
    TObjectPtr<UAvenorHydrologyChannelModifier> HydrologyChannelModifier = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Avenor|Status")
    TObjectPtr<UProceduralMeshComponent> FastPreviewMesh = nullptr;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Status")
    FString LastBuildStamp;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Status")
    FString BakedDataStatus;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Status")
    FString LastRiverProjectionStatus;

    UPROPERTY(VisibleAnywhere, Transient, Category = "Avenor|Status")
    bool bTerrainPlanReadyForWater = false;

    UPROPERTY(VisibleAnywhere, Transient, Category = "Avenor|Status")
    bool bRefinementPlanReadyForWater = false;

    UPROPERTY(VisibleAnywhere, Transient, Category = "Avenor|Status")
    bool bWaterProjectionPending = false;

    // Internal resolved constants. These are deliberately not UPROPERTYs:
    // they are implementation detail, not a second user-facing settings UI.
    double AnalysisCellSize = 10000.0;
    int32 MaximumAnalysisCells = 500000;
    double StructuralRelief = 300000.0;
    double StructuralScale = 1600000.0;
    double TectonicActivity = 0.65;
    double RiftStrength = 0.45;
    bool bGenerateClimate = true;
    double ClimateTemperature = 0.5;
    double ClimateMoisture = 0.5;
    double ClimateRegionalVariation = 0.72;
    double ClimateRegionSpacing = 1500000.0;
    double ClimateMapTexelSize = 5000.0;
    double ClimateWaterInfluenceDistance = 100000.0;
    double ClimateWaterMoistureBoost = 0.35;
    bool bShowcaseClimateCompression = false;
    double PrevailingWindDirectionDegrees = 0.0;
    double RainShadowStrength = 0.6;
    double ClimateContinentalDryness = 0.45;
    double ClimateElevationLapseStrength = 0.62;
    double ClimateMountainExposureCooling = 0.24;
    double ClimateHillExposureCooling = 0.09;
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
    double RefinementEdgeLengthHeadwater = 300.0;
    double RefinementEdgeLengthMainRiver = 500.0;
    double RefinementEdgeLengthCanyon = 300.0;
    double RefinementEdgeLengthLakeShore = 400.0;
    double RefinementCoverageMargin = 2000.0;
    double RefinementMaximumCanyonRadius = 30000.0;
    int32 RefinementMaxTessellationLevel = 4;
    double HeadwaterValleyHalfWidth = 10000.0;
    double MainValleyHalfWidth = 80000.0;
    double MaximumValleyDepth = 14000.0;
    double LowlandMeanderStrength = 0.75;
    double FeatureSplinePointSpacing = 500.0;
    // Do not truncate the extracted reach graph. River density and minimum
    // catchment/length controls determine how much river exists; an arbitrary
    // reach-count cap can retain tributaries while dropping their downstream
    // trunk, producing impossible inland termini.
    int32 MaximumRiverReaches = MAX_int32;
    bool bGenerateLakes = true;
    double MinimumLakeDepth = 1500.0;
    double MinimumLakeBedDepth = 500.0;
    double MaximumLakeBedDepth = 5000.0;
    double MaximumLakeArea = 250.0;
    int32 MaximumLakeCount = 12;
    int32 MaximumDesertOases = 4;
    double DesertOasisRadius = 8000.0;
    double MaximumLakeCoverageFraction = 0.03;
    double LakeBankBlendWidth = 24000.0;
    double LakeDepthRampWidth = 7500.0;
    double LakeSurfaceInset = 0.0;
    bool bGenerateLakeOutflows = true;
    bool bGenerateOcean = false;
    double SeaLevel = 0.0;
    double CoastTransitionWidth = 100000.0;

protected:
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    void ResolveSettings();
    void InvalidateData();
    void ReleaseCachedData();
    bool BakeData(const TSharedPtr<const FAvenorStripData>& Data);
    TSharedPtr<const FAvenorStripData> LoadBakedData() const;
    void BuildCompleteWorldFromCurrentData();
    FString BuildSettingsSnapshot() const;
    FString BuildSettingsHash() const;
    void CreateWaterActors(const TSharedPtr<const FAvenorStripData>& Data);
    bool BindModifiersAndRefresh(bool bShowFailureDialog);

    mutable FCriticalSection DataMutex;
    mutable TSharedPtr<const FAvenorStripData> CachedData;
    TSharedPtr<const FAvenorStripData> PendingWaterData;
    bool bDeferMeshRefresh = false;
    mutable bool bGeneratingGeography = false;
};

// The terrain implementation defines a local lambda named Oriented and uses it
// only for geological structure. Redirect those calls through a deterministic
// seed- and position-dependent rotation. This deliberately leaves LongAxis for
// the macro climate system, while ridges, rifts and uplift no longer inherit
// the rectangular strip's compass direction. The low-frequency bend prevents
// an otherwise realistic range from staying ruler-straight for tens of km.
namespace UE::Avenor::Strip::StructuralOrientation
{
static FORCEINLINE FVector2D RotateAxis(
    const FVector2D& Axis,
    int32 Seed,
    const FVector2D& Position,
    double StructuralScale
)
{
    const double SafeScale = FMath::Max(100000.0, StructuralScale);
    const double SeedAngle = FMath::Fmod(
        FMath::Abs(static_cast<double>(Seed)) * 0.000827 + 0.617,
        PI
    );
    const double Bend =
        FMath::Sin((Position.X + Position.Y * 0.61
            + static_cast<double>(Seed) * 173.0) / (SafeScale * 1.65)) * 0.20
        + FMath::Sin((Position.Y - Position.X * 0.37
            - static_cast<double>(Seed) * 97.0) / (SafeScale * 2.75)) * 0.11;
    const double Angle = SeedAngle + Bend;
    const double C = FMath::Cos(Angle);
    const double S = FMath::Sin(Angle);
    return FVector2D(
        Axis.X * C - Axis.Y * S,
        Axis.X * S + Axis.Y * C
    );
}
}

#define Oriented(P, Along, Across, AlongStretch, AcrossStretch) \
    Oriented( \
        (P), \
        UE::Avenor::Strip::StructuralOrientation::RotateAxis( \
            (Along), Seed, Position, Scale), \
        UE::Avenor::Strip::StructuralOrientation::RotateAxis( \
            (Across), Seed, Position, Scale), \
        (AlongStretch), (AcrossStretch))
