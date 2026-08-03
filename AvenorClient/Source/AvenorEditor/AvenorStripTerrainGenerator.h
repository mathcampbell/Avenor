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
     * Stage 2 can follow immediately because it consumes the cached feature
     * plan rather than the built mesh.
     */
    UFUNCTION(CallInEditor, Category = "Avenor|Generation",
        meta = (DisplayName = "1. Generate Terrain (No Water)"))
    void GenerateTerrain();

    /**
     * Stage 2: place USplineRemeshModifier components along the cached
     * river reaches and lake shorelines so Mesh Partition creates dense
     * vertices exactly where crisp banks/shores/canyon rims are needed.
     * This uses the feature plan produced by stage 1, so it can be run as
     * soon as Generate Terrain has completed; an intermediate Mesh Partition
     * rebuild is not required. Avenor assigns spline remesh to the first
     * priority layer declared by the Mesh Partition Definition and terrain
     * carving to the last layer. The later terrain pass then evaluates its
     * analytic height function on every vertex created by remeshing.
     */
    UFUNCTION(CallInEditor, Category = "Avenor|Generation",
        meta = (DisplayName = "2. Generate/Update Refinement Splines"))
    void GenerateRefinementSplines();

    /**
     * Complete editor iteration path. Recalculates the feature plan, replaces
     * refinement splines, then notifies and refreshes Mesh Partition in-place.
     * The Mesh Partition build can still take time, but saving/reloading the
     * level should not be necessary to see its result.
     */
    UFUNCTION(CallInEditor, Category = "Avenor|Generation",
        meta = (DisplayName = "Generate + Refresh Mesh Terrain"))
    void RegenerateAndRefreshTerrain();

    /** Re-submit the current modifiers and redraw Mesh Partition in-place. */
    UFUNCTION(CallInEditor, Category = "Avenor|Generation",
        meta = (DisplayName = "Refresh Mesh Terrain In Place"))
    void RefreshMeshTerrainInPlace();

    /**
     * Build a lightweight local mesh directly from the analytic height
     * function. This bypasses Mesh Partition, collision and water so terrain
     * parameters can be iterated in seconds.
     */
    UFUNCTION(CallInEditor, Category = "Avenor|Preview",
        meta = (DisplayName = "Generate Fast Local Preview"))
    void GenerateFastPreview();

    UFUNCTION(CallInEditor, Category = "Avenor|Preview")
    void ClearFastPreview();

    /**
     * Stage 3: create Water actors from the exact cached river/lake plan
     * used to carve the terrain. Refuses to run until stage 1 has
     * succeeded; run this after stage 2's Mesh Partition build finishes.
     */
    UFUNCTION(CallInEditor, Category = "Avenor|Generation",
        meta = (DisplayName = "3. Generate Water (After Rebuild)"))
    void GenerateWater();

    UFUNCTION(CallInEditor, Category = "Avenor|Generation")
    void ClearGeneratedWater();

    UFUNCTION(CallInEditor, Category = "Avenor|Generation")
    void ClearGeneratedRefinementSplines();

    FBox GetGenerationBounds() const;
    TSharedPtr<const FAvenorStripData> GetOrCreateData() const;

    // ------------------------------------------------------------------
    // Target and world layout
    // ------------------------------------------------------------------

    UPROPERTY(EditAnywhere, Category = "Avenor|Target")
    TObjectPtr<AActor> MeshPartitionActor = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Avenor|Target")
    TObjectPtr<UAvenorStripTerrainModifier> TerrainModifier = nullptr;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Avenor|Preview")
    TObjectPtr<UProceduralMeshComponent> FastPreviewMesh = nullptr;

    /** Centre of the preview relative to this generator, in centimetres. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Preview", meta = (Units = "cm"))
    FVector2D PreviewCentreOffset = FVector2D::ZeroVector;

    /** A 2 km square is large enough to judge a proof-of-concept district. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Preview",
        meta = (Units = "cm", ClampMin = "10000.0"))
    FVector2D PreviewSize = FVector2D(200000.0, 200000.0);

    /** Preview-only grid spacing; does not alter production terrain density. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Preview",
        meta = (Units = "cm", ClampMin = "500.0", ClampMax = "25000.0"))
    double PreviewVertexSpacing = 2500.0;

    /** Keeps the preview visibly separate from the currently compiled mesh. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Preview", meta = (Units = "cm"))
    double PreviewDisplayOffsetZ = 500000.0;

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

    UPROPERTY(VisibleAnywhere, Transient, Category = "Avenor|Generation")
    bool bRefinementPlanReadyForWater = false;

    // ------------------------------------------------------------------
    // Blended landform zones. The spine is deliberately NOT a height bias
    // of any kind here - only mountains are excluded near it (see
    // MountainExclusionHalfWidth below). A separate terrain modifier is
    // intended to shape the spine land itself later.
    // ------------------------------------------------------------------

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Zones", meta = (ClampMin = "10000.0"))
    double ZoneLength = 2500000.0;

    UPROPERTY(
        EditAnywhere,
        Category = "Avenor|Landforms|Zones",
        meta = (ClampMin = "0.0",
            ToolTip = "Multiplier applied to placed mountain-range relief. 0 removes mountain relief; 1 uses the configured Mountain Relief."))
    double MountainZoneWeight = 1.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Zones", meta = (ClampMin = "0.0"))
    double HillZoneWeight = 1.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Zones", meta = (ClampMin = "0.0"))
    double DesertZoneWeight = 0.35;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Zones", meta = (ClampMin = "0.0"))
    double PlainsZoneWeight = 1.25;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Mountains")
    bool bGenerateMountains = true;

    UPROPERTY(
        EditAnywhere,
        Category = "Avenor|Landforms|Mountains",
        meta = (EditCondition = "bGenerateMountains", ClampMin = "0.0", Units = "cm",
            ToolTip = "Peak relief at a range's own core - guaranteed, not diluted by any blend.")
    )
    double MountainRelief = 300000.0;

    UPROPERTY(
        EditAnywhere,
        Category = "Avenor|Landforms|Mountains",
        meta = (EditCondition = "bGenerateMountains", ClampMin = "0.0",
            ToolTip = "How many discrete mountain ranges to place per 100km of world length.")
    )
    double MountainRangesPer100Km = 3.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Mountains", meta = (EditCondition = "bGenerateMountains", ClampMin = "10000.0", Units = "cm"))
    double MountainRangeLength = 1800000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Mountains", meta = (EditCondition = "bGenerateMountains", ClampMin = "10000.0", Units = "cm"))
    double MountainRangeWidth = 550000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Mountains", meta = (EditCondition = "bGenerateMountains", ClampMin = "10000.0", Units = "cm"))
    double MountainPeakSpacing = 260000.0;

    UPROPERTY(
        EditAnywhere,
        Category = "Avenor|Landforms|Mountains",
        meta = (EditCondition = "bGenerateMountains", ClampMin = "0.0", Units = "cm",
            ToolTip = "A range's near edge (not just its centre) is kept at least this far from the spine centreline. Nothing else (hills, desert, plains) is affected by distance from the spine at all.")
    )
    double MountainExclusionHalfWidth = 150000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Hills")
    bool bGenerateHills = true;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Hills", meta = (EditCondition = "bGenerateHills", ClampMin = "0.0"))
    double HillsRelief = 65000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Hills", meta = (EditCondition = "bGenerateHills", ClampMin = "1000.0"))
    double HillsScale = 220000.0;

    UPROPERTY(
        EditAnywhere,
        Category = "Avenor|Landforms|Mesas and Canyons",
        meta = (EditCondition = "bGenerateMesasAndCanyons",
            ToolTip = "Mesa and canyon shape is never painted directly - it emerges from erosion. This only enables the desert zone (gentle base terrain + resistant rock) and lets rivers there cut real canyons and leave real mesa remnants.")
    )
    bool bGenerateMesasAndCanyons = false;

    UPROPERTY(EditAnywhere, Category = "Avenor|Landforms|Mesas and Canyons", meta = (EditCondition = "bGenerateMesasAndCanyons", ClampMin = "1000.0"))
    double MesaScale = 400000.0;

    UPROPERTY(
        EditAnywhere,
        Category = "Avenor|Landforms|Mesas and Canyons",
        meta = (EditCondition = "bGenerateMesasAndCanyons", ClampMin = "0.0", ClampMax = "1.0",
            ToolTip = "How strongly resistant rock resists erosion. This is what actually produces mesas: terrain a river erodes past but doesn't cut through is left standing.")
    )
    double ErosionResistanceStrength = 0.55;

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

    UPROPERTY(
        EditAnywhere,
        Category = "Avenor|Hydrology|Rivers",
        meta = (EditCondition = "bGenerateRivers", ClampMin = "0.5", ClampMax = "6.0",
            ToolTip = "Shape of just the wetted channel, separate from the wider valley. 1.0 is a smooth rounded bowl. Higher values keep the bed near full depth across most of the width and rise sharply only right at the true waterline, giving a flatter deeper bed with defined banks.")
    )
    double RiverChannelSteepness = 2.2;

    // --- Refinement splines (Mesh Partition spline remesh, driven by the
    // same river/lake data already extracted above) ---

    UPROPERTY(EditAnywhere, Category = "Avenor|Refinement", meta = (Units = "cm", ClampMin = "10.0"))
    double RefinementEdgeLengthHeadwater = 150.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Refinement", meta = (Units = "cm", ClampMin = "10.0"))
    double RefinementEdgeLengthMainRiver = 200.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Refinement", meta = (Units = "cm", ClampMin = "10.0"))
    double RefinementEdgeLengthCanyon = 100.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Refinement", meta = (Units = "cm", ClampMin = "10.0"))
    double RefinementEdgeLengthLakeShore = 150.0;

    UPROPERTY(
        EditAnywhere,
        Category = "Avenor|Refinement",
        meta = (Units = "cm", ClampMin = "0.0",
            ToolTip = "Extra remesh width outside a river bank or lake shoreline. This is deliberately added to the channel width, not the much wider broad valley width.")
    )
    double RefinementCoverageMargin = 2500.0;

    UPROPERTY(
        EditAnywhere,
        Category = "Avenor|Refinement",
        meta = (Units = "cm", ClampMin = "1000.0",
            ToolTip = "Maximum half-width refined around a canyon centreline. This prevents a very wide valley from being tessellated at 1-2 metre resolution across its full width.")
    )
    double RefinementMaximumCanyonRadius = 30000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Refinement", meta = (ClampMin = "0", ClampMax = "10"))
    int32 RefinementMaxTessellationLevel = 6;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "100.0"))
    double HeadwaterValleyHalfWidth = 10000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "100.0"))
    double MainValleyHalfWidth = 80000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "0.0"))
    double MaximumValleyDepth = 14000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology|Rivers", meta = (EditCondition = "bGenerateRivers", ClampMin = "0.0", ClampMax = "3.0"))
    double LowlandMeanderStrength = 0.75;

    UPROPERTY(
        EditAnywhere,
        Category = "Avenor|Hydrology",
        meta = (Units = "cm", ClampMin = "100.0", ClampMax = "2500.0",
            ToolTip = "World-space spacing of the final shared river and lake polylines. This is independent of the hydrology analysis-cell size and is used by terrain carving, spline remeshing and Water Bodies alike. 500 cm gives 5 m feature geometry without increasing the global analysis grid."))
    double FeatureSplinePointSpacing = 500.0;

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
    bool BindModifiersAndRefresh(bool bShowFailureDialog);

    mutable FCriticalSection DataMutex;
    mutable TSharedPtr<const FAvenorStripData> CachedData;
    bool bDeferMeshRefresh = false;
};
