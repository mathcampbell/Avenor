#include "ParcelGenerator.h"

#include "ProceduralTerrainGenerator.h"
#include "SpineGenerator.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

AParcelGenerator::AParcelGenerator()
{
    PrimaryActorTick.bCanEverTick = false;

    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);

    auto CreateBoundaries = [this](const TCHAR* Name)
    {
        UInstancedStaticMeshComponent* Component =
            CreateDefaultSubobject<UInstancedStaticMeshComponent>(Name);
        Component->SetupAttachment(SceneRoot);
        Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        return Component;
    };

    SellableBoundaries = CreateBoundaries(TEXT("SellableBoundaries"));
    ReviewBoundaries = CreateBoundaries(TEXT("ReviewBoundaries"));
    UnavailableBoundaries = CreateBoundaries(TEXT("UnavailableBoundaries"));

    static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMeshFinder(
        TEXT("/Engine/BasicShapes/Cube.Cube")
    );
    if (CubeMeshFinder.Succeeded())
    {
        CubeMesh = CubeMeshFinder.Object;
        SellableBoundaries->SetStaticMesh(CubeMesh);
        ReviewBoundaries->SetStaticMesh(CubeMesh);
        UnavailableBoundaries->SetStaticMesh(CubeMesh);
    }
}

void AParcelGenerator::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    RegenerateParcels();
}

void AParcelGenerator::SetVisualisationMode(
    EParcelVisualisationMode NewMode
)
{
    VisualisationMode = NewMode;
    RegenerateParcels();
}

void AParcelGenerator::CycleVisualisationMode()
{
    switch (VisualisationMode)
    {
    case EParcelVisualisationMode::Hidden:
        SetVisualisationMode(EParcelVisualisationMode::All);
        break;
    case EParcelVisualisationMode::All:
        SetVisualisationMode(EParcelVisualisationMode::RestrictedOnly);
        break;
    default:
        SetVisualisationMode(EParcelVisualisationMode::Hidden);
        break;
    }
}

FVector AParcelGenerator::ToWorld(
    float Chainage,
    float Lateral,
    float Height
) const
{
    if (Spine)
    {
        FVector World = Spine->GetSpineLocationAtChainage(
            Chainage,
            Lateral
        );
        World.Z = Terrain
            ? Terrain->GetActorLocation().Z + Height
            : GetActorLocation().Z + Height;
        return World;
    }
    return GetActorTransform().TransformPosition(
        FVector(Chainage, Lateral, Height)
    );
}

FGeneratedParcel AParcelGenerator::AnalyseParcel(
    int32 AlongIndex,
    int32 DepthIndex,
    int32 Side
) const
{
    FGeneratedParcel Parcel;
    Parcel.AlongIndex = AlongIndex;
    Parcel.DepthIndex = DepthIndex;
    Parcel.Side = Side;

    const float CentreChainage =
        (static_cast<float>(AlongIndex) + 0.5f) * BlockSize;
    const float CentreLateral = static_cast<float>(Side) * (
        CorridorHalfWidth +
        (static_cast<float>(DepthIndex) + 0.5f) * BlockSize
    );

    const int32 Count = FMath::Max(3, SamplesPerSide);
    TArray<float> Heights;
    Heights.SetNumUninitialized(Count * Count);
    int32 UnderwaterSamples = 0;
    float MaximumSlope = 0.0f;

    for (int32 X = 0; X < Count; ++X)
    {
        const float XAlpha = static_cast<float>(X) / (Count - 1);
        const float Chainage = CentreChainage +
            (XAlpha - 0.5f) * BlockSize;
        for (int32 Y = 0; Y < Count; ++Y)
        {
            const float YAlpha = static_cast<float>(Y) / (Count - 1);
            const float Lateral = CentreLateral +
                (YAlpha - 0.5f) * BlockSize;
            const int32 SampleIndex = X * Count + Y;
            Heights[SampleIndex] = Terrain
                ? Terrain->GetTerrainHeightAtSpineSpace(
                    Chainage,
                    Lateral
                )
                : 0.0f;

            float WaterDepth = 0.0f;
            if (Terrain && Terrain->IsUnderwaterAtSpineSpace(
                Chainage,
                Lateral,
                WaterDepth
            ))
            {
                ++UnderwaterSamples;
            }
        }
    }

    Parcel.MinimumHeight = TNumericLimits<float>::Max();
    Parcel.MaximumHeight = -TNumericLimits<float>::Max();
    for (const float Height : Heights)
    {
        Parcel.MinimumHeight = FMath::Min(Parcel.MinimumHeight, Height);
        Parcel.MaximumHeight = FMath::Max(Parcel.MaximumHeight, Height);
    }

    const float SampleSpacing = BlockSize /
        static_cast<float>(Count - 1);
    for (int32 X = 0; X < Count; ++X)
    {
        for (int32 Y = 0; Y < Count; ++Y)
        {
            const int32 Index = X * Count + Y;
            if (X + 1 < Count)
            {
                const float Rise = FMath::Abs(
                    Heights[(X + 1) * Count + Y] - Heights[Index]
                );
                MaximumSlope = FMath::Max(
                    MaximumSlope,
                    FMath::RadiansToDegrees(FMath::Atan2(
                        Rise,
                        SampleSpacing
                    ))
                );
            }
            if (Y + 1 < Count)
            {
                const float Rise = FMath::Abs(
                    Heights[X * Count + Y + 1] - Heights[Index]
                );
                MaximumSlope = FMath::Max(
                    MaximumSlope,
                    FMath::RadiansToDegrees(FMath::Atan2(
                        Rise,
                        SampleSpacing
                    ))
                );
            }
        }
    }

    Parcel.MaximumSlopeDegrees = MaximumSlope;
    Parcel.UnderwaterFraction =
        static_cast<float>(UnderwaterSamples) /
        static_cast<float>(Count * Count);

    if (Parcel.UnderwaterFraction >= SubmergedThreshold)
    {
        Parcel.Topography = EParcelTopography::Submerged;
        Parcel.Availability = EParcelAvailability::Unavailable;
    }
    else if (Parcel.UnderwaterFraction >= WaterfrontReviewThreshold)
    {
        Parcel.Topography = Parcel.UnderwaterFraction >= 0.2f
            ? EParcelTopography::MixedLandAndWater
            : EParcelTopography::Waterfront;
        Parcel.Availability = EParcelAvailability::ManualReview;
    }
    else if (MaximumSlope >= HillsideSlopeDegrees)
    {
        Parcel.Topography = EParcelTopography::Hillside;
        Parcel.Availability = EParcelAvailability::Sellable;
    }
    else if (MaximumSlope >= RollingSlopeDegrees)
    {
        Parcel.Topography = EParcelTopography::Rolling;
        Parcel.Availability = EParcelAvailability::Sellable;
    }
    else
    {
        Parcel.Topography = EParcelTopography::Flat;
        Parcel.Availability = EParcelAvailability::Sellable;
    }

    const float CentreHeight = Terrain
        ? Terrain->GetTerrainHeightAtSpineSpace(
            CentreChainage,
            CentreLateral
        )
        : 0.0f;
    Parcel.Centre = ToWorld(
        CentreChainage,
        CentreLateral,
        CentreHeight
    );
    return Parcel;
}

void AParcelGenerator::AddBoundaryEdge(
    UInstancedStaticMeshComponent* Component,
    const FVector2D& A,
    const FVector2D& B
)
{
    if (!Component || !Terrain)
    {
        return;
    }

    const float EdgeLength = FVector2D::Distance(A, B);
    const int32 Segments = FMath::Max(
        1,
        FMath::CeilToInt(EdgeLength / BoundarySegmentLength)
    );
    for (int32 Index = 0; Index < Segments; ++Index)
    {
        const float AlphaA = static_cast<float>(Index) / Segments;
        const float AlphaB = static_cast<float>(Index + 1) / Segments;
        const FVector2D SampleA = FMath::Lerp(A, B, AlphaA);
        const FVector2D SampleB = FMath::Lerp(A, B, AlphaB);
        FVector WorldA = ToWorld(
            SampleA.X,
            SampleA.Y,
            Terrain->GetTerrainHeightAtSpineSpace(SampleA.X, SampleA.Y)
                + BoundaryHeightOffset
        );
        FVector WorldB = ToWorld(
            SampleB.X,
            SampleB.Y,
            Terrain->GetTerrainHeightAtSpineSpace(SampleB.X, SampleB.Y)
                + BoundaryHeightOffset
        );
        const FVector Delta = WorldB - WorldA;
        if (Delta.IsNearlyZero())
        {
            continue;
        }
        Component->AddInstance(
            FTransform(
                Delta.Rotation(),
                (WorldA + WorldB) * 0.5f,
                FVector(
                    Delta.Size() / 100.0f,
                    BoundaryWidth / 100.0f,
                    BoundaryWidth / 100.0f
                )
            ),
            true
        );
    }
}

void AParcelGenerator::DrawParcel(const FGeneratedParcel& Parcel)
{
    UInstancedStaticMeshComponent* Component = SellableBoundaries;
    if (Parcel.Availability == EParcelAvailability::ManualReview)
    {
        Component = ReviewBoundaries;
    }
    else if (Parcel.Availability == EParcelAvailability::Unavailable)
    {
        Component = UnavailableBoundaries;
    }

    if (VisualisationMode == EParcelVisualisationMode::RestrictedOnly &&
        Parcel.Availability == EParcelAvailability::Sellable)
    {
        return;
    }

    const float CentreChainage =
        (static_cast<float>(Parcel.AlongIndex) + 0.5f) * BlockSize;
    const float CentreLateral = static_cast<float>(Parcel.Side) * (
        CorridorHalfWidth +
        (static_cast<float>(Parcel.DepthIndex) + 0.5f) * BlockSize
    );
    const float Half = BlockSize * 0.5f;
    const FVector2D SW(CentreChainage - Half, CentreLateral - Half);
    const FVector2D SE(CentreChainage + Half, CentreLateral - Half);
    const FVector2D NE(CentreChainage + Half, CentreLateral + Half);
    const FVector2D NW(CentreChainage - Half, CentreLateral + Half);
    AddBoundaryEdge(Component, SW, SE);
    AddBoundaryEdge(Component, SE, NE);
    AddBoundaryEdge(Component, NE, NW);
    AddBoundaryEdge(Component, NW, SW);
}

void AParcelGenerator::RegenerateParcels()
{
    SellableBoundaries->ClearInstances();
    ReviewBoundaries->ClearInstances();
    UnavailableBoundaries->ClearInstances();
    Parcels.Reset();

    SellableBoundaries->SetHiddenInGame(!bVisibleInGame);
    ReviewBoundaries->SetHiddenInGame(!bVisibleInGame);
    UnavailableBoundaries->SetHiddenInGame(!bVisibleInGame);

    if (SellableMaterial)
    {
        SellableBoundaries->SetMaterial(0, SellableMaterial);
    }
    if (ReviewMaterial)
    {
        ReviewBoundaries->SetMaterial(0, ReviewMaterial);
    }
    if (UnavailableMaterial)
    {
        UnavailableBoundaries->SetMaterial(0, UnavailableMaterial);
    }

    if (!Spine || !Terrain)
    {
        return;
    }

    for (int32 Along = -BlocksWest; Along < BlocksEast; ++Along)
    {
        for (int32 Depth = 0; Depth < ParcelRowsPerSide; ++Depth)
        {
            for (const int32 Side : {-1, 1})
            {
                Parcels.Add(AnalyseParcel(Along, Depth, Side));
            }
        }
    }

    if (VisualisationMode == EParcelVisualisationMode::Hidden)
    {
        return;
    }
    for (const FGeneratedParcel& Parcel : Parcels)
    {
        DrawParcel(Parcel);
    }
}
