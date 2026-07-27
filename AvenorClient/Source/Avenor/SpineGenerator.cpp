#include "SpineGenerator.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

ASpineGenerator::ASpineGenerator()
{
    PrimaryActorTick.bCanEverTick = false;

    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);

    RoadInstances =
        CreateDefaultSubobject<UInstancedStaticMeshComponent>(
            TEXT("RoadInstances")
        );
    RoadInstances->SetupAttachment(SceneRoot);

    MedianInstances =
        CreateDefaultSubobject<UInstancedStaticMeshComponent>(
            TEXT("MedianInstances")
        );
    MedianInstances->SetupAttachment(SceneRoot);

    PavementInstances =
        CreateDefaultSubobject<UInstancedStaticMeshComponent>(
            TEXT("PavementInstances")
        );
    PavementInstances->SetupAttachment(SceneRoot);

    ParcelInstances =
        CreateDefaultSubobject<UInstancedStaticMeshComponent>(
            TEXT("ParcelInstances")
        );
    ParcelInstances->SetupAttachment(SceneRoot);

    MonorailInstances =
        CreateDefaultSubobject<UInstancedStaticMeshComponent>(
            TEXT("MonorailInstances")
        );
    MonorailInstances->SetupAttachment(SceneRoot);

    RoadInstances->SetCollisionEnabled(
        ECollisionEnabled::QueryAndPhysics
    );

    MedianInstances->SetCollisionEnabled(
        ECollisionEnabled::QueryAndPhysics
    );

    PavementInstances->SetCollisionEnabled(
        ECollisionEnabled::QueryAndPhysics
    );

    ParcelInstances->SetCollisionEnabled(
        ECollisionEnabled::QueryAndPhysics
    );

    MonorailInstances->SetCollisionEnabled(
        ECollisionEnabled::QueryAndPhysics
    );

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

void ASpineGenerator::AddBox(
    UInstancedStaticMeshComponent* Component,
    const FVector& Location,
    const FVector& Size
)
{
    if (!Component)
    {
        return;
    }

    const FTransform BoxTransform(
        FRotator::ZeroRotator,
        Location,
        Size / 100.0f
    );

    Component->AddInstance(BoxTransform);
}

void ASpineGenerator::RebuildStrip()
{
    RoadInstances->ClearInstances();
    MedianInstances->ClearInstances();
    PavementInstances->ClearInstances();
    ParcelInstances->ClearInstances();
    MonorailInstances->ClearInstances();

    if (!CubeMesh)
    {
        return;
    }

    const float StartX =
        -static_cast<float>(BlocksWest) * BlockSize;

    const float EndX =
        static_cast<float>(BlocksEast) * BlockSize;

    const float GeneratedLength = EndX - StartX;

    if (GeneratedLength <= 0.0f)
    {
        return;
    }

    const float CentreX = (StartX + EndX) * 0.5f;

    const float HalfMedianWidth = MedianWidth * 0.5f;
    const float HalfCarriagewayWidth = CarriagewayWidth * 0.5f;
    const float HalfPavementWidth = PavementWidth * 0.5f;

    const float CarriagewayOffset =
        HalfMedianWidth +
        HalfCarriagewayWidth;

    const float PavementOffset =
        HalfMedianWidth +
        CarriagewayWidth +
        HalfPavementWidth;

    const float HalfCorridorWidth =
        HalfMedianWidth +
        CarriagewayWidth +
        PavementWidth;

    /*
     * Central median.
     */
    AddBox(
        MedianInstances,
        FVector(
            CentreX,
            0.0f,
            RoadThickness * 0.5f
        ),
        FVector(
            GeneratedLength,
            MedianWidth,
            RoadThickness
        )
    );

    /*
     * North and south carriageways.
     */
    AddBox(
        RoadInstances,
        FVector(
            CentreX,
            CarriagewayOffset,
            RoadThickness * 0.5f
        ),
        FVector(
            GeneratedLength,
            CarriagewayWidth,
            RoadThickness
        )
    );

    AddBox(
        RoadInstances,
        FVector(
            CentreX,
            -CarriagewayOffset,
            RoadThickness * 0.5f
        ),
        FVector(
            GeneratedLength,
            CarriagewayWidth,
            RoadThickness
        )
    );

    /*
     * Five-metre pavements at the outer edges.
     */
    AddBox(
        PavementInstances,
        FVector(
            CentreX,
            PavementOffset,
            PavementThickness * 0.5f
        ),
        FVector(
            GeneratedLength,
            PavementWidth,
            PavementThickness
        )
    );

    AddBox(
        PavementInstances,
        FVector(
            CentreX,
            -PavementOffset,
            PavementThickness * 0.5f
        ),
        FVector(
            GeneratedLength,
            PavementWidth,
            PavementThickness
        )
    );

    /*
     * The underlying 100 m parcel registry grid.
     *
     * Positive Y is north.
     * Negative Y is south.
     */
    const float ParcelCellSize =
        FMath::Max(100.0f, BlockSize - ParcelGridGap);

    for (
        int32 AlongIndex = -BlocksWest;
        AlongIndex < BlocksEast;
        ++AlongIndex
    )
    {
        const float ParcelX =
            (static_cast<float>(AlongIndex) + 0.5f) *
            BlockSize;

        for (
            int32 DepthIndex = 0;
            DepthIndex < ParcelRowsPerSide;
            ++DepthIndex
        )
        {
            const float DistanceFromMain =
                HalfCorridorWidth +
                (static_cast<float>(DepthIndex) + 0.5f) *
                BlockSize;

            AddBox(
                ParcelInstances,
                FVector(
                    ParcelX,
                    DistanceFromMain,
                    ParcelPadThickness * 0.5f
                ),
                FVector(
                    ParcelCellSize,
                    ParcelCellSize,
                    ParcelPadThickness
                )
            );

            AddBox(
                ParcelInstances,
                FVector(
                    ParcelX,
                    -DistanceFromMain,
                    ParcelPadThickness * 0.5f
                ),
                FVector(
                    ParcelCellSize,
                    ParcelCellSize,
                    ParcelPadThickness
                )
            );
        }
    }

    /*
     * Temporary elevated monorail guideway.
     */
    AddBox(
        MonorailInstances,
        FVector(
            CentreX,
            0.0f,
            MonorailBeamCentreHeight
        ),
        FVector(
            GeneratedLength,
            MonorailGuidewayWidth,
            MonorailBeamDepth
        )
    );

    /*
     * Columns rise from the median to the underside of the beam.
     */
    const float SupportHeight =
        FMath::Max(
            100.0f,
            MonorailBeamCentreHeight -
            MonorailBeamDepth * 0.5f
        );

    const int32 FirstSupportIndex =
        FMath::CeilToInt(StartX / SupportSpacing);

    const int32 LastSupportIndex =
        FMath::FloorToInt(EndX / SupportSpacing);

    for (
        int32 SupportIndex = FirstSupportIndex;
        SupportIndex <= LastSupportIndex;
        ++SupportIndex
    )
    {
        const float SupportX =
            static_cast<float>(SupportIndex) *
            SupportSpacing;

        AddBox(
            MonorailInstances,
            FVector(
                SupportX,
                0.0f,
                SupportHeight * 0.5f
            ),
            FVector(
                SupportWidth,
                SupportWidth,
                SupportHeight
            )
        );
    }
}