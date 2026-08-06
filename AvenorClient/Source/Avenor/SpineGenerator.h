#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SpineGenerator.generated.h"

class UPCGComponent;
class UPCGGraphInterface;
class USceneComponent;
class USplineComponent;

/** An art-directed adjustment applied on top of the authored guide spline. */
USTRUCT(BlueprintType)
struct FSpineEffector
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spine")
    bool bEnabled = true;

    /** Signed centimetres from Station 0. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spine")
    float Chainage = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spine",
        meta = (ClampMin = "100.0"))
    float InfluenceRadius = 50000.0f;

    /** Positive values move the Spine towards local right. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spine")
    float LateralOffset = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spine")
    float VerticalOffset = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spine",
        meta = (ClampMin = "0.1"))
    float FalloffExponent = 2.0f;
};

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
 * already contains the scale required for roads, guideways and local streets.
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
    USplineComponent* GetGuideSpline() const { return GuideSpline; }

    /** Rebuild deterministic PCG inputs without generating visible content. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|Spine")
    void RebuildLayoutData();

    /** Rebuild inputs, then execute the assigned PCG graph. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|PCG")
    void RegenerateInfrastructure();

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

    void GetBaseSplineFrameAtChainage(
        float Chainage,
        FVector& OutLocation,
        FVector& OutForward
    ) const;
    void RebuildDerivedSplines();
    void ClearGeneratedPlanningData();
    void RebuildStationAndBlockRecords();
    void RebuildGreyboxSegments();
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
    float GetDevelopmentOuterLateral() const;
    FString FormatSignedId(const TCHAR* Prefix, int32 Index) const;

    UPROPERTY(VisibleAnywhere, Category = "Avenor")
    TObjectPtr<USceneComponent> SceneRoot;

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

    /** Distance along GuideSpline that represents Station 0. */
    UPROPERTY(EditAnywhere, Category = "Avenor|Alignment",
        meta = (ClampMin = "0.0"))
    float StationZeroSplineDistance = 204800.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Alignment",
        meta = (ClampMin = "100.0"))
    float AlignmentSampleLength = 2500.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Alignment")
    TArray<FSpineEffector> Effectors;

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

    UPROPERTY(EditAnywhere, Category = "Avenor|Spine Road",
        meta = (ClampMin = "300.0"))
    float HighwayCarriagewayWidth = 700.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Spine Road",
        meta = (ClampMin = "800.0"))
    float HighwayMedianWidth = 1600.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Spine Road",
        meta = (ClampMin = "1.0"))
    float RoadThickness = 20.0f;

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
};
