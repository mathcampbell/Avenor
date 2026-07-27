#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SpineGenerator.generated.h"

class USceneComponent;
class UStaticMesh;
class UInstancedStaticMeshComponent;

UCLASS()
class AVENOR_API ASpineGenerator : public AActor
{
    GENERATED_BODY()

public:
    ASpineGenerator();

protected:
    virtual void OnConstruction(const FTransform& Transform) override;

private:
    void RebuildStrip();

    void AddBox(
        UInstancedStaticMeshComponent* Component,
        const FVector& Location,
        const FVector& Size
    );

    UPROPERTY(VisibleAnywhere, Category = "Avenor")
    TObjectPtr<USceneComponent> SceneRoot;

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

    /*
     * Generated extent.
     *
     * Block zero begins at Station 0 and extends east.
     * Block minus one extends from 100 m west to Station 0.
     */
    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "0"))
    int32 BlocksEast = 5;

    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "0"))
    int32 BlocksWest = 5;

    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "1"))
    int32 ParcelRowsPerSide = 3;

    /*
     * One base registry block: 100 metres.
     */
    UPROPERTY(EditAnywhere, Category = "Avenor|Grid",
        meta = (ClampMin = "1000.0"))
    float BlockSize = 10000.0f;

    /*
     * A small visual separation between parcel pads.
     * This is not yet a road or an ownership gap.
     */
    UPROPERTY(EditAnywhere, Category = "Avenor|Grid",
        meta = (ClampMin = "0.0"))
    float ParcelGridGap = 100.0f;

    /*
     * Main cross-section:
     *
     * 5 m pavement
     * 20 m carriageway
     * 10 m median
     * 20 m carriageway
     * 5 m pavement
     */
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

    /*
     * Temporary elevated monorail geometry.
     *
     * This is a placeholder guideway, not the finished train system.
     */
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