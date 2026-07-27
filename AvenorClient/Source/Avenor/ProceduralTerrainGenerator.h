#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralTerrainGenerator.generated.h"

class ASpineGenerator;
class UMaterialInterface;
class UProceduralMeshComponent;
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

protected:
    virtual void OnConstruction(const FTransform& Transform) override;

private:
    float EvaluateBaseHeight(float Chainage, float Lateral) const;
    float EvaluateTerrainHeight(float Chainage, float Lateral) const;
    FVector SpineSpaceToWorld(float Chainage, float Lateral, float Height) const;
    void GenerateWatercourses();
    void BuildTerrainMesh();
    void BuildWaterMesh();
    float DistanceToSegment(
        const FVector2D& Point,
        const FVector2D& A,
        const FVector2D& B,
        float& OutAlpha
    ) const;

    UPROPERTY(VisibleAnywhere, Category = "Avenor")
    TObjectPtr<USceneComponent> SceneRoot;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Generated")
    TObjectPtr<UProceduralMeshComponent> GeneratedMesh;

    UPROPERTY(EditInstanceOnly, Category = "Avenor|Terrain")
    TObjectPtr<ASpineGenerator> Spine;

    UPROPERTY(EditAnywhere, Category = "Avenor|Generation")
    int32 WorldSeed = 1847;

    UPROPERTY(EditAnywhere, Category = "Avenor|Generation")
    bool bRegenerateOnConstruction = true;

    // Dimensions in centimetres.
    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "100000.0"))
    float Length = 1200000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "100000.0"))
    float HalfWidth = 1500000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "1000.0"))
    float CellSize = 10000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Lowlands",
        meta = (ClampMin = "0.0"))
    float LowlandCoreDistance = 100000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Lowlands",
        meta = (ClampMin = "1000.0"))
    float LowlandTransitionEnd = 500000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Lowlands",
        meta = (ClampMin = "0.0"))
    float LowlandRelief = 1800.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hills",
        meta = (ClampMin = "0.0"))
    float HillRelief = 9000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Mountains",
        meta = (ClampMin = "0"))
    int32 MountainRegionCount = 2;

    UPROPERTY(EditAnywhere, Category = "Avenor|Mountains",
        meta = (ClampMin = "100000.0"))
    float MountainMinimumDistance = 1000000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Mountains",
        meta = (ClampMin = "1000.0"))
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
