#include "ProceduralTerrainGenerator.h"

#include "ProceduralMeshComponent.h"
#include "SpineGenerator.h"
#include "Components/SceneComponent.h"
#include "KismetProceduralMeshLibrary.h"
#include "Materials/MaterialInterface.h"

AProceduralTerrainGenerator::AProceduralTerrainGenerator()
{
    PrimaryActorTick.bCanEverTick = false;

    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);

    GeneratedMesh = CreateDefaultSubobject<UProceduralMeshComponent>(
        TEXT("GeneratedMesh")
    );
    GeneratedMesh->SetupAttachment(SceneRoot);
    GeneratedMesh->bUseAsyncCooking = true;
    GeneratedMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
}

void AProceduralTerrainGenerator::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    if (bRegenerateOnConstruction)
    {
        Regenerate();
    }
}

void AProceduralTerrainGenerator::ClearGeneratedTerrain()
{
    GeneratedMesh->ClearAllMeshSections();
    Watercourses.Reset();
    MountainCentres.Reset();
    MountainRadii.Reset();
    MountainHeights.Reset();
}

void AProceduralTerrainGenerator::Regenerate()
{
    ClearGeneratedTerrain();

    FRandomStream Random(WorldSeed);
    for (int32 Index = 0; Index < MountainRegionCount; ++Index)
    {
        const float Side = Random.RandRange(0, 1) == 0 ? -1.0f : 1.0f;
        const float AvailableDistance = FMath::Max(
            10000.0f,
            HalfWidth - MountainMinimumDistance
        );
        MountainCentres.Add(FVector2D(
            Random.FRandRange(-Length * 0.5f, Length * 0.5f),
            Side * (
                MountainMinimumDistance +
                Random.FRandRange(0.0f, AvailableDistance)
            )
        ));
        MountainRadii.Add(Random.FRandRange(180000.0f, 420000.0f));
        MountainHeights.Add(
            MountainRelief * Random.FRandRange(0.65f, 1.25f)
        );
    }

    GenerateWatercourses();
    BuildTerrainMesh();
    BuildWaterMesh();
}

float AProceduralTerrainGenerator::EvaluateBaseHeight(
    float Chainage,
    float Lateral
) const
{
    const FVector2D SeedOffset(
        static_cast<float>(WorldSeed) * 13.17f,
        static_cast<float>(WorldSeed) * -7.91f
    );
    const FVector2D P(Chainage, Lateral);

    const float Broad = FMath::PerlinNoise2D(
        (P + SeedOffset) / 600000.0f
    );
    const float Medium = FMath::PerlinNoise2D(
        (P + SeedOffset * 1.73f) / 180000.0f
    );
    const float Detail = FMath::PerlinNoise2D(
        (P + SeedOffset * 2.41f) / 65000.0f
    );

    const float Distance = FMath::Abs(Lateral);
    const float HillFactor = FMath::SmoothStep(
        LowlandCoreDistance,
        FMath::Max(LowlandCoreDistance + 1.0f, LowlandTransitionEnd),
        Distance
    );
    const float Relief = FMath::Lerp(
        LowlandRelief,
        HillRelief,
        HillFactor
    );

    float Height =
        Broad * Relief * 0.62f +
        Medium * Relief * 0.28f +
        Detail * Relief * 0.10f;

    for (int32 Index = 0; Index < MountainCentres.Num(); ++Index)
    {
        const FVector2D Delta = P - MountainCentres[Index];
        const float Radius = MountainRadii[Index];
        const float NormalisedDistance = Delta.Size() / Radius;
        if (NormalisedDistance < 1.0f)
        {
            const float Envelope = FMath::Square(
                1.0f - FMath::Square(NormalisedDistance)
            );
            const float RidgeNoise = 0.68f + 0.32f * FMath::Abs(
                FMath::PerlinNoise2D(
                    (P + SeedOffset * (Index + 3.0f)) / 90000.0f
                )
            );
            Height += MountainHeights[Index] * Envelope * RidgeNoise;
        }
    }
    return Height;
}

float AProceduralTerrainGenerator::DistanceToSegment(
    const FVector2D& Point,
    const FVector2D& A,
    const FVector2D& B,
    float& OutAlpha
) const
{
    const FVector2D Segment = B - A;
    const float LengthSquared = Segment.SizeSquared();
    OutAlpha = LengthSquared > KINDA_SMALL_NUMBER
        ? FMath::Clamp(
            FVector2D::DotProduct(Point - A, Segment) / LengthSquared,
            0.0f,
            1.0f
        )
        : 0.0f;
    return FVector2D::Distance(Point, A + Segment * OutAlpha);
}

float AProceduralTerrainGenerator::EvaluateTerrainHeight(
    float Chainage,
    float Lateral
) const
{
    float Height = EvaluateBaseHeight(Chainage, Lateral);
    const FVector2D Point(Chainage, Lateral);

    for (const FGeneratedWatercourse& Watercourse : Watercourses)
    {
        for (int32 Index = 0;
             Index + 1 < Watercourse.SpineSpacePoints.Num();
             ++Index)
        {
            float Alpha = 0.0f;
            const float Distance = DistanceToSegment(
                Point,
                Watercourse.SpineSpacePoints[Index],
                Watercourse.SpineSpacePoints[Index + 1],
                Alpha
            );
            const float ValleyWidth = Watercourse.Width * 5.0f;
            if (Distance < ValleyWidth)
            {
                const float Weight = FMath::Square(1.0f - Distance / ValleyWidth);
                Height -= RiverCarveDepth * Weight;
            }
        }

        const float LakeDistance = FVector2D::Distance(
            Point,
            Watercourse.LakeCentre
        );
        if (LakeDistance < Watercourse.LakeRadius * 1.35f)
        {
            const float Weight = 1.0f - FMath::SmoothStep(
                0.0f,
                1.0f,
                LakeDistance / (Watercourse.LakeRadius * 1.35f)
            );
            const float LakeFloor = EvaluateBaseHeight(
                Watercourse.LakeCentre.X,
                Watercourse.LakeCentre.Y
            ) - RiverCarveDepth * 2.5f;
            Height = FMath::Lerp(Height, LakeFloor, Weight);
        }
    }
    return Height;
}

FVector AProceduralTerrainGenerator::SpineSpaceToWorld(
    float Chainage,
    float Lateral,
    float Height
) const
{
    if (Spine)
    {
        FVector World = Spine->GetSpineLocationAtChainage(
            Chainage,
            Lateral,
            0.0f
        );
        World.Z = GetActorLocation().Z + Height;
        return World;
    }
    return GetActorTransform().TransformPosition(
        FVector(Chainage, Lateral, Height)
    );
}

void AProceduralTerrainGenerator::GenerateWatercourses()
{
    FRandomStream Random(WorldSeed ^ 0x6A09E667);
    for (int32 RiverIndex = 0; RiverIndex < RiverCount; ++RiverIndex)
    {
        FGeneratedWatercourse Watercourse;
        Watercourse.Width = RiverBaseWidth *
            Random.FRandRange(0.75f, 1.55f);

        const float SourceSide = RiverIndex % 2 == 0 ? 1.0f : -1.0f;
        const float CrossingChainage = Random.FRandRange(
            -Length * 0.42f,
            Length * 0.42f
        );

        for (int32 PointIndex = 0;
             PointIndex < RiverControlPointCount;
             ++PointIndex)
        {
            const float Alpha = static_cast<float>(PointIndex) /
                static_cast<float>(RiverControlPointCount - 1);
            const float Lateral = FMath::Lerp(
                SourceSide * HalfWidth,
                -SourceSide * HalfWidth,
                Alpha
            );
            const float MeanderStrength =
                FMath::Sin(Alpha * PI) * Length * 0.16f;
            const float Meander =
                FMath::Sin(
                    Alpha * PI * Random.FRandRange(2.0f, 4.5f) +
                    Random.FRandRange(-PI, PI)
                ) * MeanderStrength;
            Watercourse.SpineSpacePoints.Add(FVector2D(
                CrossingChainage + Meander,
                Lateral
            ));
        }

        const int32 LakePoint = FMath::Clamp(
            RiverControlPointCount / 3,
            1,
            RiverControlPointCount - 2
        );
        Watercourse.LakeCentre =
            Watercourse.SpineSpacePoints[LakePoint];
        Watercourse.LakeRadius =
            Random.FRandRange(45000.0f, 120000.0f);
        Watercourses.Add(MoveTemp(Watercourse));
    }
}

void AProceduralTerrainGenerator::BuildTerrainMesh()
{
    const int32 XCount = FMath::Max(2, FMath::FloorToInt(Length / CellSize) + 1);
    const int32 YCount = FMath::Max(
        2,
        FMath::FloorToInt((HalfWidth * 2.0f) / CellSize) + 1
    );

    TArray<FVector> Vertices;
    TArray<int32> Triangles;
    TArray<FVector> Normals;
    TArray<FVector2D> UVs;
    TArray<FColor> Colours;
    TArray<FProcMeshTangent> Tangents;
    Vertices.Reserve(XCount * YCount);
    UVs.Reserve(XCount * YCount);

    for (int32 X = 0; X < XCount; ++X)
    {
        const float Chainage = -Length * 0.5f + X * CellSize;
        for (int32 Y = 0; Y < YCount; ++Y)
        {
            const float Lateral = -HalfWidth + Y * CellSize;
            const float Height = EvaluateTerrainHeight(Chainage, Lateral);
            const FVector World = SpineSpaceToWorld(
                Chainage,
                Lateral,
                Height
            );
            Vertices.Add(GetActorTransform().InverseTransformPosition(World));
            UVs.Add(FVector2D(
                static_cast<float>(X) / (XCount - 1),
                static_cast<float>(Y) / (YCount - 1)
            ));
        }
    }

    for (int32 X = 0; X + 1 < XCount; ++X)
    {
        for (int32 Y = 0; Y + 1 < YCount; ++Y)
        {
            const int32 A = X * YCount + Y;
            const int32 B = (X + 1) * YCount + Y;
            const int32 C = (X + 1) * YCount + Y + 1;
            const int32 D = X * YCount + Y + 1;
            Triangles.Append({A, B, C, A, C, D});
        }
    }

    UKismetProceduralMeshLibrary::CalculateTangentsForMesh(
        Vertices, Triangles, UVs, Normals, Tangents
    );
    GeneratedMesh->CreateMeshSection(
        0, Vertices, Triangles, Normals, UVs, Colours, Tangents, true
    );
    if (TerrainMaterial)
    {
        GeneratedMesh->SetMaterial(0, TerrainMaterial);
    }
}

void AProceduralTerrainGenerator::BuildWaterMesh()
{
    TArray<FVector> Vertices;
    TArray<int32> Triangles;
    TArray<FVector> Normals;
    TArray<FVector2D> UVs;
    TArray<FColor> Colours;
    TArray<FProcMeshTangent> Tangents;

    for (const FGeneratedWatercourse& Watercourse : Watercourses)
    {
        for (int32 Index = 0;
             Index < Watercourse.SpineSpacePoints.Num();
             ++Index)
        {
            const FVector2D Point = Watercourse.SpineSpacePoints[Index];
            const FVector2D Direction =
                Index + 1 < Watercourse.SpineSpacePoints.Num()
                ? (Watercourse.SpineSpacePoints[Index + 1] - Point).GetSafeNormal()
                : (Point - Watercourse.SpineSpacePoints[Index - 1]).GetSafeNormal();
            const FVector2D Across(-Direction.Y, Direction.X);
            const float Height =
                EvaluateBaseHeight(Point.X, Point.Y) - RiverCarveDepth * 0.35f;
            for (const float Side : {-1.0f, 1.0f})
            {
                const FVector2D Edge =
                    Point + Across * Watercourse.Width * 0.5f * Side;
                Vertices.Add(GetActorTransform().InverseTransformPosition(
                    SpineSpaceToWorld(Edge.X, Edge.Y, Height)
                ));
                UVs.Add(FVector2D(
                    static_cast<float>(Index),
                    Side > 0.0f ? 1.0f : 0.0f
                ));
            }
        }

        const int32 RiverStart = Vertices.Num() -
            Watercourse.SpineSpacePoints.Num() * 2;
        for (int32 Index = 0;
             Index + 1 < Watercourse.SpineSpacePoints.Num();
             ++Index)
        {
            const int32 A = RiverStart + Index * 2;
            Triangles.Append({A, A + 2, A + 3, A, A + 3, A + 1});
        }

        const int32 LakeSegments = 32;
        const int32 CentreIndex = Vertices.Num();
        const float LakeHeight = EvaluateBaseHeight(
            Watercourse.LakeCentre.X,
            Watercourse.LakeCentre.Y
        ) - RiverCarveDepth * 0.35f;
        Vertices.Add(GetActorTransform().InverseTransformPosition(
            SpineSpaceToWorld(
                Watercourse.LakeCentre.X,
                Watercourse.LakeCentre.Y,
                LakeHeight
            )
        ));
        UVs.Add(FVector2D(0.5f, 0.5f));
        for (int32 Segment = 0; Segment < LakeSegments; ++Segment)
        {
            const float Angle = 2.0f * PI * Segment / LakeSegments;
            const FVector2D Edge =
                Watercourse.LakeCentre +
                FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) *
                Watercourse.LakeRadius;
            Vertices.Add(GetActorTransform().InverseTransformPosition(
                SpineSpaceToWorld(Edge.X, Edge.Y, LakeHeight)
            ));
            UVs.Add(FVector2D(
                0.5f + FMath::Cos(Angle) * 0.5f,
                0.5f + FMath::Sin(Angle) * 0.5f
            ));
        }
        for (int32 Segment = 0; Segment < LakeSegments; ++Segment)
        {
            Triangles.Append({
                CentreIndex,
                CentreIndex + 1 + Segment,
                CentreIndex + 1 + (Segment + 1) % LakeSegments
            });
        }
    }

    UKismetProceduralMeshLibrary::CalculateTangentsForMesh(
        Vertices, Triangles, UVs, Normals, Tangents
    );
    GeneratedMesh->CreateMeshSection(
        1, Vertices, Triangles, Normals, UVs, Colours, Tangents, false
    );
    if (WaterMaterial)
    {
        GeneratedMesh->SetMaterial(1, WaterMaterial);
    }
}
