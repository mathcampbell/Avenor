#include "AvenorSpineTerrainModifier.h"

#include "AvenorTerrainData.h"
#include "SpineGenerator.h"
#include "MeshPartition.h"
#include "MeshPartitionMeshView.h"
#include "EngineUtils.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterSplineComponent.h"
#include "WaterSplineMetadata.h"

namespace UE::Avenor::Spine
{
static const FName SpineExclusionChannel(TEXT("SpineExclusion"));

struct FCorridorSample
{
    FVector2D Centre = FVector2D::ZeroVector;
    double RoadDatumZ = 0.0;
    TArray<double> LeftDevelopmentProfileZ;
    TArray<double> RightDevelopmentProfileZ;
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
        if (Samples.Num() < 2 || DevelopmentHalfWidth <= 0.0
            || TransitionHalfWidth <= DevelopmentHalfWidth)
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
            int32 BestSegmentIndex = INDEX_NONE;
            double BestSegmentAlpha = 0.0;
            double SignedLateral = 0.0;
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
                    const FVector2D Projected = A + Segment * Alpha;
                    const FVector2D Offset = Position - Projected;
                    const double SegmentLength = FMath::Sqrt(LengthSquared);
                    SignedLateral = SegmentLength > UE_DOUBLE_SMALL_NUMBER
                        ? FVector2D::CrossProduct(Segment, Offset)
                            / SegmentLength
                        : 0.0;
                    BestSegmentIndex = Index;
                    BestSegmentAlpha = Alpha;
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

            const double CrossSectionAlpha = FMath::Clamp(
                (Distance - FlatHalfWidth) /
                    FMath::Max(1.0, DevelopmentHalfWidth - FlatHalfWidth),
                0.0,
                1.0
            );
            double TargetDevelopmentZ = TargetRoadZ;
            if (BestSegmentIndex != INDEX_NONE && CrossSectionAlpha > 0.0)
            {
                const bool bRightSide = SignedLateral >= 0.0;
                const TArray<double>& ProfileA = bRightSide
                    ? Samples[BestSegmentIndex].RightDevelopmentProfileZ
                    : Samples[BestSegmentIndex].LeftDevelopmentProfileZ;
                const TArray<double>& ProfileB = bRightSide
                    ? Samples[BestSegmentIndex + 1].RightDevelopmentProfileZ
                    : Samples[BestSegmentIndex + 1].LeftDevelopmentProfileZ;
                const int32 ProfileCount = FMath::Min(
                    ProfileA.Num(),
                    ProfileB.Num()
                );
                if (ProfileCount >= 2)
                {
                    const double ProfilePosition = CrossSectionAlpha
                        * static_cast<double>(ProfileCount - 1);
                    const int32 ProfileIndex = FMath::Min(
                        FMath::FloorToInt(ProfilePosition),
                        ProfileCount - 2
                    );
                    const double ProfileAlpha = ProfilePosition
                        - static_cast<double>(ProfileIndex);
                    const double HeightA = FMath::Lerp(
                        ProfileA[ProfileIndex],
                        ProfileA[ProfileIndex + 1],
                        ProfileAlpha
                    );
                    const double HeightB = FMath::Lerp(
                        ProfileB[ProfileIndex],
                        ProfileB[ProfileIndex + 1],
                        ProfileAlpha
                    );
                    TargetDevelopmentZ = FMath::Lerp(
                        HeightA,
                        HeightB,
                        BestSegmentAlpha
                    );
                }
            }
            const double Blend = Distance <= DevelopmentHalfWidth
                ? 1.0
                : 1.0 - FMath::SmoothStep(
                    DevelopmentHalfWidth,
                    TransitionHalfWidth,
                    Distance
                );
            WorldPosition.Z = FMath::Lerp(
                WorldPosition.Z,
                TargetDevelopmentZ,
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
    double DevelopmentHalfWidth = 12000.0;
    double TransitionHalfWidth = 24000.0;
    TArray<FCorridorSample> Samples;
};
} // namespace UE::Avenor::Spine


TArray<FBox> UAvenorSpineTerrainModifier::ComputeBounds() const
{
    const ASpineGenerator* Spine = Cast<ASpineGenerator>(GetOwner());
    if (!Spine) return {};
    const FBox CorridorBounds = Spine->GetTerrainCorridorBounds();
    return CorridorBounds.IsValid ? TArray<FBox>{CorridorBounds} : TArray<FBox>{};
}

TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
UAvenorSpineTerrainModifier::CreateBackgroundOp(UE::MeshPartition::EBuildType BuildType) const
{
    (void)BuildType;
    using namespace UE::Avenor::Spine;
    TSharedPtr<FSpineTerrainCorridorOp> Op = MakeShared<FSpineTerrainCorridorOp>(GetFName());
    const ASpineGenerator* Spine = Cast<ASpineGenerator>(GetOwner());
    if (!Spine) return Op;
    const UAvenorTerrainData* Data = Spine->TerrainData.LoadSynchronous();
    if (!Data || !Data->SpineLayer.HasValidData()) return Op;
    const FAvenorBakedSpineLayer& Layer = Data->SpineLayer;
    Op->WorldBounds = Spine->GetTerrainCorridorBounds();
    Op->FlatHalfWidth = Layer.FlatHalfWidth;
    Op->DevelopmentHalfWidth = Layer.DevelopmentHalfWidth;
    Op->TransitionHalfWidth = Layer.TransitionHalfWidth;
    Op->Samples.Reserve(Layer.Samples.Num());
    for (const FAvenorBakedSpineSample& Sample : Layer.Samples)
    {
        FCorridorSample& Dest = Op->Samples.AddDefaulted_GetRef();
        Dest.Centre = Sample.Centre;
        Dest.RoadDatumZ = Sample.RoadDatumZ;
        for (float Height : Sample.LeftDevelopmentProfileZ) Dest.LeftDevelopmentProfileZ.Add(Height);
        for (float Height : Sample.RightDevelopmentProfileZ) Dest.RightDevelopmentProfileZ.Add(Height);
    }
    return Op;
}

FGuid UAvenorSpineTerrainModifier::GetCodeVersionKey() const
{
    return FGuid(TEXT("3f5c6a2e-e38b-4b55-97f0-b40ca65c1ee9"));
}

namespace
{
static const FName GeneratedWaterTag(TEXT("AvenorStripWater"));

UAvenorSpineTerrainModifier* FindModifier(ASpineGenerator* Spine)
{
    return Spine ? Spine->FindComponentByClass<UAvenorSpineTerrainModifier>() : nullptr;
}

bool CollectGeneratedWater(
    ASpineGenerator* Spine,
    TArray<FAvenorGeneratedWaterFootprint>& OutFootprints
)
{
    OutFootprints.Reset();
    UWorld* World = Spine ? Spine->GetWorld() : nullptr;
    if (!World)
    {
        return false;
    }
    constexpr float SampleSpacing = 500.0f;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor || !Actor->Tags.Contains(GeneratedWaterTag))
        {
            continue;
        }
        AWaterBodyLake* Lake = Cast<AWaterBodyLake>(Actor);
        AWaterBodyRiver* River = Cast<AWaterBodyRiver>(Actor);
        UWaterSplineComponent* Spline = Lake
            ? Lake->GetWaterBodyComponent()->GetWaterSpline()
            : River
                ? River->GetWaterBodyComponent()->GetWaterSpline()
                : nullptr;
        if (!Spline || Spline->GetNumberOfSplinePoints() < 2)
        {
            continue;
        }
        FAvenorGeneratedWaterFootprint& Footprint =
            OutFootprints.AddDefaulted_GetRef();
        Footprint.bClosed = Lake != nullptr;
        Footprint.HalfWidth = River ? 400.0f : 0.0f;
        const float Length = Spline->GetSplineLength();
        const int32 IntervalCount = FMath::Max(
            Footprint.bClosed ? 3 : 1,
            FMath::CeilToInt(Length / SampleSpacing)
        );
        const int32 PointCount = Footprint.bClosed
            ? IntervalCount
            : IntervalCount + 1;
        Footprint.Points.Reserve(PointCount);
        for (int32 Index = 0; Index < PointCount; ++Index)
        {
            const float Distance = Length
                * static_cast<float>(Index)
                / static_cast<float>(IntervalCount);
            Footprint.Points.Add(Spline->GetLocationAtDistanceAlongSpline(
                Distance, ESplineCoordinateSpace::World
            ));
        }
        if (River)
        {
            if (const UWaterSplineMetadata* Metadata = Cast<UWaterSplineMetadata>(
                    Spline->GetSplinePointsMetadata()))
            {
                if (!Metadata->RiverWidth.Points.IsEmpty())
                {
                    Footprint.HalfWidth = FMath::Max(
                        0.0f,
                        Metadata->RiverWidth.Points[0].OutVal * 0.5f
                    );
                }
            }
        }
    }
    return !OutFootprints.IsEmpty();
}
}

bool UAvenorSpineTerrainModifier::BindToSpine(ASpineGenerator* Spine)
{
    if (!Spine || Spine->AlignmentSamples.Num() < 2) return false;
    UE::MeshPartition::AMeshPartition* Partition =
        Cast<UE::MeshPartition::AMeshPartition>(Spine->MeshPartitionActor);
    if (!Partition) return false;
    UAvenorSpineTerrainModifier* Modifier = FindModifier(Spine);
    if (!Modifier)
    {
        Modifier = NewObject<UAvenorSpineTerrainModifier>(Spine, TEXT("SpineTerrainCorridor"), RF_Transactional);
        Spine->AddInstanceComponent(Modifier);
        Modifier->bIsEditorOnly = true;
        Modifier->RegisterComponent();
    }
    Modifier->Modify();
    Modifier->SetAffectedMeshPartition(nullptr);
    Modifier->BP_SetAffectedMegaMesh(Partition);
    const TArray<FName> Layers = Modifier->GetDefinitionPriorityLayers();
    if (Layers.Num() < 2)
    {
        UE_LOG(LogTemp, Error, TEXT("Avenor Spine: Mesh Partition Definition needs at least two modifier priority layers."));
        Modifier->SetAffectedMeshPartition(nullptr);
        return false;
    }
    // Shape the engineered corridor after the broad terrain operation but
    // before the final-layer refinement and native water modifiers. Putting
    // this modifier in Layers.Last() allowed it to run after lake carving and
    // refill the lake bed; rivers happened to work only because their higher
    // sub-priority carved the corridor again afterwards.
    Modifier->SetPriorityLayer(Layers[0]);
    Modifier->SetPriority(Spine->TerrainModifierPriority);
    Modifier->PostEditChange();
    Partition->Modify();
    Partition->PostEditChange();
    Partition->ReregisterAllComponents();
    Partition->MarkPackageDirty();
    Spine->MarkPackageDirty();
    return true;
}

void UAvenorSpineTerrainModifier::ClearFromSpine(ASpineGenerator* Spine)
{
    if (!Spine) return;
    if (UAvenorSpineTerrainModifier* Modifier = FindModifier(Spine))
    {
        Modifier->Modify();
        Modifier->SetAffectedMeshPartition(nullptr);
        Modifier->PostEditChange();
    }
    if (UE::MeshPartition::AMeshPartition* Partition =
        Cast<UE::MeshPartition::AMeshPartition>(Spine->MeshPartitionActor))
    {
        Partition->Modify();
        Partition->PostEditChange();
        Partition->ReregisterAllComponents();
        Partition->MarkPackageDirty();
    }
}
void RegisterAvenorSpineTerrainModifierBridge()
{
    AvenorSpineEditorBridge::BindTerrainModifier =
        UAvenorSpineTerrainModifier::BindToSpine;
    AvenorSpineEditorBridge::ClearTerrainModifier =
        UAvenorSpineTerrainModifier::ClearFromSpine;
    AvenorSpineEditorBridge::CollectGeneratedWater =
        CollectGeneratedWater;
}

void UnregisterAvenorSpineTerrainModifierBridge()
{
    AvenorSpineEditorBridge::BindTerrainModifier = nullptr;
    AvenorSpineEditorBridge::ClearTerrainModifier = nullptr;
    AvenorSpineEditorBridge::CollectGeneratedWater = nullptr;
}
