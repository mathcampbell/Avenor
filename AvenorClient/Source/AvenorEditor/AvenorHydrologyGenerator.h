#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AvenorHydrologyGenerator.generated.h"

class UAvenorTerrainModifier;

/**
 * Editor-only deterministic drainage solver and native UE Water generator.
 */
UCLASS()
class AVENOREDITOR_API AAvenorHydrologyGenerator : public AActor
{
    GENERATED_BODY()

public:
    AAvenorHydrologyGenerator();

    UFUNCTION(CallInEditor, Category = "Avenor|Hydrology")
    void RegenerateHydrology();

    UFUNCTION(CallInEditor, Category = "Avenor|Hydrology")
    void ClearGeneratedHydrology();

private:
    UAvenorTerrainModifier* ResolveTerrainModifier() const;

    UPROPERTY(EditInstanceOnly, Category = "Avenor|References")
    TObjectPtr<AActor> TerrainModifierActor;

    UPROPERTY(EditInstanceOnly, Category = "Avenor|References")
    TObjectPtr<AActor> MeshPartitionActor;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology",
        meta = (ClampMin = "2500.0", ClampMax = "50000.0"))
    double HydrologyCellSize = 10000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology",
        meta = (ClampMin = "0", ClampMax = "16"))
    int32 LakeCount = 3;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology",
        meta = (ClampMin = "5000.0"))
    double LakeRadius = 60000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology",
        meta = (ClampMin = "0.0"))
    double MinimumLakeFillDepth = 1000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology",
        meta = (ClampMin = "100.0"))
    double RiverBedDepth = 2500.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology",
        meta = (ClampMin = "1", ClampMax = "16"))
    int32 RiverSplineStride = 2;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology",
        meta = (ClampMin = "0.01"))
    double DrainageEpsilon = 1.0;
};
