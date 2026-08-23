#include "AvenorStripTerrainGenerator.h"
#include "AvenorTerrainData.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "WaterBodyComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterSplineComponent.h"

namespace UE::Avenor::LakeDatumGuard
{
static constexpr float UpdateIntervalSeconds = 0.25f;
static const FString LakeLabelPrefix(TEXT("Avenor_Strip_Lake_"));

static FName MakeWaterOwnerTag(const AAvenorStripTerrainGenerator& Generator)
{
    return FName(*FString::Printf(
        TEXT("AvenorStripOwner_%s"),
        *Generator.GetFName().ToString()
    ));
}

static int32 LakeIndexFromLabel(const AWaterBodyLake& Lake)
{
#if WITH_EDITOR
    const FString Label = Lake.GetActorLabel();
    if (!Label.StartsWith(LakeLabelPrefix))
    {
        return INDEX_NONE;
    }
    const FString Suffix = Label.RightChop(LakeLabelPrefix.Len());
    if (Suffix.IsEmpty())
    {
        return INDEX_NONE;
    }
    const int32 OneBasedIndex = FCString::Atoi(*Suffix);
    return OneBasedIndex > 0 ? OneBasedIndex - 1 : INDEX_NONE;
#else
    return INDEX_NONE;
#endif
}

static void ApplyLakeDatum(
    AWaterBodyLake& Lake,
    const FAvenorBakedLakeBasin& Basin,
    double GeneratorWorldZ
)
{
    UWaterBodyComponent* WaterComponent = Lake.GetWaterBodyComponent();
    UWaterSplineComponent* Spline = WaterComponent
        ? WaterComponent->GetWaterSpline()
        : nullptr;
    if (!Spline || Basin.Shoreline.Num() < 3)
    {
        return;
    }

    const double ExpectedWorldZ = Basin.SurfaceHeight + GeneratorWorldZ;
    const double CurrentActorZ = Lake.GetActorLocation().Z;
    const double CurrentSplineZ = Spline->GetNumberOfSplinePoints() > 0
        ? Spline->GetLocationAtSplinePoint(
            0, ESplineCoordinateSpace::World
        ).Z
        : TNumericLimits<double>::Max();

    // WaterBodyLake treats the actor transform as the level-water datum and
    // can normalise a closed spline back onto that datum during its editor
    // synchronisation. Generated actors used to be spawned at identity (Z=0),
    // so valid elevated lake splines were subsequently flattened to world
    // zero. Only touch a lake when the actor or spline has actually drifted.
    if (FMath::IsNearlyEqual(CurrentActorZ, ExpectedWorldZ, 1.0)
        && FMath::IsNearlyEqual(CurrentSplineZ, ExpectedWorldZ, 1.0))
    {
        return;
    }

    Lake.Modify();
    Spline->Modify();

    FVector ActorLocation = Lake.GetActorLocation();
    ActorLocation.Z = ExpectedWorldZ;
    Lake.SetActorLocation(ActorLocation);

    TArray<FVector> WorldPoints = Basin.Shoreline;
    for (FVector& Point : WorldPoints)
    {
        Point.Z = ExpectedWorldZ;
    }
    Spline->SetSplinePoints(
        WorldPoints, ESplineCoordinateSpace::World, false
    );
    Spline->SetClosedLoop(true, false);
    for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
    {
        Spline->SetSplinePointType(Index, ESplinePointType::Curve, false);
        const int32 PreviousIndex =
            (Index - 1 + WorldPoints.Num()) % WorldPoints.Num();
        const int32 NextIndex = (Index + 1) % WorldPoints.Num();
        FVector Tangent =
            (WorldPoints[NextIndex] - WorldPoints[PreviousIndex]) * 0.5;
        Tangent.Z = 0.0;
        Spline->SetTangentsAtSplinePoint(
            Index,
            Tangent,
            Tangent,
            ESplineCoordinateSpace::World,
            false
        );
    }
    Spline->UpdateSpline();
    Spline->K2_SynchronizeAndBroadcastDataChange();
    Lake.PostEditChange();
}

class FGeneratedLakeDatumGuard
{
public:
    FGeneratedLakeDatumGuard()
    {
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateRaw(
                this, &FGeneratedLakeDatumGuard::Tick
            ),
            UpdateIntervalSeconds
        );
    }

    ~FGeneratedLakeDatumGuard()
    {
        if (TickHandle.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        }
    }

private:
    bool Tick(float)
    {
#if WITH_EDITOR
        if (!GEditor)
        {
            return true;
        }
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World)
        {
            return true;
        }

        for (TActorIterator<AAvenorStripTerrainGenerator> GeneratorIt(World);
             GeneratorIt; ++GeneratorIt)
        {
            AAvenorStripTerrainGenerator* Generator = *GeneratorIt;
            if (!Generator)
            {
                continue;
            }
            const UAvenorTerrainData* Data =
                Generator->BakedTerrainData.LoadSynchronous();
            if (!Data || !Data->HasValidData() || Data->Lakes.IsEmpty())
            {
                continue;
            }

            const FName OwnerTag = MakeWaterOwnerTag(*Generator);
            for (TActorIterator<AWaterBodyLake> LakeIt(World); LakeIt; ++LakeIt)
            {
                AWaterBodyLake* Lake = *LakeIt;
                if (!Lake || !Lake->Tags.Contains(OwnerTag))
                {
                    continue;
                }
                const int32 LakeIndex = LakeIndexFromLabel(*Lake);
                if (!Data->Lakes.IsValidIndex(LakeIndex))
                {
                    continue;
                }
                ApplyLakeDatum(
                    *Lake,
                    Data->Lakes[LakeIndex],
                    Generator->GetActorLocation().Z
                );
            }
        }
#endif
        return true;
    }

    FTSTicker::FDelegateHandle TickHandle;
};

static FGeneratedLakeDatumGuard GGeneratedLakeDatumGuard;
} // namespace UE::Avenor::LakeDatumGuard
