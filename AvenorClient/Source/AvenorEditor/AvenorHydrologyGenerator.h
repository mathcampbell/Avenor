#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AvenorHydrologyGenerator.generated.h"

class UAvenorTerrainRefinementModifier;

/**
 * Editor-only adapter from refined hydrology data to native UE Water actors.
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
    UAvenorTerrainRefinementModifier*
        ResolveRefinementModifier() const;

    UPROPERTY(EditInstanceOnly, Category = "Avenor|References")
    TObjectPtr<AActor> RefinementModifierActor;

    UPROPERTY(EditInstanceOnly, Category = "Avenor|References")
    TObjectPtr<AActor> MeshPartitionActor;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology",
        meta = (ClampMin = "0", ClampMax = "256"))
    int32 MaximumRiverReaches = 64;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology",
        meta = (ClampMin = "0", ClampMax = "32"))
    int32 MaximumLakes = 8;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology",
        meta = (ClampMin = "1.0"))
    double RiverSurfaceInset = 100.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology",
        meta = (ClampMin = "0.0"))
    double LakeFalloffWidth = 30000.0;

    UPROPERTY(EditAnywhere, Category = "Avenor|Hydrology",
        meta = (ClampMin = "0.0"))
    double RiverFalloffWidth = 10000.0;
};
