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
        MountainRadii.Add(Random.FRandRange(600000.0f, 1200000.0f));
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
            // Quintic-style smooth transition: zero slope both at the
            // foothill boundary and around the broad summit.
            const float Envelope = 1.0f - FMath::SmoothStep(
                0.0f,
                1.0f,
                NormalisedDistance
            );
            const float RidgeNoise = 0.85f + 0.15f * FMath::Abs(
                FMath::PerlinNoise2D(
                    (P + SeedOffset * (Index + 3.0f)) / 250000.0f
                )
            );
            const float DistantMountainFactor = FMath::SmoothStep(
                MountainMinimumDistance * 0.75f,
                MountainMinimumDistance,
                Distance
            );
            Height += MountainHeights[Index] * Envelope * RidgeNoise *
                DistantMountainFactor;
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

float AProceduralTerrainGenerator::GetTerrainHeightAtSpineSpace(
    float Chainage,
    float Lateral
) const
{
    return EvaluateTerrainHeight(Chainage, Lateral);
}

bool AProceduralTerrainGenerator::GetWaterSurfaceAtSpineSpace(
    float Chainage,
    float Lateral,
    float& OutSurfaceHeight
) const
{
    const FVector2D Point(Chainage, Lateral);
    bool bFoundWater = false;
    float HighestSurface = -TNumericLimits<float>::Max();

    for (const FGeneratedWatercourse& Watercourse : Watercourses)
    {
        const float LakeDistance = FVector2D::Distance(
            Point,
            Watercourse.LakeCentre
        );
        if (LakeDistance <= Watercourse.LakeRadius)
        {
            const float LakeSurface = Watercourse.LakeSurfaceHeight;
            HighestSurface = FMath::Max(HighestSurface, LakeSurface);
            bFoundWater = true;
        }

        for (int32 Index = 0;
             Index + 1 < Watercourse.SpineSpacePoints.Num();
             ++Index)
        {
            float Alpha = 0.0f;
            const FVector2D& A = Watercourse.SpineSpacePoints[Index];
            const FVector2D& B = Watercourse.SpineSpacePoints[Index + 1];
            const float Distance = DistanceToSegment(Point, A, B, Alpha);
            if (Distance <= Watercourse.Width * 0.5f)
            {
                const float RiverSurface =
                    Watercourse.SurfaceHeights.IsValidIndex(Index + 1)
                    ? FMath::Lerp(
                        Watercourse.SurfaceHeights[Index],
                        Watercourse.SurfaceHeights[Index + 1],
                        Alpha
                    )
                    : EvaluateBaseHeight(Point.X, Point.Y) -
                        RiverCarveDepth * 0.35f;
                HighestSurface = FMath::Max(HighestSurface, RiverSurface);
                bFoundWater = true;
            }
        }
    }

    OutSurfaceHeight = bFoundWater ? HighestSurface : 0.0f;
    return bFoundWater;
}

bool AProceduralTerrainGenerator::IsUnderwaterAtSpineSpace(
    float Chainage,
    float Lateral,
    float& OutDepth
) const
{
    float SurfaceHeight = 0.0f;
    if (!GetWaterSurfaceAtSpineSpace(
        Chainage,
        Lateral,
        SurfaceHeight
    ))
    {
        OutDepth = 0.0f;
        return false;
    }

    OutDepth = FMath::Max(
        0.0f,
        SurfaceHeight - EvaluateTerrainHeight(Chainage, Lateral)
    );
    return OutDepth > 1.0f;
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
        FVector2D Current = FVector2D::ZeroVector;
        float CurrentHeight = -TNumericLimits<float>::Max();

        // Pick a genuinely high source in the outer hill country.
        for (int32 CandidateIndex = 0; CandidateIndex < 64; ++CandidateIndex)
        {
            const FVector2D Candidate(
                Random.FRandRange(-Length * 0.48f, Length * 0.48f),
                SourceSide * Random.FRandRange(
                    HalfWidth * 0.65f,
                    HalfWidth * 0.95f
                )
            );
            const float Height = EvaluateBaseHeight(
                Candidate.X,
                Candidate.Y
            );
            if (Height > CurrentHeight)
            {
                Current = Candidate;
                CurrentHeight = Height;
            }
        }

        const float Step = FMath::Max(CellSize * 2.0f, 20000.0f);
        FVector2D PreviousDirection(0.0f, -SourceSide);
        TSet<FIntPoint> Visited;

        for (int32 PointIndex = 0;
             PointIndex < RiverControlPointCount;
             ++PointIndex)
        {
            const FIntPoint Cell(
                FMath::RoundToInt(Current.X / Step),
                FMath::RoundToInt(Current.Y / Step)
            );
            if (Visited.Contains(Cell))
            {
                break;
            }
            Visited.Add(Cell);

            Watercourse.SpineSpacePoints.Add(Current);
            const float DesiredSurface =
                EvaluateBaseHeight(Current.X, Current.Y) -
                RiverCarveDepth * 0.35f;
            const float Surface = Watercourse.SurfaceHeights.IsEmpty()
                ? DesiredSurface
                : FMath::Min(
                    DesiredSurface,
                    Watercourse.SurfaceHeights.Last() - 5.0f
                );
            Watercourse.SurfaceHeights.Add(Surface);

            if (FMath::Abs(Current.X) >= Length * 0.49f ||
                FMath::Abs(Current.Y) >= HalfWidth * 0.98f)
            {
                break;
            }

            FVector2D BestPoint = Current;
            FVector2D BestDirection = PreviousDirection;
            float BestHeight = TNumericLimits<float>::Max();
            float BestScore = TNumericLimits<float>::Max();
            constexpr int32 DirectionSamples = 24;
            for (int32 DirectionIndex = 0;
                 DirectionIndex < DirectionSamples;
                 ++DirectionIndex)
            {
                const float Angle =
                    2.0f * PI * DirectionIndex / DirectionSamples;
                const FVector2D Direction(
                    FMath::Cos(Angle),
                    FMath::Sin(Angle)
                );
                const FVector2D Candidate = Current + Direction * Step;
                if (FMath::Abs(Candidate.X) > Length * 0.5f ||
                    FMath::Abs(Candidate.Y) > HalfWidth)
                {
                    continue;
                }
                const float Height = EvaluateBaseHeight(
                    Candidate.X,
                    Candidate.Y
                );
                const float TurnPenalty =
                    (1.0f - FVector2D::DotProduct(
                        PreviousDirection,
                        Direction
                    )) * 250.0f;
                const float Score = Height + TurnPenalty;
                if (Score < BestScore)
                {
                    BestScore = Score;
                    BestHeight = Height;
                    BestPoint = Candidate;
                    BestDirection = Direction;
                }
            }

            if (BestHeight < CurrentHeight - 10.0f)
            {
                Current = BestPoint;
                CurrentHeight = BestHeight;
                PreviousDirection = BestDirection;
                continue;
            }

            // A local depression becomes a lake. Search expanding rings for
            // its lowest spill route, then continue the river from there.
            FVector2D SpillPoint = Current;
            float SpillHeight = TNumericLimits<float>::Max();
            float SpillRadius = 0.0f;
            for (int32 Ring = 2; Ring <= 14; ++Ring)
            {
                const float Radius = Step * Ring;
                FVector2D RingBest = Current;
                float RingBestHeight = TNumericLimits<float>::Max();
                for (int32 Sample = 0; Sample < 32; ++Sample)
                {
                    const float Angle = 2.0f * PI * Sample / 32.0f;
                    const FVector2D Candidate = Current + FVector2D(
                        FMath::Cos(Angle),
                        FMath::Sin(Angle)
                    ) * Radius;
                    if (FMath::Abs(Candidate.X) > Length * 0.5f ||
                        FMath::Abs(Candidate.Y) > HalfWidth)
                    {
                        continue;
                    }
                    const float Height = EvaluateBaseHeight(
                        Candidate.X,
                        Candidate.Y
                    );
                    if (Height < RingBestHeight)
                    {
                        RingBestHeight = Height;
                        RingBest = Candidate;
                    }
                }
                if (RingBestHeight < SpillHeight)
                {
                    SpillHeight = RingBestHeight;
                    SpillPoint = RingBest;
                    SpillRadius = Radius;
                }
                if (RingBestHeight < CurrentHeight)
                {
                    break;
                }
            }

            if (Watercourse.LakeRadius <= 0.0f)
            {
                Watercourse.LakeCentre = Current;
                Watercourse.LakeRadius = FMath::Clamp(
                    SpillRadius * 0.55f,
                    35000.0f,
                    180000.0f
                );
                Watercourse.LakeSurfaceHeight = FMath::Max(
                    CurrentHeight,
                    SpillHeight
                );
            }

            if (SpillPoint.Equals(Current, Step * 0.25f))
            {
                break;
            }
            PreviousDirection = (SpillPoint - Current).GetSafeNormal();
            Current = SpillPoint;
            CurrentHeight = SpillHeight;
        }

        if (Watercourse.SpineSpacePoints.Num() >= 2)
        {
            Watercourses.Add(MoveTemp(Watercourse));
        }
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
            // Unreal uses clockwise front-face winding. Keep terrain normals up.
            Triangles.Append({A, C, B, A, D, C});
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
                Watercourse.SurfaceHeights.IsValidIndex(Index)
                ? Watercourse.SurfaceHeights[Index]
                : EvaluateBaseHeight(Point.X, Point.Y) -
                    RiverCarveDepth * 0.35f;
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
            Triangles.Append({A, A + 3, A + 2, A, A + 1, A + 3});
        }

        if (Watercourse.LakeRadius <= 0.0f)
        {
            continue;
        }

        const int32 LakeSegments = 32;
        const int32 CentreIndex = Vertices.Num();
        const float LakeHeight = Watercourse.LakeSurfaceHeight;
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
                CentreIndex + 1 + (Segment + 1) % LakeSegments,
                CentreIndex + 1 + Segment
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
