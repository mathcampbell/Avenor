#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#if WITH_EDITORONLY_DATA
#include "MeshPartitionModifierComponent.h"
#endif
#include "SpineGenerator.generated.h"

class UPCGComponent;
class UPCGGraphInterface;
class USceneComponent;
class USplineComponent;
class ASpineGenerator;
class UAvenorTerrainData;

/** One sample in the solved road-level vertical alignment. */
USTRUCT(BlueprintType)
struct FSpineAlignmentSample
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
    float Chainage = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
    float NaturalTerrainZ = 0.0f;

    /**
     * Smoothed, slope-limited ground profile from the road edge to the outside
     * development edge on the left. Index zero is pinned to RoadDatumZ.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
    TArray<float> LeftDevelopmentProfileZ;

    /** Right-side equivalent; it is solved independently from the left side. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
    TArray<float> RightDevelopmentProfileZ;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
    float RoadDatumZ = 0.0f;

    /** Positive is fill/embankment; negative is cut into natural terrain. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
    float EarthworkDelta = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
    bool bStructureCandidate = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Alignment")
    bool bTerrainHit = false;
};

/**
 * Development grading modifier owned by the Spine actor. It changes vertices
 * in the existing Mesh Terrain; it never creates a second terrain surface.
 */
#if WITH_EDITORONLY_DATA
UCLASS(ClassGroup = (Avenor), meta = (BlueprintSpawnableComponent))
class AVENOR_API UAvenorSpineTerrainModifier
    : public UE::MeshPartition::UModifierComponent
{
    GENERATED_BODY()

public:
    virtual TArray<FBox> ComputeBounds() const override;
    virtual TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
        CreateBackgroundOp(
            UE::MeshPartition::EBuildType BuildType
        ) const override;
    virtual FGuid GetCodeVersionKey() const override;
};
#endif

/**
 * A station datum consumed by PCG through Get Actor Property.
 * Transform is the road-level station origin; PlatformDatum is relative to it.
 */
USTRUCT(BlueprintType)
struct FSpineStationRecord
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Station")
    FTransform Transform = FTransform::Identity;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Station")
    FName StationId;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Station")
    int32 StationIndex = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Station")
    float Chainage = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Station")
    float PlatformDatum = 930.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Station")
    int32 PublicRealmBayIndex = 0;
};

/** Exact 100 m clear block generated from a 1,024 m station district. */
USTRUCT(BlueprintType)
struct FSpineBlockRecord
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Block")
    FTransform Transform = FTransform::Identity;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Block")
    FName BlockId;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Block")
    FName ZoneRole = FName(TEXT("Development"));

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Block")
    int32 DistrictIndex = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Block")
    int32 BayIndex = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Block")
    int32 RowIndex = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Block")
    int32 Side = 1;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Block")
    float Chainage = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Block")
    float Lateral = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Block")
    FVector ClearSize = FVector(10000.0f, 10000.0f, 0.0f);
};

/**
 * Disposable visible blockout input. With Engine/Cube as the mesh, Transform
 * already contains the scale required for highway and local-street cubes.
 * Finished monorail assets use FSpineInfrastructurePlacement instead.
 */
USTRUCT(BlueprintType)
struct FSpineGreyboxSegment
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Greybox")
    FTransform Transform = FTransform::Identity;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Greybox")
    FName Kind;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Greybox")
    int32 DistrictIndex = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Greybox")
    int32 Side = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Greybox")
    float Chainage = 0.0f;
};

/**
 * Unit-scale placement for a finished infrastructure mesh. Unlike a greybox
 * segment, Transform does not contain cube dimensions. PCG can therefore feed
 * it directly to a Static Mesh Spawner using an asset authored in centimetres.
 */
USTRUCT(BlueprintType)
struct FSpineInfrastructurePlacement
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Infrastructure")
    FTransform Transform = FTransform::Identity;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Infrastructure")
    FName Kind;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Infrastructure")
    int32 SpanIndex = 0;

    /** -1/+1 for individual guideways; zero for a shared pier/support. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Infrastructure")
    int32 Side = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Infrastructure")
    float Chainage = 0.0f;
};

/**
 * Unit-scale placement for a street-lamp Blueprint. Local +X points from the
 * pole toward the carriageway unless StreetLampYawOffset is used to compensate
 * for a differently authored asset.
 */
USTRUCT(BlueprintType)
struct FSpineStreetLampPlacement
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Street Lamp")
    FTransform Transform = FTransform::Identity;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Street Lamp")
    FName RoadKind;

    /** Stable index within RoadKind; useful for future PCG filtering. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Street Lamp")
    int32 RoadIndex = 0;

    /** -1/+1 identifies the negative or positive side of the Spine. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Street Lamp")
    int32 RoadSide = 0;

    /** -1/+1 identifies which edge of that road owns the lamp. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Street Lamp")
    int32 EdgeSide = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Street Lamp")
    float Chainage = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Street Lamp")
    float Lateral = 0.0f;
};

/**
 * Authoritative editor-time spatial definition for Avenor's developed Spine.
 *
 * This actor plans and exposes data; PCG constructs the visible infrastructure.
 * The generated arrays are deliberately flat so UE's Get Actor Property node
 * can convert them to Attribute Sets without a custom PCG element.
 */
UCLASS()
class AVENOR_API ASpineGenerator : public AActor
{
    GENERATED_BODY()

public:
    ASpineGenerator();

    virtual void PostLoad() override;

    UFUNCTION(BlueprintCallable, Category = "Avenor|Spine")
    FTransform GetSpineTransformAtChainage(float Chainage) const;

    UFUNCTION(BlueprintCallable, Category = "Avenor|Spine")
    FVector GetSpineLocationAtChainage(
        float Chainage,
        float LateralOffset = 0.0f,
        float VerticalOffset = 0.0f
    ) const;

    UFUNCTION(BlueprintPure, Category = "Avenor|Spine")
    float GetMinimumChainage() const;

    UFUNCTION(BlueprintPure, Category = "Avenor|Spine")
    float GetMaximumChainage() const;

    UFUNCTION(BlueprintPure, Category = "Avenor|Spine")
    float GetResolvedLocalStreetWidth() const;

    UFUNCTION(BlueprintPure, Category = "Avenor|Spine")
    float GetResolvedLocalPavementWidth() const;

    UFUNCTION(BlueprintPure, Category = "Avenor|Spine")
    float GetResolvedStationPavementWidth() const;

    UFUNCTION(BlueprintPure, Category = "Avenor|Spine")
    float GetSpineCentralPublicRealmWidth() const;

    UFUNCTION(BlueprintPure, Category = "Avenor|Spine")
    USplineComponent* GetGuideSpline() const { return GuideSpline; }

    /** Rebuild deterministic PCG inputs without generating visible content. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|Spine")
    void RebuildLayoutData();

    /** Rebuild inputs, then execute the assigned PCG graph. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|PCG")
    void RegenerateInfrastructure();

    /** Sample shared terrain data, store the Spine layer, then refresh grading. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|Terrain",
        meta = (DisplayName = "Rebuild Terrain Alignment"))
    void RebuildTerrainAlignment();

    /** Rebuild terrain alignment first, then regenerate all infrastructure. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|Spine",
        meta = (DisplayName = "Regenerate Complete Spine"))
    void RegenerateCompleteSpine();

    /** Unbind the grading modifier and discard its solved alignment. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|Terrain")
    void ClearTerrainAlignment();

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|PCG")
    void ClearInfrastructure();

    /** Remove legacy generated data and restore the lightweight test extent. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|PCG")
    void ResetToPrototypeDefaults();

    UFUNCTION(BlueprintPure, Category = "Avenor|Spine")
    void GetSpineSpaceForWorldLocation(
        const FVector& WorldLocation,
        float& OutChainage,
        float& OutLateral,
        float& OutVertical
    ) const;

private:
    static constexpr int32 BlocksPerDistrict = 9;

#if WITH_EDITORONLY_DATA
    friend class UAvenorSpineTerrainModifier;
#endif

    void GetBaseSplineFrameAtChainage(
        float Chainage,
        FVector& OutLocation,
        FVector& OutForward
    ) const;
    void RebuildDerivedSplines();
    void ClearGeneratedPlanningData();
    void RebuildStationAndBlockRecords();
    void RebuildGreyboxSegments();
    void RebuildMonorailPlacements();
    void RebuildStreetLampPlacements();
    void AddStreetLampPlacement(
        FName RoadKind,
        int32 RoadIndex,
        int32 RoadSide,
        int32 EdgeSide,
        float Chainage,
        float Lateral,
        float RoadCentreChainage,
        float RoadCentreLateral
    );
    void AddDerivedSplinePoint(
        USplineComponent* Spline,
        float Chainage,
        float Lateral,
        float Vertical
    );
    void AddGreyboxSpan(
        FName Kind,
        float StartChainage,
        float EndChainage,
        float StartLateral,
        float EndLateral,
        float CentreHeight,
        float Width,
        float Thickness,
        int32 DistrictIndex,
        int32 Side
    );
    float GetBlockCentreOffset(int32 BayIndex) const;
    float GetInternalStreetCentreOffset(int32 StreetIndex) const;
    float GetSpineCarriagewayCentreOffset() const;
    float GetSpineOuterKerbLateral() const;
    float GetDevelopmentOuterLateral() const;
    float GetDevelopmentProfileStartLateral() const;
    FString FormatSignedId(const TCHAR* Prefix, int32 Index) const;
    bool SolveTerrainAlignment();
    bool StoreTerrainAlignmentLayer();
    void LoadTerrainAlignmentLayer();
    bool BindTerrainModifier();
    float EvaluateRoadDatumZ(float Chainage) const;
    float EvaluateDevelopmentSurfaceZ(
        float Chainage,
        float Lateral
    ) const;
    FTransform GetDevelopmentSurfaceTransformAtChainage(
        float Chainage,
        float Lateral
    ) const;
    FBox GetTerrainCorridorBounds() const;

    UPROPERTY(VisibleAnywhere, Category = "Avenor")
    TObjectPtr<USceneComponent> SceneRoot;

#if WITH_EDITORONLY_DATA
    /** Grades the road reservation, parcels and outer transition together. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Avenor|Terrain",
        meta = (AllowPrivateAccess = "true"))
    TObjectPtr<UAvenorSpineTerrainModifier> TerrainCorridorModifier;
#endif

    /** Edit this spline directly. Station 0 is an offset along it. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Avenor|Spine",
        meta = (AllowPrivateAccess = "true"))
    TObjectPtr<USplineComponent> GuideSpline;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly,
        Category = "Avenor|PCG|Derived Paths",
        meta = (AllowPrivateAccess = "true"))
    TObjectPtr<USplineComponent> HighwayPositiveSpline;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly,
        Category = "Avenor|PCG|Derived Paths",
        meta = (AllowPrivateAccess = "true"))
    TObjectPtr<USplineComponent> HighwayNegativeSpline;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly,
        Category = "Avenor|PCG|Derived Paths",
        meta = (AllowPrivateAccess = "true"))
    TObjectPtr<USplineComponent> MonorailPositiveSpline;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly,
        Category = "Avenor|PCG|Derived Paths",
        meta = (AllowPrivateAccess = "true"))
    TObjectPtr<USplineComponent> MonorailNegativeSpline;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UPCGComponent> InfrastructurePCG;

    UPROPERTY(EditAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UPCGGraphInterface> InfrastructureGraph;

    UPROPERTY(EditAnywhere, Category = "Avenor|PCG")
    bool bPartitionedGeneration = false;

    /** The same Mesh Partition actor used by the main terrain generator. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain")
    TObjectPtr<AActor> MeshPartitionActor;

    /** Shared authoritative asset also assigned to the terrain generator. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain",
        meta = (DisplayName = "Baked Terrain Data"))
    TSoftObjectPtr<UAvenorTerrainData> TerrainData;

    /** Natural-terrain samples are averaged across this longitudinal window. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain",
        meta = (Units = "cm", ClampMin = "2500.0"))
    float AlignmentSmoothingDistance = 25000.0f;

    /**
     * Moves the smoothed datum toward the lower terrain in each sample window.
     * Zero is a balanced average; one strongly favours cut over embankment.
     */
    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain",
        meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float AlignmentCutBias = 0.65f;

    /** Maximum road rise/run. 0.04 is a four percent grade. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain",
        meta = (ClampMin = "0.001", ClampMax = "0.10"))
    float MaximumRoadGrade = 0.04f;

    /** Added to the sampled natural surface before solving the road datum. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain", meta = (Units = "cm"))
    float RoadDatumOffset = 0.0f;

    /** Terrain exactly matches the road datum inside the road reservation. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain",
        meta = (Units = "cm", ClampMin = "100.0"))
    float CorridorFlatHalfWidth = 2700.0f;

    /**
     * Distance beyond the outside parcel/street edge used to blend the planned
     * development surface back into untouched terrain.
     */
    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain",
        meta = (DisplayName = "Development Outer Blend Distance",
            Units = "cm", ClampMin = "100.0"))
    float CorridorTransitionHalfWidth = 12000.0f;

    /** Maximum gentle sideways grade beneath blocks and their side streets. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain",
        meta = (ClampMin = "0.0", ClampMax = "0.10"))
    float MaximumDevelopmentCrossGrade = 0.06f;

    /**
     * Lateral spacing used to sample the natural ground beneath development.
     * Smaller values retain more rolling cross-slope detail.
     */
    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain|Advanced",
        meta = (Units = "cm", ClampMin = "500.0"))
    float DevelopmentProfileSampleSpacing = 2500.0f;

    /** Longitudinal smoothing applied to each lateral profile sample. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain|Advanced",
        meta = (Units = "cm", ClampMin = "0.0"))
    float DevelopmentProfileSmoothingDistance = 10000.0f;

    /** Runs after broad terrain but before native water modifiers (priority 10). */
    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain|Advanced")
    float TerrainModifierPriority = 5.0f;

    /** Samples beyond this cut/fill depth are flagged for bridge/tunnel review. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Terrain|Advanced",
        meta = (Units = "cm", ClampMin = "100.0"))
    float EarthworkWarningThreshold = 1000.0f;

    UPROPERTY(VisibleAnywhere, Transient, Category = "Avenor|Terrain|Status")
    float LastMaximumCutDepth = 0.0f;

    UPROPERTY(VisibleAnywhere, Transient, Category = "Avenor|Terrain|Status")
    float LastMaximumFillHeight = 0.0f;

    UPROPERTY(VisibleAnywhere, Transient, Category = "Avenor|Terrain|Status")
    int32 LastStructureCandidateCount = 0;

    /** Distance along GuideSpline that represents Station 0. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Alignment",
        meta = (ClampMin = "0.0"))
    float StationZeroSplineDistance = 204800.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Alignment",
        meta = (ClampMin = "100.0"))
    float AlignmentSampleLength = 2500.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "0"))
    int32 DistrictsBeforeStationZero = 0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "1"))
    int32 DistrictsAfterStationZero = 1;

    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "1", ClampMax = "16"))
    int32 DevelopmentRowsPerSide = 1;

    /** Exact chainage interval. 102,400 cm = 1,024 m. */
    UPROPERTY(EditAnywhere, Category = "Avenor|District",
        meta = (ClampMin = "100000.0"))
    float StationSpacing = 102400.0f;

    /** Guaranteed usable dimension; roads are additional. */
    UPROPERTY(EditAnywhere, Category = "Avenor|District",
        meta = (ClampMin = "10000.0"))
    float BlockSize = 10000.0f;

    /** The major cross-street centred on every station. */
    UPROPERTY(EditAnywhere, Category = "Avenor|District",
        meta = (ClampMin = "100.0"))
    float StationStreetWidth = 2000.0f;

    /** One of nine longitudinal bays is permanently public realm. */
    UPROPERTY(EditAnywhere, Category = "Avenor|District",
        meta = (ClampMin = "0", ClampMax = "8"))
    int32 PublicRealmBayIndex = 0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Spine Road",
        meta = (ClampMin = "1000.0"))
    float SpineReservationWidth = 5400.0f;

    /** Two traffic lanes on each side of the central monorail public realm. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Spine Road",
        meta = (Units = "cm", ClampMin = "600.0"))
    float HighwayCarriagewayWidth = 800.0f;

    /** Wide boulevard pavement between each carriageway and its parcels. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Spine Road",
        meta = (Units = "cm", ClampMin = "200.0"))
    float SpinePavementWidth = 500.0f;

    /** Traffic surface within each resolved 13 m ordinary street reserve. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Local Roads",
        meta = (Units = "cm", ClampMin = "500.0"))
    float LocalCarriagewayWidth = 700.0f;

    /** Traffic surface within each 20 m station cross-street reserve. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Local Roads",
        meta = (Units = "cm", ClampMin = "600.0"))
    float StationCarriagewayWidth = 1000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Spine Road",
        meta = (ClampMin = "1.0"))
    float RoadThickness = 20.0f;

    /** Raised pavement surface above the carriageway surface. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Spine Road",
        meta = (Units = "cm", ClampMin = "0.0", ClampMax = "30.0"))
    float PavementKerbHeight = 15.0f;

    /** Centre-to-centre lamp interval. 5,000 cm is half a 100 m parcel. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Street Lamps",
        meta = (Units = "cm", ClampMin = "500.0"))
    float StreetLampSpacing = 5000.0f;

    /** Pole-centre distance into the pavement behind the kerb. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Street Lamps",
        meta = (Units = "cm", ClampMin = "0.0"))
    float StreetLampSetback = 50.0f;

    /** Keeps lamps away from the clear-road edge at local junctions. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Street Lamps",
        meta = (Units = "cm", ClampMin = "0.0"))
    float StreetLampJunctionClearance = 500.0f;

    /**
     * Yaw correction for the spawned Blueprint. Zero means its local +X faces
     * the road; use -90 for +Y, 180 for -X, or 90 for -Y.
     */
    UPROPERTY(EditAnywhere, Category = "Avenor|Street Lamps",
        meta = (Units = "deg", ClampMin = "-180.0", ClampMax = "180.0"))
    float StreetLampYawOffset = 0.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Street Lamps")
    bool bGenerateStreetLamps = true;

    UPROPERTY(EditAnywhere, Category = "Avenor|Monorail",
        meta = (ClampMin = "100.0"))
    float MonorailTrackCentreOffset = 620.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Monorail",
        meta = (ClampMin = "100.0"))
    float MonorailGuidewayWidth = 120.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Monorail",
        meta = (ClampMin = "20.0"))
    float MonorailGuidewayDepth = 120.0f;

    /** Beam centre; the v01 station greybox beam top is 840 cm. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Monorail",
        meta = (ClampMin = "100.0"))
    float MonorailGuidewayCentreHeight = 780.0f;

    /** Length of the reusable straight guideway mesh along local X. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Monorail",
        meta = (ClampMin = "100.0"))
    float MonorailSpanLength = 2500.0f;

    /**
     * Local pivot height for the separately exported support mesh. Set this to
     * zero when the FBX retains the common ground-level origin from Modo.
     */
    UPROPERTY(EditAnywhere, Category = "Avenor|Monorail")
    float MonorailSupportPivotHeight = 680.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Station",
        meta = (ClampMin = "100.0"))
    float StationPlatformDatum = 930.0f;

    // These properties stay reflected so Get Actor Property can read them,
    // but deliberately have no Edit/Visible flag. Showing thousands of struct
    // rows in the Details panel makes selecting this actor catastrophically
    // expensive in the editor.
    UPROPERTY(Transient, BlueprintReadOnly,
        Category = "Avenor|PCG Data",
        meta = (AllowPrivateAccess = "true"))
    TArray<FSpineStationRecord> StationRecords;

    UPROPERTY(Transient, BlueprintReadOnly,
        Category = "Avenor|PCG Data",
        meta = (AllowPrivateAccess = "true"))
    TArray<FSpineBlockRecord> BlockRecords;

    UPROPERTY(Transient, BlueprintReadOnly,
        Category = "Avenor|PCG Data",
        meta = (AllowPrivateAccess = "true"))
    TArray<FSpineGreyboxSegment> GreyboxSegments;

    /** One unit-scale placement for each individual 25 m guideway beam. */
    UPROPERTY(Transient, BlueprintReadOnly,
        Category = "Avenor|PCG Data",
        meta = (AllowPrivateAccess = "true"))
    TArray<FSpineInfrastructurePlacement> GuidewayPlacements;

    /** One central, ground-level placement at every guideway span boundary. */
    UPROPERTY(Transient, BlueprintReadOnly,
        Category = "Avenor|PCG Data",
        meta = (AllowPrivateAccess = "true"))
    TArray<FSpineInfrastructurePlacement> MonorailPierPlacements;

    /** One central upper-support placement corresponding to each pier. */
    UPROPERTY(Transient, BlueprintReadOnly,
        Category = "Avenor|PCG Data",
        meta = (AllowPrivateAccess = "true"))
    TArray<FSpineInfrastructurePlacement> MonorailSupportPlacements;

    /** Deterministic 50 m lamp points placed within generated pavements. */
    UPROPERTY(Transient, BlueprintReadOnly,
        Category = "Avenor|PCG Data",
        meta = (AllowPrivateAccess = "true"))
    TArray<FSpineStreetLampPlacement> StreetLampPlacements;

    /** Cached road-level profile shared by terrain, roads and monorail. */
    UPROPERTY(BlueprintReadOnly,
        Category = "Avenor|Terrain Data",
        meta = (AllowPrivateAccess = "true"))
    TArray<FSpineAlignmentSample> AlignmentSamples;
};
