#include "AvenorHydrologyGenerator.h"

#include "AvenorTerrainRefinementModifier.h"
#include "ActorFactories/ActorFactory.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "MeshPartition.h"
#include "MeshPartitionModifierComponent.h"
#include "Misc/ScopedSlowTask.h"
#include "UObject/UObjectGlobals.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterSplineComponent.h"

namespace UE::Avenor::Hydrology
{
static const FName GeneratedWaterTag(TEXT("AvenorGeneratedWater"));

template<typename TWaterBodyActor>
static TWaterBodyActor* CreateWaterBody(
    UWorld* World,
    UE::MeshPartition::AMeshPartition* MeshPartition,
    const FString& Label,
    const TCHAR* ModifierClassPath,
    double FalloffWidth,
    UE::MeshPartition::UModifierComponent*& OutLastModifier
)
{
    UActorFactory* Factory =
        GEditor->FindActorFactoryForActorClass(
            TWaterBodyActor::StaticClass()
        );
    if (!Factory)
    {
        return nullptr;
    }

    TWaterBodyActor* WaterBody = Cast<TWaterBodyActor>(
        Factory->CreateActor(
            TWaterBodyActor::StaticClass(),
            World->GetCurrentLevel(),
            FTransform::Identity
        )
    );
    if (!WaterBody)
    {
        return nullptr;
    }

    WaterBody->SetActorLabel(Label);
    WaterBody->SetFolderPath(TEXT("Avenor/Generated/Water"));
    WaterBody->Tags.AddUnique(GeneratedWaterTag);

    UWaterBodyComponent* WaterComponent =
        WaterBody->GetWaterBodyComponent();
    WaterComponent->WaterHeightmapSettings.FalloffSettings.FalloffMode =
        EWaterBrushFalloffMode::Width;
    WaterComponent->WaterHeightmapSettings.FalloffSettings.FalloffWidth =
        FalloffWidth;

    UClass* ModifierClass =
        LoadClass<UE::MeshPartition::UModifierComponent>(
            nullptr,
            ModifierClassPath
        );
    if (!ModifierClass)
    {
        World->EditorDestroyActor(WaterBody, true);
        return nullptr;
    }

    UE::MeshPartition::UModifierComponent* Modifier =
        NewObject<UE::MeshPartition::UModifierComponent>(
            WaterBody,
            ModifierClass,
            ModifierClass->GetFName()
        );
    WaterBody->AddInstanceComponent(Modifier);
    Modifier->AttachToComponent(
        WaterComponent,
        FAttachmentTransformRules::KeepWorldTransform
    );
    Modifier->RegisterComponent();

    // Do not notify here. All water modifiers are registered in one batch at
    // the end of regeneration.
    Modifier->SetAffectedMeshPartition(MeshPartition);
    OutLastModifier = Modifier;
    return WaterBody;
}

static void ConfigureWaterSpline(
    UWaterSplineComponent& Spline,
    const TArray<FVector>& Points,
    bool bClosedLoop
)
{
    Spline.SetSplinePoints(
        Points,
        ESplineCoordinateSpace::World,
        false
    );
    Spline.SetClosedLoop(bClosedLoop, false);
    for (int32 Index = 0;
         Index < Spline.GetNumberOfSplinePoints();
         ++Index)
    {
        Spline.SetSplinePointType(
            Index,
            ESplinePointType::CurveClamped,
            false
        );
    }
    Spline.UpdateSpline();
}
} // namespace UE::Avenor::Hydrology

using namespace UE::Avenor::Hydrology;

AAvenorHydrologyGenerator::AAvenorHydrologyGenerator()
{
    PrimaryActorTick.bCanEverTick = false;
    SetIsSpatiallyLoaded(false);
}

UAvenorTerrainRefinementModifier*
AAvenorHydrologyGenerator::ResolveRefinementModifier() const
{
    return RefinementModifierActor
        ? RefinementModifierActor->FindComponentByClass<
            UAvenorTerrainRefinementModifier>()
        : nullptr;
}

void AAvenorHydrologyGenerator::ClearGeneratedHydrology()
{
#if WITH_EDITOR
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    TArray<AWaterBody*> ToDelete;
    for (TActorIterator<AWaterBody> It(World); It; ++It)
    {
        if (It->Tags.Contains(GeneratedWaterTag))
        {
            ToDelete.Add(*It);
        }
    }

    // Detach generated modifiers before deleting their actors. This prevents
    // one expensive Mesh Partition rebuild notification per Water Body.
    for (AWaterBody* WaterBody : ToDelete)
    {
        TInlineComponentArray<
            UE::MeshPartition::UModifierComponent*
        > Modifiers;
        WaterBody->GetComponents(Modifiers);
        for (UE::MeshPartition::UModifierComponent* Modifier : Modifiers)
        {
            Modifier->SetAffectedMeshPartition(nullptr);
        }
    }
    for (AWaterBody* WaterBody : ToDelete)
    {
        World->EditorDestroyActor(WaterBody, true);
    }
#endif
}

void AAvenorHydrologyGenerator::RegenerateHydrology()
{
#if WITH_EDITOR
    UAvenorTerrainRefinementModifier* Refinement =
        ResolveRefinementModifier();
    UE::MeshPartition::AMeshPartition* MeshPartition =
        Cast<UE::MeshPartition::AMeshPartition>(MeshPartitionActor);
    UWorld* World = GetWorld();
    if (!Refinement || !MeshPartition || !World)
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT(
                "Avenor hydrology requires Refinement Modifier Actor and "
                "Mesh Partition Actor references."
            )
        );
        return;
    }

    FScopedSlowTask Progress(
        3.0f,
        FText::FromString(TEXT("Creating Avenor water bodies..."))
    );
    Progress.MakeDialog();

    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Reading refined drainage network"))
    );
    TArray<FAvenorRiverReach> Rivers;
    TArray<FAvenorLakeBasin> Lakes;
    if (!Refinement->GetHydrologyFeatures(
            MaximumRiverReaches,
            MaximumLakes,
            RiverSurfaceInset,
            Rivers,
            Lakes
        ))
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("Avenor refinement hydrology analysis is unavailable.")
        );
        return;
    }

    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Removing previous water actors"))
    );
    ClearGeneratedHydrology();

    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Creating native UE Water actors"))
    );
    UE::MeshPartition::UModifierComponent* LastCreatedModifier = nullptr;
    int32 CreatedLakes = 0;
    int32 CreatedRivers = 0;

    for (int32 LakeIndex = 0; LakeIndex < Lakes.Num(); ++LakeIndex)
    {
        const FAvenorLakeBasin& Feature = Lakes[LakeIndex];
        if (Feature.Shoreline.Num() < 4)
        {
            continue;
        }
        AWaterBodyLake* Lake = CreateWaterBody<AWaterBodyLake>(
            World,
            MeshPartition,
            FString::Printf(TEXT("Avenor_Lake_%02d"), LakeIndex + 1),
            TEXT("/Script/MeshPartitionWater.LakeModifier"),
            LakeFalloffWidth,
            LastCreatedModifier
        );
        if (!Lake)
        {
            continue;
        }
        ConfigureWaterSpline(
            *Lake->GetWaterBodyComponent()->GetWaterSpline(),
            Feature.Shoreline,
            true
        );
        Lake->PostEditChange();
        ++CreatedLakes;
    }

    for (int32 RiverIndex = 0;
         RiverIndex < Rivers.Num();
         ++RiverIndex)
    {
        const FAvenorRiverReach& Feature = Rivers[RiverIndex];
        if (Feature.Points.Num() < 2)
        {
            continue;
        }
        AWaterBodyRiver* River = CreateWaterBody<AWaterBodyRiver>(
            World,
            MeshPartition,
            FString::Printf(TEXT("Avenor_River_%03d"), RiverIndex + 1),
            TEXT("/Script/MeshPartitionWater.RiverModifier"),
            RiverFalloffWidth,
            LastCreatedModifier
        );
        if (!River)
        {
            continue;
        }
        ConfigureWaterSpline(
            *River->GetWaterBodyComponent()->GetWaterSpline(),
            Feature.Points,
            false
        );
        River->PostEditChange();
        ++CreatedRivers;
    }

    if (LastCreatedModifier)
    {
        // One public registration call scans and registers every modifier
        // created above.
        LastCreatedModifier->SetAffectedMeshPartition(nullptr);
        LastCreatedModifier->BP_SetAffectedMegaMesh(MeshPartition);
    }

    bool bHasWaterZone = false;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (It->GetClass()->GetName() == TEXT("WaterZone"))
        {
            bHasWaterZone = true;
            break;
        }
    }
    if (!bHasWaterZone)
    {
        UE_LOG(
            LogTemp,
            Warning,
            TEXT(
                "Generated Water Bodies require a Water Zone to render."
            )
        );
    }

    UE_LOG(
        LogTemp,
        Display,
        TEXT(
            "Avenor water adapter created %d lakes and %d river reaches "
            "from the refinement modifier's drainage graph."
        ),
        CreatedLakes,
        CreatedRivers
    );
#endif
}
