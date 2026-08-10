#pragma once

#include "CoreMinimal.h"
#include "MeshPartitionModifierComponent.h"

#include "AvenorSpineTerrainModifier.generated.h"

class ASpineGenerator;

/** Editor-only Mesh Terrain grading component for the persistent Spine data. */
UCLASS(ClassGroup = (Avenor))
class AVENOREDITOR_API UAvenorSpineTerrainModifier
    : public UE::MeshPartition::UModifierComponent
{
    GENERATED_BODY()

public:
    static bool BindToSpine(ASpineGenerator* Spine);
    static void ClearFromSpine(ASpineGenerator* Spine);

    virtual TArray<FBox> ComputeBounds() const override;
    virtual TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
        CreateBackgroundOp(UE::MeshPartition::EBuildType BuildType) const override;
    virtual FGuid GetCodeVersionKey() const override;
};
