#include "SpineGenerator.h"

#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Engine/World.h"
#include "MeshPartition.h"
#include "MeshPartitionMeshView.h"
#include "PCGComponent.h"
#include "PCGGraph.h"

namespace UE::Avenor::Spine
{
static const FName SpineExclusionChannel(TEXT("SpineExclusion"));

struct FCorridorSample
{
    FVector2D Centre = FVector2D::ZeroVector;
    double RoadDatumZ = 0.0;
};

class FSpineTerrainCorridorOp final
    : public UE::MeshPartition::IModifierBackgroundOp
{
public:
    explicit FSpineTerrainCorridorOp(FName Name)
        : IModifierBackgroundOp(Name)
    {
    }

    virtual void GetInstancesInBounds(
        const FBox& InBounds,
        TArray<FInstanceInfo>& OutInstances
    ) const override
    {
        if (!WorldBounds.Intersect(InBounds) || Samples.Num() < 2)
        {
            return;
        }

        FInstanceInfo& Instance = OutInstances.AddDefaulted_GetRef();
        Instance.InstanceID = 0;
        Instance.Bounds = WorldBounds;
        Instance.ReadViewComponents =
            UE::MeshPartition::EMeshViewComponents::VertexPos;
        Instance.WriteViewComponents = static_cast<
            UE::MeshPartition::EMeshViewComponents>(
                UE::MeshPartition::EMeshViewComponents::VertexPos |
                UE::MeshPartition::EMeshViewComponents::VertexAttributeWeight
            );
        Instance.UsedChannels = {SpineExclusionChannel};
    }

    virtual void ApplyModifications(
        UE::MeshPartition::FMeshView& MeshView,
        const FTransform3d& MeshTransform,
        const FInstanceInfo& InstanceInfo
    ) const override
    {
        (void)InstanceInfo;
        if (Samples.Num() < 2 || TransitionHalfWidth <= 0.0)
        {
            return;
        }

        for (int32 Vertex = 0; Vertex < MeshView.VertexCount(); ++Vertex)
        {
            FVector3d WorldPosition = MeshTransform.TransformPosition(
                MeshView.GetVertexPos(Vertex)
            );
            const FVector2D Position(WorldPosition.X, WorldPosition.Y);

            double BestDistanceSquared = TNumericLimits<double>::Max();
            double TargetRoadZ = WorldPosition.Z;
            for (int32 Index = 0; Index + 1 < Samples.Num(); ++Index)
            {
                const FVector2D A = Samples[Index].Centre;
                const FVector2D B = Samples[Index + 1].Centre;
                const FVector2D Segment = B - A;
                const double LengthSquared = Segment.SizeSquared();
                const double Alpha = LengthSquared > UE_DOUBLE_SMALL_NUMBER
                    ? FMath::Clamp(
                        FVector2D::DotProduct(Position - A, Segment) /
                            LengthSquared,
                        0.0,
                        1.0
                    )
                    : 0.0;
                const double DistanceSquared = (
                    Position - (A + Segment * Alpha)
                ).SizeSquared();
                if (DistanceSquared < BestDistanceSquared)
                {
                    BestDistanceSquared = DistanceSquared;
                    TargetRoadZ = FMath::Lerp(
                        Samples[Index].RoadDatumZ,
                        Samples[Index + 1].RoadDatumZ,
                        Alpha
                    );
                }
            }

            const double Distance = FMath::Sqrt(BestDistanceSquared);
            if (Distance >= TransitionHalfWidth)
            {
                MeshView.SetVertexAttributeWeight(
                    SpineExclusionChannel,
                    Vertex,
                    0.0f
                );
                continue;
            }

            const double Blend = Distance <= FlatHalfWidth
                ? 1.0
                : 1.0 - FMath::SmoothStep(
                    FlatHalfWidth,
                    TransitionHalfWidth,
                    Distance
                );
            WorldPosition.Z = FMath::Lerp(
                WorldPosition.Z,
                TargetRoadZ,
                Blend
            );
            MeshView.SetVertexPos(
                Vertex,
                MeshTransform.InverseTransformPosition(WorldPosition)
            );
            MeshView.SetVertexAttributeWeight(
                SpineExclusionChannel,
                Vertex,
                static_cast<float>(Blend)
            );
        }
    }

    virtual bool DisableDDCWrite() const override { return false; }

    FBox WorldBounds = FBox(ForceInit);
    double FlatHalfWidth = 2700.0;
    double TransitionHalfWidth = 12000.0;
    TArray<FCorridorSample> Samples;
};
} // namespace UE::Avenor::Spine

TArray<FBox> UAvenorSpineTerrainModifier::ComputeBounds() const
{
    const ASpineGenerator* Spine = Cast<ASpineGenerator>(GetOwner());
    if (!Spine)
    {
        return {};
    }
    const FBox Bounds = Spine->GetTerrainCorridorBounds();
    return Bounds.IsValid ? TArray<FBox>{Bounds} : TArray<FBox>{};
}

TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
UAvenorSpineTerrainModifier::CreateBackgroundOp(
    UE::MeshPartition::EBuildType BuildType
) const
{
    (void)BuildType;
    using namespace UE::Avenor::Spine;

    TSharedPtr<FSpineTerrainCorridorOp> Op =
        MakeShared<FSpineTerrainCorridorOp>(GetFName());
    const ASpineGenerator* Spine = Cast<ASpineGenerator>(GetOwner());
    if (!Spine)
    {
        return Op;
    }

    Op->WorldBounds = Spine->GetTerrainCorridorBounds();
    Op->FlatHalfWidth = FMath::Max(0.0f, Spine->CorridorFlatHalfWidth);
    Op->TransitionHalfWidth = FMath::Max(
        Op->FlatHalfWidth + 1.0,
        static_cast<double>(Spine->CorridorTransitionHalfWidth)
    );
    Op->Samples.Reserve(Spine->AlignmentSamples.Num());
    for (const FSpineAlignmentSample& Sample : Spine->AlignmentSamples)
    {
        FVector Location;
        FVector Forward;
        Spine->GetBaseSplineFrameAtChainage(
            Sample.Chainage,
            Location,
            Forward
        );
        FCorridorSample& OpSample = Op->Samples.AddDefaulted_GetRef();
        OpSample.Centre = FVector2D(Location.X, Location.Y);
        OpSample.RoadDatumZ = Sample.RoadDatumZ;
    }
    return Op;
}

FGuid UAvenorSpineTerrainModifier::GetCodeVersionKey() const
{
    return FGuid(TEXT("a731c96c-aafb-4f72-8704-4015b6a94e49"));
}

namespace AvenorSpineTags
{
    static const FName Center(TEXT("Avenor.Spine.Center"));
    static const FName HighwayPositive(TEXT("Avenor.Highway.Positive"));
    static const FName HighwayNegative(TEXT("Avenor.Highway.Negative"));
    static const FName MonorailPositive(TEXT("Avenor.Monorail.Positive"));
    static const FName MonorailNegative(TEXT("Avenor.Monorail.Negative"));
}

ASpineGenerator::ASpineGenerator()
{
    PrimaryActorTick.bCanEverTick = false;
    Tags.AddUnique(FName(TEXT("Avenor.Spine.Source")));

    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);

    TerrainCorridorModifier =
        CreateDefaultSubobject<UAvenorSpineTerrainModifier>(
            TEXT("SpineTerrainCorridor")
        );
    TerrainCorridorModifier->SetupAttachment(SceneRoot);
    TerrainCorridorModifier->bIsEditorOnly = true;

    GuideSpline = CreateDefaultSubobject<USplineComponent>(TEXT("GuideSpline"));
    GuideSpline->SetupAttachment(SceneRoot);
    GuideSpline->SetClosedLoop(false);
    GuideSpline->ComponentTags.Add(AvenorSpineTags::Center);
    GuideSpline->ClearSplinePoints(false);
    GuideSpline->AddSplinePoint(
        FVector(-204800.0f, 0.0f, 0.0f),
        ESplineCoordinateSpace::Local,
        false
    );
    GuideSpline->AddSplinePoint(
        FVector(204800.0f, 0.0f, 0.0f),
        ESplineCoordinateSpace::Local,
        true
    );

    auto CreateDerivedSpline = [this](const TCHAR* Name, FName Tag)
    {
        USplineComponent* Spline =
            CreateDefaultSubobject<USplineComponent>(Name);
        Spline->SetupAttachment(SceneRoot);
        Spline->SetClosedLoop(false);
        Spline->bInputSplinePointsToConstructionScript = false;
        Spline->ComponentTags.Add(Tag);
        return Spline;
    };

    HighwayPositiveSpline = CreateDerivedSpline(
        TEXT("HighwayPositiveSpline"),
        AvenorSpineTags::HighwayPositive
    );
    HighwayNegativeSpline = CreateDerivedSpline(
        TEXT("HighwayNegativeSpline"),
        AvenorSpineTags::HighwayNegative
    );
    MonorailPositiveSpline = CreateDerivedSpline(
        TEXT("MonorailPositiveSpline"),
        AvenorSpineTags::MonorailPositive
    );
    MonorailNegativeSpline = CreateDerivedSpline(
        TEXT("MonorailNegativeSpline"),
        AvenorSpineTags::MonorailNegative
    );

    InfrastructurePCG = CreateDefaultSubobject<UPCGComponent>(
        TEXT("InfrastructurePCG")
    );
    InfrastructurePCG->bIsComponentPartitioned = false;
    InfrastructurePCG->bRegenerateInEditor = false;
}

void ASpineGenerator::PostLoad()
{
    Super::PostLoad();

    // Older versions serialized very large generated arrays and derived
    // splines into the level. They are disposable PCG inputs, so discard them
    // on load instead of paying their memory and Details-panel costs.
    if (!HasAnyFlags(RF_ClassDefaultObject))
    {
        ClearGeneratedPlanningData();
    }
}

void ASpineGenerator::ClearGeneratedPlanningData()
{
    StationRecords.Reset();
    BlockRecords.Reset();
    GreyboxSegments.Reset();
    GuidewayPlacements.Reset();
    MonorailPierPlacements.Reset();
    MonorailSupportPlacements.Reset();

    USplineComponent* Splines[] = {
        HighwayPositiveSpline,
        HighwayNegativeSpline,
        MonorailPositiveSpline,
        MonorailNegativeSpline
    };
    for (USplineComponent* Spline : Splines)
    {
        if (Spline)
        {
            Spline->ClearSplinePoints(true);
        }
    }
}

void ASpineGenerator::RebuildLayoutData()
{
    InfrastructurePCG->bIsComponentPartitioned = bPartitionedGeneration;
    RebuildDerivedSplines();
    RebuildStationAndBlockRecords();
    RebuildGreyboxSegments();
    RebuildMonorailPlacements();

    UE_LOG(
        LogTemp,
        Display,
        TEXT("Avenor Spine: rebuilt %d stations, %d blocks, %d greybox segments, %d guideways, %d piers and %d supports."),
        StationRecords.Num(),
        BlockRecords.Num(),
        GreyboxSegments.Num(),
        GuidewayPlacements.Num(),
        MonorailPierPlacements.Num(),
        MonorailSupportPlacements.Num()
    );
}

void ASpineGenerator::RegenerateInfrastructure()
{
#if WITH_EDITOR
    RebuildLayoutData();
    if (!InfrastructurePCG || !InfrastructureGraph)
    {
        UE_LOG(
            LogTemp,
            Warning,
            TEXT("Avenor Spine: assign PCG_Spine_Master before generating.")
        );
        return;
    }

    InfrastructurePCG->SetGraphLocal(InfrastructureGraph);
    InfrastructurePCG->CleanupLocal(true);
    InfrastructurePCG->GenerateLocal(true);
#endif
}

bool ASpineGenerator::SolveTerrainAlignment()
{
    AlignmentSamples.Reset();
    LastMaximumCutDepth = 0.0f;
    LastMaximumFillHeight = 0.0f;
    LastStructureCandidateCount = 0;
    UWorld* World = GetWorld();
    UE::MeshPartition::AMeshPartition* TargetMeshPartition =
        Cast<UE::MeshPartition::AMeshPartition>(MeshPartitionActor);
    if (!World || !TargetMeshPartition)
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("Avenor Spine: assign the existing Mesh Partition actor before solving terrain alignment.")
        );
        return false;
    }

    const float Start = GetMinimumChainage();
    const float End = GetMaximumChainage();
    const float Step = FMath::Max(2500.0f, AlignmentSampleLength);
    const int32 SampleCount = FMath::Max(
        2,
        FMath::CeilToInt((End - Start) / Step) + 1
    );
    AlignmentSamples.Reserve(SampleCount);

    FCollisionQueryParams QueryParams(
        SCENE_QUERY_STAT(AvenorSpineTerrainSample),
        true
    );
    QueryParams.AddIgnoredActor(this);

    int32 ValidHits = 0;
    int32 FallbackHits = 0;
    for (int32 Index = 0; Index < SampleCount; ++Index)
    {
        const float Chainage = Index == SampleCount - 1
            ? End
            : FMath::Min(Start + Index * Step, End);
        FVector BaseLocation;
        FVector BaseForward;
        GetBaseSplineFrameAtChainage(
            Chainage,
            BaseLocation,
            BaseForward
        );

        const FVector TraceStart = BaseLocation
            + FVector::UpVector * TerrainTraceHalfHeight;
        const FVector TraceEnd = BaseLocation
            - FVector::UpVector * TerrainTraceHalfHeight;
        TArray<FHitResult> Hits;
        World->LineTraceMultiByChannel(
            Hits,
            TraceStart,
            TraceEnd,
            TerrainTraceChannel,
            QueryParams
        );

        const FHitResult* ChosenHit = nullptr;
        for (const FHitResult& Hit : Hits)
        {
            AActor* HitActor = Hit.GetActor();
            for (AActor* Candidate = HitActor;
                 Candidate;
                 Candidate = Candidate->GetAttachParentActor())
            {
                if (Candidate == TargetMeshPartition)
                {
                    ChosenHit = &Hit;
                    break;
                }
            }
            if (ChosenHit)
            {
                break;
            }
        }
        if (!ChosenHit && Hits.Num() > 0)
        {
            // Partition implementations may proxy collision through generated
            // actors. With this Spine actor ignored, the first blocking hit is
            // a useful fallback and is reported explicitly below.
            ChosenHit = &Hits[0];
            ++FallbackHits;
        }

        FSpineAlignmentSample& Sample =
            AlignmentSamples.AddDefaulted_GetRef();
        Sample.Chainage = Chainage;
        Sample.bTerrainHit = ChosenHit != nullptr;
        Sample.NaturalTerrainZ = ChosenHit
            ? static_cast<float>(ChosenHit->ImpactPoint.Z)
            : BaseLocation.Z;
        Sample.RoadDatumZ = Sample.NaturalTerrainZ + RoadDatumOffset;
        ValidHits += Sample.bTerrainHit ? 1 : 0;
    }

    if (ValidHits == 0)
    {
        AlignmentSamples.Reset();
        UE_LOG(
            LogTemp,
            Error,
            TEXT("Avenor Spine: terrain sampling found no collision. Enable collision on Mesh Terrain and rebuild it first.")
        );
        return false;
    }

    // Fill isolated misses by interpolating the nearest valid terrain samples.
    for (int32 Index = 0; Index < AlignmentSamples.Num(); ++Index)
    {
        if (AlignmentSamples[Index].bTerrainHit)
        {
            continue;
        }
        int32 Left = Index - 1;
        while (Left >= 0 && !AlignmentSamples[Left].bTerrainHit)
        {
            --Left;
        }
        int32 Right = Index + 1;
        while (Right < AlignmentSamples.Num()
            && !AlignmentSamples[Right].bTerrainHit)
        {
            ++Right;
        }

        float FilledZ = AlignmentSamples[Index].NaturalTerrainZ;
        if (Left >= 0 && Right < AlignmentSamples.Num())
        {
            const float Alpha = FMath::GetRangePct(
                AlignmentSamples[Left].Chainage,
                AlignmentSamples[Right].Chainage,
                AlignmentSamples[Index].Chainage
            );
            FilledZ = FMath::Lerp(
                AlignmentSamples[Left].NaturalTerrainZ,
                AlignmentSamples[Right].NaturalTerrainZ,
                Alpha
            );
        }
        else if (Left >= 0)
        {
            FilledZ = AlignmentSamples[Left].NaturalTerrainZ;
        }
        else if (Right < AlignmentSamples.Num())
        {
            FilledZ = AlignmentSamples[Right].NaturalTerrainZ;
        }
        AlignmentSamples[Index].NaturalTerrainZ = FilledZ;
        AlignmentSamples[Index].RoadDatumZ = FilledZ + RoadDatumOffset;
    }

    TArray<float> SmoothedHeights;
    SmoothedHeights.SetNumUninitialized(AlignmentSamples.Num());
    const int32 SmoothingRadius = FMath::Max(
        1,
        FMath::RoundToInt(AlignmentSmoothingDistance / (2.0f * Step))
    );
    for (int32 Index = 0; Index < AlignmentSamples.Num(); ++Index)
    {
        double Sum = 0.0;
        double TotalWeight = 0.0;
        const int32 First = FMath::Max(0, Index - SmoothingRadius);
        const int32 Last = FMath::Min(
            AlignmentSamples.Num() - 1,
            Index + SmoothingRadius
        );
        for (int32 Neighbor = First; Neighbor <= Last; ++Neighbor)
        {
            const double Weight = static_cast<double>(
                SmoothingRadius + 1 - FMath::Abs(Neighbor - Index)
            );
            Sum += AlignmentSamples[Neighbor].RoadDatumZ * Weight;
            TotalWeight += Weight;
        }
        SmoothedHeights[Index] = static_cast<float>(
            Sum / FMath::Max(1.0, TotalWeight)
        );
    }

    // Alternating forward/backward projection prevents either end of the
    // route from being the sole anchor while enforcing the grade everywhere.
    for (int32 Pass = 0; Pass < 8; ++Pass)
    {
        for (int32 Index = 1; Index < SmoothedHeights.Num(); ++Index)
        {
            const float MaximumDelta = MaximumRoadGrade * (
                AlignmentSamples[Index].Chainage
                - AlignmentSamples[Index - 1].Chainage
            );
            SmoothedHeights[Index] = FMath::Clamp(
                SmoothedHeights[Index],
                SmoothedHeights[Index - 1] - MaximumDelta,
                SmoothedHeights[Index - 1] + MaximumDelta
            );
        }
        for (int32 Index = SmoothedHeights.Num() - 2; Index >= 0; --Index)
        {
            const float MaximumDelta = MaximumRoadGrade * (
                AlignmentSamples[Index + 1].Chainage
                - AlignmentSamples[Index].Chainage
            );
            SmoothedHeights[Index] = FMath::Clamp(
                SmoothedHeights[Index],
                SmoothedHeights[Index + 1] - MaximumDelta,
                SmoothedHeights[Index + 1] + MaximumDelta
            );
        }
    }
    LastMaximumCutDepth = 0.0f;
    LastMaximumFillHeight = 0.0f;
    LastStructureCandidateCount = 0;
    for (int32 Index = 0; Index < AlignmentSamples.Num(); ++Index)
    {
        AlignmentSamples[Index].RoadDatumZ = SmoothedHeights[Index];
        AlignmentSamples[Index].EarthworkDelta =
            AlignmentSamples[Index].RoadDatumZ
            - AlignmentSamples[Index].NaturalTerrainZ;
        AlignmentSamples[Index].bStructureCandidate =
            FMath::Abs(AlignmentSamples[Index].EarthworkDelta)
            > EarthworkWarningThreshold;
        LastStructureCandidateCount +=
            AlignmentSamples[Index].bStructureCandidate ? 1 : 0;
        LastMaximumFillHeight = FMath::Max(
            LastMaximumFillHeight,
            AlignmentSamples[Index].EarthworkDelta
        );
        LastMaximumCutDepth = FMath::Max(
            LastMaximumCutDepth,
            -AlignmentSamples[Index].EarthworkDelta
        );
    }

    UE_LOG(
        LogTemp,
        Display,
        TEXT("Avenor Spine: solved %d terrain samples at %.0f cm spacing (%d direct/proxy hits, %d proxy fallbacks), %.1f%% maximum grade; maximum cut %.1f m, fill %.1f m, %d structure candidates."),
        AlignmentSamples.Num(),
        Step,
        ValidHits - FallbackHits,
        FallbackHits,
        MaximumRoadGrade * 100.0f,
        LastMaximumCutDepth / 100.0f,
        LastMaximumFillHeight / 100.0f,
        LastStructureCandidateCount
    );
    return true;
}

bool ASpineGenerator::BindTerrainModifier()
{
#if WITH_EDITOR
    UE::MeshPartition::AMeshPartition* TargetMeshPartition =
        Cast<UE::MeshPartition::AMeshPartition>(MeshPartitionActor);
    if (!TargetMeshPartition || !TerrainCorridorModifier
        || AlignmentSamples.Num() < 2)
    {
        return false;
    }

    TerrainCorridorModifier->Modify();
    TerrainCorridorModifier->SetAffectedMeshPartition(nullptr);
    TerrainCorridorModifier->BP_SetAffectedMegaMesh(TargetMeshPartition);
    const TArray<FName> PriorityLayers =
        TerrainCorridorModifier->GetDefinitionPriorityLayers();
    if (PriorityLayers.Num() < 2)
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("Avenor Spine: Mesh Partition Definition needs at least two modifier priority layers.")
        );
        TerrainCorridorModifier->SetAffectedMeshPartition(nullptr);
        return false;
    }

    TerrainCorridorModifier->SetPriorityLayer(PriorityLayers.Last());
    TerrainCorridorModifier->SetPriority(TerrainModifierPriority);
    TerrainCorridorModifier->PostEditChange();
    TargetMeshPartition->Modify();
    TargetMeshPartition->PostEditChange();
    TargetMeshPartition->ReregisterAllComponents();
    TargetMeshPartition->MarkPackageDirty();
    MarkPackageDirty();
    return true;
#else
    return false;
#endif
}

void ASpineGenerator::RebuildTerrainAlignment()
{
#if WITH_EDITOR
    Modify();
    if (!SolveTerrainAlignment())
    {
        return;
    }
    if (!BindTerrainModifier())
    {
        AlignmentSamples.Reset();
        LastMaximumCutDepth = 0.0f;
        LastMaximumFillHeight = 0.0f;
        LastStructureCandidateCount = 0;
        return;
    }
    RebuildLayoutData();
    UE_LOG(
        LogTemp,
        Display,
        TEXT("Avenor Spine: submitted the %.0f cm flat corridor and %.0f cm terrain transition."),
        CorridorFlatHalfWidth,
        CorridorTransitionHalfWidth
    );
#endif
}

void ASpineGenerator::RegenerateCompleteSpine()
{
#if WITH_EDITOR
    RebuildTerrainAlignment();
    if (AlignmentSamples.Num() >= 2)
    {
        RegenerateInfrastructure();
    }
#endif
}

void ASpineGenerator::ClearTerrainAlignment()
{
#if WITH_EDITOR
    Modify();
    if (TerrainCorridorModifier)
    {
        TerrainCorridorModifier->Modify();
        TerrainCorridorModifier->SetAffectedMeshPartition(nullptr);
        TerrainCorridorModifier->PostEditChange();
    }
    AlignmentSamples.Reset();
    LastMaximumCutDepth = 0.0f;
    LastMaximumFillHeight = 0.0f;
    LastStructureCandidateCount = 0;

    if (UE::MeshPartition::AMeshPartition* TargetMeshPartition =
        Cast<UE::MeshPartition::AMeshPartition>(MeshPartitionActor))
    {
        TargetMeshPartition->Modify();
        TargetMeshPartition->PostEditChange();
        TargetMeshPartition->ReregisterAllComponents();
        TargetMeshPartition->MarkPackageDirty();
    }
    RebuildLayoutData();
    MarkPackageDirty();
#endif
}

void ASpineGenerator::ClearInfrastructure()
{
#if WITH_EDITOR
    if (InfrastructurePCG)
    {
        InfrastructurePCG->CleanupLocal(true);
    }
    ClearGeneratedPlanningData();
#endif
}

void ASpineGenerator::ResetToPrototypeDefaults()
{
#if WITH_EDITOR
    Modify();
    if (InfrastructurePCG)
    {
        InfrastructurePCG->CleanupLocal(true);
        InfrastructurePCG->bIsComponentPartitioned = false;
        InfrastructurePCG->bRegenerateInEditor = false;
    }

    bPartitionedGeneration = false;
    DistrictsBeforeStationZero = 0;
    DistrictsAfterStationZero = 1;
    DevelopmentRowsPerSide = 1;
    AlignmentSampleLength = 2500.0f;
    if (TerrainCorridorModifier)
    {
        TerrainCorridorModifier->SetAffectedMeshPartition(nullptr);
        TerrainCorridorModifier->PostEditChange();
    }
    AlignmentSamples.Reset();
    LastMaximumCutDepth = 0.0f;
    LastMaximumFillHeight = 0.0f;
    LastStructureCandidateCount = 0;
    ClearGeneratedPlanningData();
    MarkPackageDirty();

    UE_LOG(
        LogTemp,
        Display,
        TEXT("Avenor Spine: cleared generated data and restored lightweight prototype defaults.")
    );
#endif
}

float ASpineGenerator::GetMinimumChainage() const
{
    return -static_cast<float>(DistrictsBeforeStationZero) * StationSpacing;
}

float ASpineGenerator::GetMaximumChainage() const
{
    return static_cast<float>(DistrictsAfterStationZero) * StationSpacing;
}

float ASpineGenerator::GetResolvedLocalStreetWidth() const
{
    const float Remaining = StationSpacing
        - BlocksPerDistrict * BlockSize
        - StationStreetWidth;
    return FMath::Max(0.0f, Remaining / (BlocksPerDistrict - 1));
}

void ASpineGenerator::GetBaseSplineFrameAtChainage(
    float Chainage,
    FVector& OutLocation,
    FVector& OutForward
) const
{
    const float SplineLength = GuideSpline
        ? GuideSpline->GetSplineLength()
        : 0.0f;
    if (!GuideSpline || SplineLength <= KINDA_SMALL_NUMBER)
    {
        OutLocation = GetActorLocation() + FVector(Chainage, 0.0f, 0.0f);
        OutForward = GetActorForwardVector();
        return;
    }

    const float RequestedDistance = StationZeroSplineDistance + Chainage;
    const float SampleDistance = FMath::Clamp(
        RequestedDistance,
        0.0f,
        SplineLength
    );
    OutLocation = GuideSpline->GetLocationAtDistanceAlongSpline(
        SampleDistance,
        ESplineCoordinateSpace::World
    );
    OutForward = GuideSpline->GetDirectionAtDistanceAlongSpline(
        SampleDistance,
        ESplineCoordinateSpace::World
    ).GetSafeNormal();

    if (RequestedDistance < 0.0f)
    {
        OutLocation += OutForward * RequestedDistance;
    }
    else if (RequestedDistance > SplineLength)
    {
        OutLocation += OutForward * (RequestedDistance - SplineLength);
    }
}

FTransform ASpineGenerator::GetSpineTransformAtChainage(float Chainage) const
{
    FVector Location;
    FVector Forward;
    GetBaseSplineFrameAtChainage(Chainage, Location, Forward);

    if (AlignmentSamples.Num() >= 2)
    {
        Location.Z = EvaluateRoadDatumZ(Chainage);
        const float Probe = FMath::Max(
            100.0f,
            AlignmentSampleLength * 0.25f
        );
        FVector Before;
        FVector BeforeForward;
        FVector After;
        FVector AfterForward;
        GetBaseSplineFrameAtChainage(
            Chainage - Probe,
            Before,
            BeforeForward
        );
        GetBaseSplineFrameAtChainage(
            Chainage + Probe,
            After,
            AfterForward
        );
        Before.Z = EvaluateRoadDatumZ(Chainage - Probe);
        After.Z = EvaluateRoadDatumZ(Chainage + Probe);
        Forward = (After - Before).GetSafeNormal();
    }

    if (Forward.IsNearlyZero())
    {
        Forward = GetActorForwardVector();
    }

    return FTransform(Forward.Rotation(), Location);
}

float ASpineGenerator::EvaluateRoadDatumZ(float Chainage) const
{
    if (AlignmentSamples.Num() == 0)
    {
        FVector Location;
        FVector Forward;
        GetBaseSplineFrameAtChainage(Chainage, Location, Forward);
        return Location.Z;
    }
    if (Chainage <= AlignmentSamples[0].Chainage)
    {
        return AlignmentSamples[0].RoadDatumZ;
    }
    if (Chainage >= AlignmentSamples.Last().Chainage)
    {
        return AlignmentSamples.Last().RoadDatumZ;
    }

    for (int32 Index = 1; Index < AlignmentSamples.Num(); ++Index)
    {
        if (Chainage <= AlignmentSamples[Index].Chainage)
        {
            const FSpineAlignmentSample& A = AlignmentSamples[Index - 1];
            const FSpineAlignmentSample& B = AlignmentSamples[Index];
            const float Alpha = FMath::GetRangePct(
                A.Chainage,
                B.Chainage,
                Chainage
            );
            return FMath::Lerp(A.RoadDatumZ, B.RoadDatumZ, Alpha);
        }
    }
    return AlignmentSamples.Last().RoadDatumZ;
}

FBox ASpineGenerator::GetTerrainCorridorBounds() const
{
    if (AlignmentSamples.Num() < 2)
    {
        return FBox(ForceInit);
    }

    FBox Bounds(ForceInit);
    for (const FSpineAlignmentSample& Sample : AlignmentSamples)
    {
        FVector Location;
        FVector Forward;
        GetBaseSplineFrameAtChainage(
            Sample.Chainage,
            Location,
            Forward
        );
        Location.Z = Sample.RoadDatumZ;
        Bounds += Location;
    }
    const float HorizontalExtent = FMath::Max(
        CorridorFlatHalfWidth + 1.0f,
        CorridorTransitionHalfWidth
    );
    const FVector Expansion(
        HorizontalExtent,
        HorizontalExtent,
        TerrainTraceHalfHeight
    );
    return Bounds.ExpandBy(Expansion);
}

FVector ASpineGenerator::GetSpineLocationAtChainage(
    float Chainage,
    float LateralOffset,
    float VerticalOffset
) const
{
    const FTransform Frame = GetSpineTransformAtChainage(Chainage);
    return Frame.GetLocation()
        + Frame.GetUnitAxis(EAxis::Y) * LateralOffset
        + FVector::UpVector * VerticalOffset;
}

void ASpineGenerator::GetSpineSpaceForWorldLocation(
    const FVector& WorldLocation,
    float& OutChainage,
    float& OutLateral,
    float& OutVertical
) const
{
    if (!GuideSpline || GuideSpline->GetSplineLength() <= KINDA_SMALL_NUMBER)
    {
        const FVector Local = GetActorTransform().InverseTransformPosition(
            WorldLocation
        );
        OutChainage = Local.X;
        OutLateral = Local.Y;
        OutVertical = Local.Z;
        return;
    }

    const float SplineLength = GuideSpline->GetSplineLength();
    const float InputKey =
        GuideSpline->FindInputKeyClosestToWorldLocation(WorldLocation);
    float Distance =
        GuideSpline->GetDistanceAlongSplineAtSplineInputKey(InputKey);

    const FVector StartLocation = GuideSpline->GetLocationAtDistanceAlongSpline(
        0.0f,
        ESplineCoordinateSpace::World
    );
    const FVector StartDirection =
        GuideSpline->GetDirectionAtDistanceAlongSpline(
            0.0f,
            ESplineCoordinateSpace::World
        ).GetSafeNormal();
    const FVector EndLocation = GuideSpline->GetLocationAtDistanceAlongSpline(
        SplineLength,
        ESplineCoordinateSpace::World
    );
    const FVector EndDirection =
        GuideSpline->GetDirectionAtDistanceAlongSpline(
            SplineLength,
            ESplineCoordinateSpace::World
        ).GetSafeNormal();
    const float BeforeStart = FVector::DotProduct(
        WorldLocation - StartLocation,
        StartDirection
    );
    const float BeyondEnd = FVector::DotProduct(
        WorldLocation - EndLocation,
        EndDirection
    );
    if (Distance <= KINDA_SMALL_NUMBER && BeforeStart < 0.0f)
    {
        Distance = BeforeStart;
    }
    else if (Distance >= SplineLength - KINDA_SMALL_NUMBER && BeyondEnd > 0.0f)
    {
        Distance = SplineLength + BeyondEnd;
    }

    OutChainage = Distance - StationZeroSplineDistance;
    const FTransform Frame = GetSpineTransformAtChainage(OutChainage);
    const FVector Delta = WorldLocation - Frame.GetLocation();
    OutLateral = FVector::DotProduct(
        Delta,
        Frame.GetUnitAxis(EAxis::Y)
    );
    OutVertical = FVector::DotProduct(Delta, FVector::UpVector);
}

void ASpineGenerator::AddDerivedSplinePoint(
    USplineComponent* Spline,
    float Chainage,
    float Lateral,
    float Vertical
)
{
    if (Spline)
    {
        Spline->AddSplinePoint(
            GetSpineLocationAtChainage(Chainage, Lateral, Vertical),
            ESplineCoordinateSpace::World,
            false
        );
    }
}

void ASpineGenerator::RebuildDerivedSplines()
{
    USplineComponent* Splines[] = {
        HighwayPositiveSpline,
        HighwayNegativeSpline,
        MonorailPositiveSpline,
        MonorailNegativeSpline
    };
    for (USplineComponent* Spline : Splines)
    {
        if (Spline)
        {
            Spline->ClearSplinePoints(false);
        }
    }

    const float CarriagewayOffset = HighwayMedianWidth * 0.5f
        + HighwayCarriagewayWidth * 0.5f;
    const float Start = GetMinimumChainage();
    const float End = GetMaximumChainage();
    const float Step = FMath::Max(100.0f, AlignmentSampleLength);
    for (float Chainage = Start; Chainage < End; Chainage += Step)
    {
        AddDerivedSplinePoint(
            HighwayPositiveSpline,
            Chainage,
            CarriagewayOffset,
            0.0f
        );
        AddDerivedSplinePoint(
            HighwayNegativeSpline,
            Chainage,
            -CarriagewayOffset,
            0.0f
        );
        AddDerivedSplinePoint(
            MonorailPositiveSpline,
            Chainage,
            MonorailTrackCentreOffset,
            MonorailGuidewayCentreHeight
        );
        AddDerivedSplinePoint(
            MonorailNegativeSpline,
            Chainage,
            -MonorailTrackCentreOffset,
            MonorailGuidewayCentreHeight
        );
    }

    AddDerivedSplinePoint(
        HighwayPositiveSpline,
        End,
        CarriagewayOffset,
        0.0f
    );
    AddDerivedSplinePoint(
        HighwayNegativeSpline,
        End,
        -CarriagewayOffset,
        0.0f
    );
    AddDerivedSplinePoint(
        MonorailPositiveSpline,
        End,
        MonorailTrackCentreOffset,
        MonorailGuidewayCentreHeight
    );
    AddDerivedSplinePoint(
        MonorailNegativeSpline,
        End,
        -MonorailTrackCentreOffset,
        MonorailGuidewayCentreHeight
    );

    for (USplineComponent* Spline : Splines)
    {
        if (Spline)
        {
            Spline->UpdateSpline();
        }
    }
}

float ASpineGenerator::GetBlockCentreOffset(int32 BayIndex) const
{
    const float LocalStreetWidth = GetResolvedLocalStreetWidth();
    return StationStreetWidth * 0.5f
        + BlockSize * 0.5f
        + BayIndex * (BlockSize + LocalStreetWidth);
}

float ASpineGenerator::GetInternalStreetCentreOffset(int32 StreetIndex) const
{
    const float LocalStreetWidth = GetResolvedLocalStreetWidth();
    return StationStreetWidth * 0.5f
        + StreetIndex * BlockSize
        + (static_cast<float>(StreetIndex) - 0.5f) * LocalStreetWidth;
}

float ASpineGenerator::GetDevelopmentOuterLateral() const
{
    const float LocalStreetWidth = GetResolvedLocalStreetWidth();
    return SpineReservationWidth * 0.5f
        + DevelopmentRowsPerSide * (BlockSize + LocalStreetWidth);
}

FString ASpineGenerator::FormatSignedId(const TCHAR* Prefix, int32 Index) const
{
    return FString::Printf(
        TEXT("%s%c%04d"),
        Prefix,
        Index < 0 ? TEXT('-') : TEXT('+'),
        FMath::Abs(Index)
    );
}

void ASpineGenerator::RebuildStationAndBlockRecords()
{
    StationRecords.Reset();
    BlockRecords.Reset();

    const float LocalStreetWidth = GetResolvedLocalStreetWidth();
    if (LocalStreetWidth <= KINDA_SMALL_NUMBER)
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("Avenor Spine: district dimensions leave no room for streets.")
        );
        return;
    }

    for (int32 StationIndex = -DistrictsBeforeStationZero;
         StationIndex <= DistrictsAfterStationZero;
         ++StationIndex)
    {
        FSpineStationRecord& Station = StationRecords.AddDefaulted_GetRef();
        Station.StationIndex = StationIndex;
        Station.Chainage = static_cast<float>(StationIndex) * StationSpacing;
        Station.StationId = FName(*FormatSignedId(TEXT("S"), StationIndex));
        Station.Transform = GetSpineTransformAtChainage(Station.Chainage);
        Station.PlatformDatum = StationPlatformDatum;
        Station.PublicRealmBayIndex = PublicRealmBayIndex;
    }

    for (int32 DistrictIndex = -DistrictsBeforeStationZero;
         DistrictIndex < DistrictsAfterStationZero;
         ++DistrictIndex)
    {
        const float DistrictStart =
            static_cast<float>(DistrictIndex) * StationSpacing;
        for (int32 BayIndex = 0; BayIndex < BlocksPerDistrict; ++BayIndex)
        {
            const float Chainage = DistrictStart
                + GetBlockCentreOffset(BayIndex);
            for (int32 RowIndex = 0;
                 RowIndex < DevelopmentRowsPerSide;
                 ++RowIndex)
            {
                const float RowCentre = SpineReservationWidth * 0.5f
                    + LocalStreetWidth * 0.5f
                    + BlockSize * 0.5f
                    + RowIndex * (BlockSize + LocalStreetWidth);
                for (const int32 Side : {-1, 1})
                {
                    FSpineBlockRecord& Block =
                        BlockRecords.AddDefaulted_GetRef();
                    Block.DistrictIndex = DistrictIndex;
                    Block.BayIndex = BayIndex;
                    Block.RowIndex = RowIndex;
                    Block.Side = Side;
                    Block.Chainage = Chainage;
                    Block.Lateral = Side * RowCentre;
                    Block.ClearSize = FVector(BlockSize, BlockSize, 0.0f);
                    Block.ZoneRole = BayIndex == PublicRealmBayIndex
                        ? FName(TEXT("PublicRealm"))
                        : FName(TEXT("Development"));
                    Block.Transform = FTransform(
                        GetSpineTransformAtChainage(Chainage).GetRotation(),
                        GetSpineLocationAtChainage(Chainage, Block.Lateral)
                    );
                    Block.BlockId = FName(*FString::Printf(
                        TEXT("%s-B%02d-%c-R%02d"),
                        *FormatSignedId(TEXT("D"), DistrictIndex),
                        BayIndex,
                        Side > 0 ? TEXT('P') : TEXT('N'),
                        RowIndex
                    ));
                }
            }
        }
    }

}

void ASpineGenerator::AddGreyboxSpan(
    FName Kind,
    float StartChainage,
    float EndChainage,
    float StartLateral,
    float EndLateral,
    float CentreHeight,
    float Width,
    float Thickness,
    int32 DistrictIndex,
    int32 Side
)
{
    FVector A = GetSpineLocationAtChainage(StartChainage, StartLateral);
    FVector B = GetSpineLocationAtChainage(EndChainage, EndLateral);
    A.Z += CentreHeight;
    B.Z += CentreHeight;
    const FVector Delta = B - A;
    if (Delta.IsNearlyZero())
    {
        return;
    }

    FSpineGreyboxSegment& Segment =
        GreyboxSegments.AddDefaulted_GetRef();
    Segment.Kind = Kind;
    Segment.DistrictIndex = DistrictIndex;
    Segment.Side = Side;
    Segment.Chainage = (StartChainage + EndChainage) * 0.5f;
    Segment.Transform = FTransform(
        Delta.Rotation(),
        (A + B) * 0.5f,
        FVector(Delta.Size(), Width, Thickness) / 100.0f
    );
}

void ASpineGenerator::RebuildGreyboxSegments()
{
    GreyboxSegments.Reset();
    const float Start = GetMinimumChainage();
    const float End = GetMaximumChainage();
    const float Step = FMath::Max(200.0f, AlignmentSampleLength);
    const float CarriagewayOffset = HighwayMedianWidth * 0.5f
        + HighwayCarriagewayWidth * 0.5f;

    for (float Chainage = Start; Chainage < End; Chainage += Step)
    {
        const float Next = FMath::Min(Chainage + Step, End);
        const int32 DistrictIndex = FMath::FloorToInt(
            Chainage / StationSpacing
        );
        for (const int32 Side : {-1, 1})
        {
            AddGreyboxSpan(
                TEXT("Highway"),
                Chainage,
                Next,
                Side * CarriagewayOffset,
                Side * CarriagewayOffset,
                RoadThickness * 0.5f,
                HighwayCarriagewayWidth,
                RoadThickness,
                DistrictIndex,
                Side
            );
        }
    }

    const float LocalStreetWidth = GetResolvedLocalStreetWidth();
    const float InnerLateral = SpineReservationWidth * 0.5f;
    const float OuterLateral = GetDevelopmentOuterLateral();
    for (int32 DistrictIndex = -DistrictsBeforeStationZero;
         DistrictIndex < DistrictsAfterStationZero;
         ++DistrictIndex)
    {
        const float DistrictStart =
            static_cast<float>(DistrictIndex) * StationSpacing;
        for (const int32 Side : {-1, 1})
        {
            AddGreyboxSpan(
                TEXT("StationStreet"),
                DistrictStart,
                DistrictStart,
                Side * InnerLateral,
                Side * OuterLateral,
                RoadThickness * 0.5f,
                StationStreetWidth,
                RoadThickness,
                DistrictIndex,
                Side
            );
            for (int32 StreetIndex = 1;
                 StreetIndex < BlocksPerDistrict;
                 ++StreetIndex)
            {
                const float StreetChainage = DistrictStart
                    + GetInternalStreetCentreOffset(StreetIndex);
                AddGreyboxSpan(
                    TEXT("LocalStreet"),
                    StreetChainage,
                    StreetChainage,
                    Side * InnerLateral,
                    Side * OuterLateral,
                    RoadThickness * 0.5f,
                    LocalStreetWidth,
                    RoadThickness,
                    DistrictIndex,
                    Side
                );
            }

            for (int32 Boundary = 0;
                 Boundary <= DevelopmentRowsPerSide;
                 ++Boundary)
            {
                const float Lateral = Side * (
                    InnerLateral
                    + static_cast<float>(Boundary)
                        * (BlockSize + LocalStreetWidth)
                );
                for (float Chainage = DistrictStart;
                     Chainage < DistrictStart + StationSpacing;
                     Chainage += Step)
                {
                    const float Next = FMath::Min(
                        Chainage + Step,
                        DistrictStart + StationSpacing
                    );
                    AddGreyboxSpan(
                        TEXT("LocalStreet"),
                        Chainage,
                        Next,
                        Lateral,
                        Lateral,
                        RoadThickness * 0.5f,
                        LocalStreetWidth,
                        RoadThickness,
                        DistrictIndex,
                        Side
                    );
                }
            }
        }
    }

    // The final district contributes its starting station street above; add
    // the terminal station street explicitly so the generated road grid has
    // a closed outer boundary too.
    const float TerminalChainage = GetMaximumChainage();
    for (const int32 Side : {-1, 1})
    {
        AddGreyboxSpan(
            TEXT("StationStreet"),
            TerminalChainage,
            TerminalChainage,
            Side * InnerLateral,
            Side * OuterLateral,
            RoadThickness * 0.5f,
            StationStreetWidth,
            RoadThickness,
            DistrictsAfterStationZero,
            Side
        );
    }
}

void ASpineGenerator::RebuildMonorailPlacements()
{
    GuidewayPlacements.Reset();
    MonorailPierPlacements.Reset();
    MonorailSupportPlacements.Reset();

    const float Start = GetMinimumChainage();
    const float End = GetMaximumChainage();
    const float SpanLength = FMath::Max(100.0f, MonorailSpanLength);
    const float TotalLength = FMath::Max(0.0f, End - Start);
    const int32 SpanCount = FMath::CeilToInt(TotalLength / SpanLength);

    auto AddSharedPlacement = [this](
        TArray<FSpineInfrastructurePlacement>& Placements,
        FName Kind,
        int32 SpanIndex,
        float Chainage,
        float Height)
    {
        FSpineInfrastructurePlacement& Placement =
            Placements.AddDefaulted_GetRef();
        Placement.Kind = Kind;
        Placement.SpanIndex = SpanIndex;
        Placement.Side = 0;
        Placement.Chainage = Chainage;
        FVector HorizontalForward = GetSpineTransformAtChainage(Chainage)
            .GetUnitAxis(EAxis::X);
        HorizontalForward.Z = 0.0f;
        if (!HorizontalForward.Normalize())
        {
            HorizontalForward = GetActorForwardVector();
            HorizontalForward.Z = 0.0f;
            HorizontalForward.Normalize();
        }
        Placement.Transform = FTransform(
            HorizontalForward.Rotation(),
            GetSpineLocationAtChainage(Chainage, 0.0f, Height),
            FVector::OneVector
        );
    };

    // Piers and their upper supports share the same chainage and orientation.
    // There is one at both ends of every span, without duplicated boundaries.
    for (int32 BoundaryIndex = 0; BoundaryIndex <= SpanCount; ++BoundaryIndex)
    {
        const float Chainage = BoundaryIndex == SpanCount
            ? End
            : FMath::Min(Start + BoundaryIndex * SpanLength, End);
        AddSharedPlacement(
            MonorailPierPlacements,
            TEXT("MonorailPier"),
            BoundaryIndex,
            Chainage,
            0.0f
        );
        AddSharedPlacement(
            MonorailSupportPlacements,
            TEXT("MonorailSupport"),
            BoundaryIndex,
            Chainage,
            MonorailSupportPivotHeight
        );
    }

    for (int32 SpanIndex = 0; SpanIndex < SpanCount; ++SpanIndex)
    {
        const float SpanStart = Start + SpanIndex * SpanLength;
        const float SpanEnd = FMath::Min(SpanStart + SpanLength, End);
        for (const int32 Side : {-1, 1})
        {
            FVector A = GetSpineLocationAtChainage(
                SpanStart,
                Side * MonorailTrackCentreOffset,
                MonorailGuidewayCentreHeight
            );
            FVector B = GetSpineLocationAtChainage(
                SpanEnd,
                Side * MonorailTrackCentreOffset,
                MonorailGuidewayCentreHeight
            );
            const FVector Delta = B - A;
            if (Delta.IsNearlyZero())
            {
                continue;
            }

            FSpineInfrastructurePlacement& Placement =
                GuidewayPlacements.AddDefaulted_GetRef();
            Placement.Kind = TEXT("Guideway");
            Placement.SpanIndex = SpanIndex;
            Placement.Side = Side;
            Placement.Chainage = (SpanStart + SpanEnd) * 0.5f;
            Placement.Transform = FTransform(
                Delta.Rotation(),
                (A + B) * 0.5f,
                FVector(Delta.Size() / SpanLength, 1.0f, 1.0f)
            );
        }
    }
}
