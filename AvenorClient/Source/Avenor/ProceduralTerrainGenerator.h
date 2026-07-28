#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralTerrainGenerator.generated.h"

class ALandscape;
class ASpineGenerator;
class AWaterBody;
class UPCGComponent;
class UPCGGraphInterface;
class UBoxComponent;
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
 * Authoritative, deterministic world definition for Avenor.
 *
 * The legacy class name is retained so existing maps and ParcelGenerator
 * references keep loading. This actor no longer creates, imports or deletes an
 * ALandscape, Water Zone or Water Body. It owns rules and PCG entry points;
 * Unreal's native Landscape and Water systems own the actual world actors.
 */
UCLASS()
class AVENOR_API AProceduralTerrainGenerator : public AActor
{
    GENERATED_BODY()

public:
    AProceduralTerrainGenerator();

    /** Rebuild deterministic rule data, then run each assigned PCG graph. */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|World")
    void Regenerate();

    /**
     * Compatibility button. It cleans this actor's PCG output only; it never
     * destroys the referenced Landscape or authored Water Bodies.
     */
    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|World")
    void ClearGeneratedTerrain();

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|PCG")
    void RegenerateTerrain();

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|PCG")
    void RegenerateWater();

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|PCG")
    void RegenerateVegetation();

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|PCG")
    void RegenerateInfrastructure();

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|PCG")
    void ClearPCG();

    UFUNCTION(CallInEditor, BlueprintCallable, Category = "Avenor|PCG")
    void ClearVegetation();

    UFUNCTION(BlueprintPure, Category = "Avenor|Terrain")
    float GetTerrainHeightAtSpineSpace(float Chainage, float Lateral) const;

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
    ALandscape* GetGeneratedLandscape() const { return NativeLandscape; }

    UFUNCTION(BlueprintPure, Category = "Avenor|World")
    int32 GetWorldSeed() const { return WorldSeed; }

    UFUNCTION(BlueprintPure, Category = "Avenor|World")
    TArray<FGeneratedWatercourse> GetWatercourses() const
    {
        return Watercourses;
    }

private:
    float EvaluateBaseHeight(float Chainage, float Lateral) const;
    float EvaluateTerrainHeight(float Chainage, float Lateral) const;
    float DistanceToSegment(
        const FVector2D& Point,
        const FVector2D& A,
        const FVector2D& B,
        float& OutAlpha
    ) const;
    void GenerateWorldRules();
    void GenerateWatercourses();
    void ConfigurePCGComponent(
        UPCGComponent* Component,
        UPCGGraphInterface* Graph
    );
    void GeneratePCGComponent(
        UPCGComponent* Component,
        UPCGGraphInterface* Graph,
        const TCHAR* DisplayName
    );

    UPROPERTY(VisibleAnywhere, Category = "Avenor")
    TObjectPtr<USceneComponent> SceneRoot;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UBoxComponent> PCGBounds;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UPCGComponent> TerrainPCG;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UPCGComponent> WaterPCG;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UPCGComponent> VegetationPCG;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UPCGComponent> InfrastructurePCG;

    // Create one native Landscape in the editor and assign it here. It is
    // never spawned, moved or destroyed by this actor.
    UPROPERTY(EditInstanceOnly, Category = "Avenor|References")
    TObjectPtr<ALandscape> NativeLandscape;

    UPROPERTY(EditInstanceOnly, Category = "Avenor|References")
    TObjectPtr<ASpineGenerator> Spine;

    // Optional references to native Water Bodies produced/authored for this
    // definition. They remain owned by the level and Water system.
    UPROPERTY(EditInstanceOnly, Category = "Avenor|References")
    TArray<TObjectPtr<AWaterBody>> NativeWaterBodies;

    UPROPERTY(EditAnywhere, Category = "Avenor|Generation")
    int32 WorldSeed = 1847;

    UPROPERTY(EditAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UPCGGraphInterface> TerrainGraph;

    UPROPERTY(EditAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UPCGGraphInterface> WaterGraph;

    UPROPERTY(EditAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UPCGGraphInterface> VegetationGraph;

    UPROPERTY(EditAnywhere, Category = "Avenor|PCG")
    TObjectPtr<UPCGGraphInterface> InfrastructureGraph;

    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "100000.0"))
    float Length = 1200000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Extent",
        meta = (ClampMin = "100000.0"))
    float HalfWidth = 1500000.0f;

    UPROPERTY(EditAnywhere, Category = "Avenor|Lowlands",
        meta = (ClampMin = "0.0"))
    float LowlandCoreDistance = 100000.0f;

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

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Avenor|Generated",
        meta = (AllowPrivateAccess = "true"))
    TArray<FGeneratedWatercourse> Watercourses;

    TArray<FVector2D> MountainCentres;
    TArray<float> MountainRadii;
    TArray<float> MountainHeights;
};
