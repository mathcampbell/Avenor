#include "SpineGenerator.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

ASpineGenerator::ASpineGenerator()
{
    PrimaryActorTick.bCanEverTick = false;

    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);

    GuideSpline = CreateDefaultSubobject<USplineComponent>(TEXT("GuideSpline"));
    GuideSpline->SetupAttachment(SceneRoot);
    GuideSpline->SetClosedLoop(false);
    GuideSpline->ClearSplinePoints(false);
    GuideSpline->AddSplinePoint(
        FVector(-50000.0f, 0.0f, 0.0f),
        ESplineCoordinateSpace::Local,
        false
    );
    GuideSpline->AddSplinePoint(
        FVector(50000.0f, 0.0f, 0.0f),
        ESplineCoordinateSpace::Local,
        true
    );

    auto CreateInstances = [this](const TCHAR* Name)
    {
        UInstancedStaticMeshComponent* Component =
            CreateDefaultSubobject<UInstancedStaticMeshComponent>(Name);
        Component->SetupAttachment(SceneRoot);
        Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        return Component;
    };

    RoadInstances = CreateInstances(TEXT("RoadInstances"));
    MedianInstances = CreateInstances(TEXT("MedianInstances"));
    PavementInstances = CreateInstances(TEXT("PavementInstances"));
    ParcelInstances = CreateInstances(TEXT("ParcelInstances"));
    MonorailInstances = CreateInstances(TEXT("MonorailInstances"));

    static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMeshFinder(
        TEXT("/Engine/BasicShapes/Cube.Cube")
    );

    if (CubeMeshFinder.Succeeded())
    {
        CubeMesh = CubeMeshFinder.Object;
        RoadInstances->SetStaticMesh(CubeMesh);
        MedianInstances->SetStaticMesh(CubeMesh);
        PavementInstances->SetStaticMesh(CubeMesh);
        ParcelInstances->SetStaticMesh(CubeMesh);
        MonorailInstances->SetStaticMesh(CubeMesh);
    }
}

void ASpineGenerator::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    RebuildStrip();
}

float ASpineGenerator::GetMinimumChainage() const
{
    return -static_cast<float>(BlocksWest) * BlockSize;
}

float ASpineGenerator::GetMaximumChainage() const
{
    return static_cast<float>(BlocksEast) * BlockSize;
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

    // Continue along the end tangent instead of collapsing every out-of-range
    // terrain row onto the first/last spline point.
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
    if (!GuideSpline || GuideSpline->GetSplineLength() <= KINDA_SMALL_NUMBER)
    {
        return GetActorTransform();
    }

    FVector Location;
    FVector Forward;
    GetBaseSplineFrameAtChainage(Chainage, Location, Forward);

    FVector Right = FVector::CrossProduct(FVector::UpVector, Forward)
        .GetSafeNormal();
    if (Right.IsNearlyZero())
    {
        Right = FVector::RightVector;
    }

    float AppliedLateralOffset = 0.0f;
    float AppliedVerticalOffset = 0.0f;

    for (const FSpineEffector& Effector : Effectors)
    {
        if (!Effector.bEnabled || Effector.InfluenceRadius <= 0.0f)
        {
            continue;
        }

        const float NormalisedDistance =
            FMath::Abs(Chainage - Effector.Chainage) /
            Effector.InfluenceRadius;

        if (NormalisedDistance >= 1.0f)
        {
            continue;
        }

        const float SmoothFalloff =
            FMath::SmoothStep(0.0f, 1.0f, 1.0f - NormalisedDistance);
        const float Weight = FMath::Pow(
            SmoothFalloff,
            FMath::Max(0.1f, Effector.FalloffExponent)
        );

        AppliedLateralOffset += Effector.LateralOffset * Weight;
        AppliedVerticalOffset += Effector.VerticalOffset * Weight;
    }

    Location += Right * AppliedLateralOffset;
    Location.Z += AppliedVerticalOffset;

    // Sampling nearby effected locations makes rotation follow effector bends.
    const float Probe = FMath::Max(100.0f, GenerationSegmentLength * 0.25f);
    // Terrain and other systems may query well beyond the currently generated
    // road blocks. Never clamp this orientation probe to the road extent:
    // doing so reverses the tangent beyond BlocksEast and folds spine-space.
    const float AheadChainage = Chainage + Probe;
    if (!FMath::IsNearlyEqual(AheadChainage, Chainage))
    {
        FVector Ahead;
        FVector AheadBaseForward;
        GetBaseSplineFrameAtChainage(
            AheadChainage,
            Ahead,
            AheadBaseForward
        );
        FVector AheadRight = FVector::CrossProduct(
            FVector::UpVector,
            AheadBaseForward
        ).GetSafeNormal();

        float AheadLateral = 0.0f;
        float AheadVertical = 0.0f;
        for (const FSpineEffector& Effector : Effectors)
        {
            if (!Effector.bEnabled || Effector.InfluenceRadius <= 0.0f)
            {
                continue;
            }
            const float D = FMath::Abs(AheadChainage - Effector.Chainage) /
                Effector.InfluenceRadius;
            if (D < 1.0f)
            {
                const float W = FMath::Pow(
                    FMath::SmoothStep(0.0f, 1.0f, 1.0f - D),
                    FMath::Max(0.1f, Effector.FalloffExponent)
                );
                AheadLateral += Effector.LateralOffset * W;
                AheadVertical += Effector.VerticalOffset * W;
            }
        }
        Ahead += AheadRight * AheadLateral;
        Ahead.Z += AheadVertical;
        Forward = (Ahead - Location).GetSafeNormal();
    }

    return FTransform(Forward.Rotation(), Location);
}

FVector ASpineGenerator::GetSpineLocationAtChainage(
    float Chainage,
    float LateralOffset,
    float VerticalOffset
) const
{
    const FTransform SpineTransform = GetSpineTransformAtChainage(Chainage);
    return SpineTransform.GetLocation()
        + SpineTransform.GetUnitAxis(EAxis::Y) * LateralOffset
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

    // The authored guide spline can be much shorter than the generated world.
    // Extend its end tangents for reverse lookups just as
    // GetBaseSplineFrameAtChainage does for forward lookups. Without this,
    // every Landscape vertex beyond an endpoint receives the same chainage,
    // collapsing one axis of terrain noise into long flat plateaux.
    const FVector StartLocation =
        GuideSpline->GetLocationAtDistanceAlongSpline(
            0.0f,
            ESplineCoordinateSpace::World
        );
    const FVector StartDirection =
        GuideSpline->GetDirectionAtDistanceAlongSpline(
            0.0f,
            ESplineCoordinateSpace::World
        ).GetSafeNormal();
    const FVector EndLocation =
        GuideSpline->GetLocationAtDistanceAlongSpline(
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
    else if (Distance >= SplineLength - KINDA_SMALL_NUMBER &&
             BeyondEnd > 0.0f)
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

void ASpineGenerator::AddBox(
    UInstancedStaticMeshComponent* Component,
    const FVector& Location,
    const FVector& Size,
    const FQuat& Rotation
)
{
    if (!Component)
    {
        return;
    }
    Component->AddInstance(
        FTransform(Rotation, Location, Size / 100.0f),
        true
    );
}

void ASpineGenerator::AddStripSegment(
    UInstancedStaticMeshComponent* Component,
    float StartChainage,
    float EndChainage,
    float LateralOffset,
    float CentreHeight,
    float Width,
    float Thickness
)
{
    const FTransform Start = GetSpineTransformAtChainage(StartChainage);
    const FTransform End = GetSpineTransformAtChainage(EndChainage);
    FVector StartLocation = Start.GetLocation()
        + Start.GetUnitAxis(EAxis::Y) * LateralOffset;
    FVector EndLocation = End.GetLocation()
        + End.GetUnitAxis(EAxis::Y) * LateralOffset;
    StartLocation.Z += CentreHeight;
    EndLocation.Z += CentreHeight;

    const FVector Delta = EndLocation - StartLocation;
    const float Length = Delta.Size();
    if (Length <= KINDA_SMALL_NUMBER)
    {
        return;
    }

    AddBox(
        Component,
        (StartLocation + EndLocation) * 0.5f,
        FVector(Length, Width, Thickness),
        Delta.Rotation().Quaternion()
    );
}

void ASpineGenerator::RebuildStrip()
{
    RoadInstances->ClearInstances();
    MedianInstances->ClearInstances();
    PavementInstances->ClearInstances();
    ParcelInstances->ClearInstances();
    MonorailInstances->ClearInstances();

    if (!CubeMesh || !GuideSpline)
    {
        return;
    }

    const float Start = GetMinimumChainage();
    const float End = GetMaximumChainage();
    if (End <= Start)
    {
        return;
    }

    const float HalfMedian = MedianWidth * 0.5f;
    const float CarriagewayOffset = HalfMedian + CarriagewayWidth * 0.5f;
    const float PavementOffset =
        HalfMedian + CarriagewayWidth + PavementWidth * 0.5f;
    const float HalfCorridor =
        HalfMedian + CarriagewayWidth + PavementWidth;

    const float Step = FMath::Max(100.0f, GenerationSegmentLength);
    for (float S = Start; S < End; S += Step)
    {
        const float Next = FMath::Min(S + Step, End);
        AddStripSegment(
            MedianInstances, S, Next, 0.0f,
            RoadThickness * 0.5f, MedianWidth, RoadThickness
        );
        AddStripSegment(
            RoadInstances, S, Next, CarriagewayOffset,
            RoadThickness * 0.5f, CarriagewayWidth, RoadThickness
        );
        AddStripSegment(
            RoadInstances, S, Next, -CarriagewayOffset,
            RoadThickness * 0.5f, CarriagewayWidth, RoadThickness
        );
        AddStripSegment(
            PavementInstances, S, Next, PavementOffset,
            PavementThickness * 0.5f, PavementWidth, PavementThickness
        );
        AddStripSegment(
            PavementInstances, S, Next, -PavementOffset,
            PavementThickness * 0.5f, PavementWidth, PavementThickness
        );
        AddStripSegment(
            MonorailInstances, S, Next, 0.0f,
            MonorailBeamCentreHeight,
            MonorailGuidewayWidth, MonorailBeamDepth
        );
    }

    if (bShowLegacyParcelPads)
    {
        const float ParcelSize = FMath::Max(
            100.0f,
            BlockSize - ParcelGridGap
        );
        for (int32 Along = -BlocksWest; Along < BlocksEast; ++Along)
        {
            const float Chainage =
                (static_cast<float>(Along) + 0.5f) * BlockSize;
            const FTransform Spine = GetSpineTransformAtChainage(Chainage);
            for (int32 Depth = 0; Depth < ParcelRowsPerSide; ++Depth)
            {
                const float Offset =
                    HalfCorridor +
                    (static_cast<float>(Depth) + 0.5f) * BlockSize;
                for (const float Side : {-1.0f, 1.0f})
                {
                    FVector Location = GetSpineLocationAtChainage(
                        Chainage,
                        Side * Offset,
                        ParcelPadThickness * 0.5f
                    );
                    AddBox(
                        ParcelInstances,
                        Location,
                        FVector(
                            ParcelSize,
                            ParcelSize,
                            ParcelPadThickness
                        ),
                        Spine.GetRotation()
                    );
                }
            }
        }
    }

    const float SupportHeight = FMath::Max(
        100.0f,
        MonorailBeamCentreHeight - MonorailBeamDepth * 0.5f
    );
    const int32 FirstSupport = FMath::CeilToInt(Start / SupportSpacing);
    const int32 LastSupport = FMath::FloorToInt(End / SupportSpacing);
    for (int32 Index = FirstSupport; Index <= LastSupport; ++Index)
    {
        const float Chainage = static_cast<float>(Index) * SupportSpacing;
        const FTransform Spine = GetSpineTransformAtChainage(Chainage);
        FVector Location = Spine.GetLocation();
        Location.Z += SupportHeight * 0.5f;
        AddBox(
            MonorailInstances,
            Location,
            FVector(SupportWidth, SupportWidth, SupportHeight),
            Spine.GetRotation()
        );
    }
}
