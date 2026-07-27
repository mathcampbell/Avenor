#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ParcelGenerator.generated.h"

class AProceduralTerrainGenerator;
class ASpineGenerator;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class USceneComponent;
class UStaticMesh;

UENUM(BlueprintType)
enum class EParcelVisualisationMode : uint8
{
    Hidden UMETA(DisplayName = "Hidden"),
    All UMETA(DisplayName = "Show All"),
    RestrictedOnly UMETA(DisplayName = "Restricted / Review Only")
};

UENUM(BlueprintType)
enum class EParcelTopography : uint8
{
    Flat,
    Rolling,
    Hillside,
    Waterfront,
    MixedLandAndWater,
    Submerged
};

UENUM(BlueprintType)
enum class EParcelAvailability : uint8
{
    Sellable,
    ManualReview,
    Unavailable
};

USTRUCT(BlueprintType)
struct FGeneratedParcel
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Parcel")
    int32 AlongIndex = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Parcel")
    int32 DepthIndex = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Parcel")
    int32 Side = 1;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Parcel")
    FVector Centre = FVector::ZeroVector;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Parcel")
    float MinimumHeight = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Parcel")
    float MaximumHeight = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Parcel")
    float MaximumSlopeDegrees = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Parcel")
    float UnderwaterFraction = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Parcel")
    EParcelTopography Topography = EParcelTopography::Flat;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Parcel")
    EParcelAvailability Availability = EParcelAvailability::Sellable;
};

UCLASS()
class AVENOR_API AParcelGenerator : public AActor
{
    GENERATED_BODY()

public:
    AParcelGenerator();

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|Parcels")
    void RegenerateParcels();

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|Parcels")
    void CycleVisualisationMode();

    UFUNCTION(BlueprintCallable, Category = "Avenor|Parcels")
    void SetVisualisationMode(EParcelVisualisationMode NewMode);

    UFUNCTION(BlueprintPure, Category = "Avenor|Parcels")
    TArray<FGeneratedParcel> GetGeneratedParcels() const
    {
        return Parcels;
    }

protected:
    virtual void OnConstruction(const FTransform& Transform) override;

private:
    FGeneratedParcel AnalyseParcel(
        int32 AlongIndex,
        int32 DepthIndex,
        int32 Side
    ) const;
    void DrawParcel(const FGeneratedParcel& Parcel);
    void AddBoundaryEdge(
        UInstancedStaticMeshComponent* Component,
        const FVector2D& A,
        const FVector2D& B
    );
    FVector ToWorld(float Chainage, float Lateral, float Height) const;

    UPROPERTY(VisibleAnywhere, Category = "Avenor")
    TObjectPtr<USceneComponent> SceneRoot;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Generated")
    TObjectPtr<UInstancedStaticMeshComponent> SellableBoundaries;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Generated")
    TObjectPtr<UInstancedStaticMeshComponent> ReviewBoundaries;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Generated")
    TObjectPtr<UInstancedStaticMeshComponent> UnavailableBoundaries;

    UPROPERTY(EditInstanceOnly, Category = "Avenor|References")
    TObjectPtr<ASpineGenerator> Spine;

    UPROPERTY(EditInstanceOnly, Category = "Avenor|References")
    TObjectPtr<AProceduralTerrainGenerator> Terrain;

    UPROPERTY(EditAnywhere, Category = "Avenor|Display")
    EParcelVisualisationMode VisualisationMode =
        EParcelVisualisationMode::Hidden;

    UPROPERTY(EditAnywhere, Category = "Avenor|Display")
    bool bVisibleInGame = false;

    UPROPERTY(EditAnywhere, Category = "Avenor|Display",
        meta = (ClampMin = "2.0"))
    float BoundaryWidth = 20.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Display",
        meta = (ClampMin = "0.0"))
    float BoundaryHeightOffset = 30.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Display",
        meta = (ClampMin = "100.0"))
    float BoundarySegmentLength = 1000.0f;

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
    float CorridorHalfWidth = 3000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Analysis",
        meta = (ClampMin = "3", ClampMax = "17"))
    int32 SamplesPerSide = 7;

    UPROPERTY(EditAnywhere, Category = "Avenor|Analysis",
        meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float SubmergedThreshold = 0.5f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Analysis",
        meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float WaterfrontReviewThreshold = 0.02f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Analysis")
    float RollingSlopeDegrees = 5.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Analysis")
    float HillsideSlopeDegrees = 15.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Materials")
    TObjectPtr<UMaterialInterface> SellableMaterial;

    UPROPERTY(EditAnywhere, Category = "Avenor|Materials")
    TObjectPtr<UMaterialInterface> ReviewMaterial;

    UPROPERTY(EditAnywhere, Category = "Avenor|Materials")
    TObjectPtr<UMaterialInterface> UnavailableMaterial;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Generated")
    TArray<FGeneratedParcel> Parcels;

    UPROPERTY()
    TObjectPtr<UStaticMesh> CubeMesh;
};
