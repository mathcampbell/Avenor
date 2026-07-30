#pragma once

#include "CoreMinimal.h"
#include "MeshPartitionModifierComponent.h"
#include "AvenorTerrainModifier.generated.h"

class ASpineGenerator;

/**
 * Deterministic base-form modifier for Avenor's Mesh Terrain.
 *
 * Distance from the Spine controls permitted roughness, not elevation:
 * broad regional height continues through the corridor while hills, deep
 * valleys and mountains are suppressed near it. Lakes and their outlet
 * channels are generated as connected deterministic terrain features.
 */
UCLASS(
    PrioritizeCategories = (
        "Modifier",
        "Avenor",
        "Spine Corridor",
        "Regional Terrain",
        "Mountains",
        "Hydrology"
    ),
    meta = (BlueprintSpawnableComponent)
)
class AVENOREDITOR_API UAvenorTerrainModifier
    : public UE::MeshPartition::UModifierComponent
{
    GENERATED_BODY()

public:
    UAvenorTerrainModifier();

    UFUNCTION(BlueprintPure, Category = "Avenor|Terrain")
    double EvaluateBaseHeightAtWorldPosition(
        const FVector2D& WorldPosition
    ) const;

    UFUNCTION(BlueprintPure, Category = "Avenor|Terrain")
    FBox GetTerrainWorldBounds() const;

    virtual TArray<FBox> ComputeBounds() const override;
    virtual TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
        CreateBackgroundOp(
            UE::MeshPartition::EBuildType InBuildType
        ) const override;
    virtual FGuid GetCodeVersionKey() const override;

private:
    UPROPERTY(EditAnywhere, Category = "Avenor")
    TObjectPtr<ASpineGenerator> Spine;

    // World-space coverage before applying this component's transform.
    UPROPERTY(EditAnywhere, Category = "Avenor",
        meta = (ClampMin = "1000.0", AllowPreserveRatio = true))
    FVector3d UnscaledCoverage =
        FVector3d(1200000.0, 3000000.0, 2000000.0);

    UPROPERTY(EditAnywhere, Category = "Avenor")
    int32 WorldSeed = 1847;

    UPROPERTY(EditAnywhere, Category = "Spine Corridor",
        meta = (ClampMin = "1000.0"))
    double SpineSampleSpacing = 10000.0;

    // Inside this distance, only gentle relief is permitted.
    UPROPERTY(EditAnywhere, Category = "Spine Corridor",
        meta = (ClampMin = "0.0"))
    double GentleCorridorHalfWidth = 100000.0;

    // Roughness reaches its unrestricted value here. This does not add height.
    UPROPERTY(EditAnywhere, Category = "Spine Corridor",
        meta = (ClampMin = "0.0"))
    double FullRoughnessDistance = 500000.0;

    UPROPERTY(EditAnywhere, Category = "Spine Corridor",
        meta = (ClampMin = "0.0", ClampMax = "1.0"))
    double CorridorRoughnessFraction = 0.12;

    UPROPERTY(EditAnywhere, Category = "Regional Terrain",
        meta = (ClampMin = "1000.0"))
    double RegionalScale = 600000.0;

    UPROPERTY(EditAnywhere, Category = "Regional Terrain",
        meta = (ClampMin = "0.0"))
    double RegionalRelief = 4000.0;

    UPROPERTY(EditAnywhere, Category = "Regional Terrain",
        meta = (ClampMin = "1000.0"))
    double HillScale = 180000.0;

    UPROPERTY(EditAnywhere, Category = "Regional Terrain",
        meta = (ClampMin = "0.0"))
    double HillRelief = 18000.0;

    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "0", ClampMax = "16"))
    int32 MountainRegionCount = 2;

    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "0.0"))
    double MountainMinimumSpineDistance = 1000000.0;

    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "10000.0"))
    double MountainRadius = 600000.0;

    UPROPERTY(EditAnywhere, Category = "Mountains",
        meta = (ClampMin = "0.0"))
    double MountainRelief = 80000.0;

    UPROPERTY(EditAnywhere, Category = "Hydrology",
        meta = (ClampMin = "0", ClampMax = "16"))
    int32 LakeCount = 3;

    UPROPERTY(EditAnywhere, Category = "Hydrology",
        meta = (ClampMin = "1000.0"))
    double LakeRadius = 60000.0;

    UPROPERTY(EditAnywhere, Category = "Hydrology",
        meta = (ClampMin = "0.0"))
    double LakeDepth = 2500.0;

    UPROPERTY(EditAnywhere, Category = "Hydrology",
        meta = (ClampMin = "100.0"))
    double RiverWidth = 3500.0;

    UPROPERTY(EditAnywhere, Category = "Hydrology",
        meta = (ClampMin = "0.0"))
    double RiverDepth = 1200.0;

    UPROPERTY(EditAnywhere, Category = "Hydrology",
        meta = (ClampMin = "0.0"))
    double RiverMeander = 70000.0;

    // Minimum fall in centimetres per kilometre along an outlet.
    UPROPERTY(EditAnywhere, Category = "Hydrology",
        meta = (ClampMin = "0.0"))
    double RiverFallPerKilometre = 120.0;

    // Every Nth outlet deliberately crosses the Spine; zero disables this.
    UPROPERTY(EditAnywhere, Category = "Hydrology",
        meta = (ClampMin = "0", ClampMax = "16"))
    int32 SpineCrossingEvery = 2;
};
