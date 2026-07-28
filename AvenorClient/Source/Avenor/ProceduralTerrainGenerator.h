#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralTerrainGenerator.generated.h"

class ALandscape;
class ASpineGenerator;
class AWaterBody;
class AWaterZone;
class UBoxComponent;
class UMaterialInterface;
class UPCGComponent;
class UPCGGraphInterface;
class USceneComponent;

USTRUCT(BlueprintType)
struct FGeneratedWatercourse
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Water")
    TArray<FVector2D> SpineSpacePoints;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Water")
    TArray<float> SurfaceHeights;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Water")
    FVector2D LakeCentre = FVector2D::ZeroVector;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Water")
    float LakeRadius = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Water")
    float LakeSurfaceHeight = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Water")
    float Width = 0.0f;
};

/**
 * Generates Avenor's authored world form as a native Unreal Landscape.
 *
 * The class name is retained so existing level references and parcel queries
 * survive the migration from UProceduralMeshComponent. Regeneration is an
 * editor operation; the resulting ALandscape is saved with the map and cooks
 * like any manually-created Landscape.
 */
UCLASS()
class AVENOR_API AProceduralTerrainGenerator : public AActor
{
    GENERATED_BODY()

public:
    AProceduralTerrainGenerator();

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|Terrain")
    void Regenerate();

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|Terrain")
    void ClearGeneratedTerrain();

    UFUNCTION(BlueprintPure, Category = "Avenor|Terrain")
    float GetTerrainHeightAtSpineSpace(
        float Chainage,
        float Lateral
    ) const;

    UFUNCTION(BlueprintPure, Category = "Avenor|Terrain")
    bool GetWaterSurfaceAtSpineSpace(
        float Chainage,
        float Lateral,
        float& OutSurfaceHeight
    ) const;

    UFUNCTION(BlueprintPure, Category = "Avenor|Terrain")
    bool IsUnderwaterAtSpineSpace(
        float Chainage,
        float Lateral,
        float& OutDepth
    ) const;

    UFUNCTION(BlueprintPure, Category = "Avenor|Terrain")
    ALandscape* GetGeneratedLandscape() const
    {
        return GeneratedLandscape;
    }

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|PCG")
    void RegenerateVegetation();

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|PCG")
    void ClearVegetation();

protected:
    virtual void OnConstruction(const FTransform& Transform) override;

private:
    float EvaluateBaseHeight(float Chainage, float Lateral) const;
    float EvaluateTerrainHeight(float Chainage, float Lateral) const;
    FVector SpineSpaceToWorld(
        float Chainage,
        float Lateral,
        float Height
    ) const;
    void WorldToSpineSpace(
        const FVector& WorldLocation,
        float& OutChainage,
        float& OutLateral
    ) const;
    void GenerateWatercourses();
    void BuildLandscape();
    void BuildNativeWater();
    float DistanceToSegment(
        const FVector2D& Point,
        const FVector2D& A,
        const FVector2D& B,
        float& OutAlpha
    ) const;

    UPROPERTY(VisibleAnywhere, Category = "Avenor")
    TObjectPtr<USceneComponent> SceneRoot;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UBoxComponent> PCGBounds;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UPCGComponent> VegetationPCG;

    UPROPERTY(VisibleInstanceOnly, Category = "Avenor|Generated")
    TObjectPtr<ALandscape> GeneratedLandscape;

    UPROPERTY(VisibleInstanceOnly, Category = "Avenor|Generated")
    TObjectPtr<AWaterZone> GeneratedWaterZone;

    UPROPERTY(VisibleInstanceOnly, Category = "Avenor|Generated")
    TArray<TObjectPtr<AWaterBody>> GeneratedWaterBodies;

    UPROPERTY(EditInstanceOnly, Category = "Avenor|Terrain")
    TObjectPtr<ASpineGenerator> Spine;

    UPROPERTY(EditAnywhere, Category = "Avenor|Generation")
    int32 WorldSeed = 1847;

    // Large native Landscapes should be rebuilt explicitly with Regenerate.
    UPROPERTY(EditAnywhere, Category = "Avenor|Generation")
    bool bRegenerateOnConstruction = false;

    UPROPERTY(EditAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UPCGGraphInterface> VegetationGraph;

    UPROPERTY(EditAnywhere, Category = "Avenor|PCG")
    bool bRegenerateVegetationWithLandscape = true;

    // Requested dimensions in centimetres. Actual dimensions are rounded up
    // to whole Landscape components.
    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "100000.0"))
    float Length = 1200000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "100000.0"))
    float HalfWidth = 1500000.0f;

    // Native Landscape vertex spacing. 2000 cm gives a useful 20 m world-form
    // grid; roads, banks and plots can add local detail without a huge base.
    UPROPERTY(EditAnywhere, Category = "Avenor|Landscape",
        meta = (ClampMin = "100.0"))
    float LandscapeVertexSpacing = 2000.0f;

    // 63 quads is the standard one-section Landscape component size.
    UPROPERTY(EditAnywhere, Category = "Avenor|Landscape",
        meta = (ClampMin = "7", ClampMax = "255"))
    int32 LandscapeQuadsPerSection = 63;

    // 400 supports approximately +/- 1,024 metres of vertical relief.
    UPROPERTY(EditAnywhere, Category = "Avenor|Landscape",
        meta = (ClampMin = "25.0"))
    float LandscapeZScale = 400.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Lowlands",
        meta = (ClampMin = "0.0"))
    float LowlandCoreDistance = 100000.0f;

    // Keep the infrastructure itself exactly on the Spine datum, then blend
    // gradually into the natural lowland relief.
    UPROPERTY(EditAnywhere, Category = "Avenor|Lowlands",
        meta = (ClampMin = "0.0"))
    float SpineLevelHalfWidth = 3000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Lowlands",
        meta = (ClampMin = "1.0"))
    float SpineLevelTransitionEnd = 100000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Lowlands",
        meta = (ClampMin = "1000.0"))
    float LowlandTransitionEnd = 500000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Lowlands",
        meta = (ClampMin = "0.0"))
    float LowlandRelief = 3000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hills",
        meta = (ClampMin = "0.0"))
    float HillRelief = 18000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Mountains",
        meta = (ClampMin = "0"))
    int32 MountainRegionCount = 2;

    UPROPERTY(EditAnywhere, Category = "Avenor|Mountains",
        meta = (ClampMin = "100000.0"))
    float MountainMinimumDistance = 1000000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Mountains",
        meta = (ClampMin = "0.0"))
    float MountainRelief = 80000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Water",
        meta = (ClampMin = "0", ClampMax = "8"))
    int32 RiverCount = 2;

    UPROPERTY(EditAnywhere, Category = "Avenor|Water",
        meta = (ClampMin = "8", ClampMax = "128"))
    int32 RiverControlPointCount = 48;

    UPROPERTY(EditAnywhere, Category = "Avenor|Water",
        meta = (ClampMin = "500.0"))
    float RiverBaseWidth = 3500.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Water",
        meta = (ClampMin = "100.0"))
    float RiverCarveDepth = 1200.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Water")
    bool bGenerateNativeWater = true;

    UPROPERTY(EditAnywhere, Category = "Avenor|Materials")
    TObjectPtr<UMaterialInterface> TerrainMaterial;

    UPROPERTY(EditAnywhere, Category = "Avenor|Materials")
    TObjectPtr<UMaterialInterface> WaterMaterial;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Generated")
    TArray<FGeneratedWatercourse> Watercourses;

    TArray<FVector2D> MountainCentres;
    TArray<float> MountainRadii;
    TArray<float> MountainHeights;
};
