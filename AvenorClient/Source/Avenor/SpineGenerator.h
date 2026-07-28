#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SpineGenerator.generated.h"

class USceneComponent;
class USplineComponent;
class UStaticMesh;
class UInstancedStaticMeshComponent;
class UPCGComponent;
class UPCGGraphInterface;

USTRUCT(BlueprintType)
struct FSpineEffector
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spine")
    bool bEnabled = true;

    // Signed centimetres from Station 0.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spine")
    float Chainage = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spine",
        meta = (ClampMin = "100.0"))
    float InfluenceRadius = 50000.0f;

    // Positive values move the spine towards local north/right.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spine")
    float LateralOffset = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spine")
    float VerticalOffset = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spine",
        meta = (ClampMin = "0.1"))
    float FalloffExponent = 2.0f;
};

UCLASS()
class AVENOR_API ASpineGenerator : public AActor
{
    GENERATED_BODY()

public:
    ASpineGenerator();

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
    USplineComponent* GetGuideSpline() const { return GuideSpline; }

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|PCG")
    void RegenerateInfrastructure();

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|PCG")
    void ClearInfrastructure();

    UFUNCTION(BlueprintPure, Category = "Avenor|Spine")
    void GetSpineSpaceForWorldLocation(
        const FVector& WorldLocation,
        float& OutChainage,
        float& OutLateral,
        float& OutVertical
    ) const;

protected:
    virtual void OnConstruction(const FTransform& Transform) override;

private:
    void RebuildStrip();
    void GetBaseSplineFrameAtChainage(
        float Chainage,
        FVector& OutLocation,
        FVector& OutForward
    ) const;

    void AddBox(
        UInstancedStaticMeshComponent* Component,
        const FVector& Location,
        const FVector& Size,
        const FQuat& Rotation = FQuat::Identity
    );

    void AddStripSegment(
        UInstancedStaticMeshComponent* Component,
        float StartChainage,
        float EndChainage,
        float LateralOffset,
        float CentreHeight,
        float Width,
        float Thickness
    );

    UPROPERTY(VisibleAnywhere, Category = "Avenor")
    TObjectPtr<USceneComponent> SceneRoot;

    // Edit this spline directly for large-scale authored changes.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Avenor|Spine",
        meta = (AllowPrivateAccess = "true"))
    TObjectPtr<USplineComponent> GuideSpline;

    // The Spine spline is authoritative data. The assigned PCG graph should
    // create guideway, supports, stations, signs and street furniture from it.
    UPROPERTY(VisibleAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UPCGComponent> InfrastructurePCG;

    UPROPERTY(EditAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UPCGGraphInterface> InfrastructureGraph;

    // Temporary cube blockout retained for comparison/migration only.
    UPROPERTY(EditAnywhere, Category = "Avenor|Legacy")
    bool bUseLegacyBlockout = false;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Generated")
    TObjectPtr<UInstancedStaticMeshComponent> RoadInstances;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Generated")
    TObjectPtr<UInstancedStaticMeshComponent> MedianInstances;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Generated")
    TObjectPtr<UInstancedStaticMeshComponent> PavementInstances;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Generated")
    TObjectPtr<UInstancedStaticMeshComponent> ParcelInstances;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Generated")
    TObjectPtr<UInstancedStaticMeshComponent> MonorailInstances;

    // Distance along GuideSpline that represents Station 0.
    UPROPERTY(EditAnywhere, Category = "Avenor|Spine",
        meta = (ClampMin = "0.0"))
    float StationZeroSplineDistance = 50000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Spine",
        meta = (ClampMin = "100.0"))
    float GenerationSegmentLength = 1000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Spine")
    TArray<FSpineEffector> Effectors;

    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "0"))
    int32 BlocksEast = 5;

    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "0"))
    int32 BlocksWest = 5;

    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "1"))
    int32 ParcelRowsPerSide = 3;

    UPROPERTY(EditAnywhere, Category = "Avenor|Grid",
        meta = (ClampMin = "1000.0"))
    float BlockSize = 10000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Grid",
        meta = (ClampMin = "0.0"))
    float ParcelGridGap = 100.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Main",
        meta = (ClampMin = "100.0"))
    float PavementWidth = 500.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Main",
        meta = (ClampMin = "100.0"))
    float CarriagewayWidth = 2000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Main",
        meta = (ClampMin = "100.0"))
    float MedianWidth = 1000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Main",
        meta = (ClampMin = "1.0"))
    float RoadThickness = 20.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Main",
        meta = (ClampMin = "1.0"))
    float PavementThickness = 30.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Grid",
        meta = (ClampMin = "1.0"))
    float ParcelPadThickness = 10.0f;

    // Deprecated blockout display. Real parcel boundaries are generated by
    // AParcelGenerator and conform to terrain.
    UPROPERTY(EditAnywhere, Category = "Avenor|Grid")
    bool bShowLegacyParcelPads = false;

    UPROPERTY(EditAnywhere, Category = "Avenor|Monorail",
        meta = (ClampMin = "300.0"))
    float MonorailBeamCentreHeight = 900.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Monorail",
        meta = (ClampMin = "100.0"))
    float MonorailGuidewayWidth = 600.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Monorail",
        meta = (ClampMin = "20.0"))
    float MonorailBeamDepth = 120.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Monorail",
        meta = (ClampMin = "500.0"))
    float SupportSpacing = 2500.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Monorail",
        meta = (ClampMin = "20.0"))
    float SupportWidth = 120.0f;

    UPROPERTY()
    TObjectPtr<UStaticMesh> CubeMesh;
};
