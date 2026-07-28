#include "ProceduralTerrainGenerator.h"

#include "SpineGenerator.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "PCGComponent.h"
#include "PCGGraph.h"

AProceduralTerrainGenerator::AProceduralTerrainGenerator()
{
    PrimaryActorTick.bCanEverTick = false;

    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);

    PCGBounds = CreateDefaultSubobject<UBoxComponent>(TEXT("PCGBounds"));
    PCGBounds->SetupAttachment(SceneRoot);
    PCGBounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    PCGBounds->SetHiddenInGame(true);

    TerrainPCG = CreateDefaultSubobject<UPCGComponent>(TEXT("TerrainPCG"));
    WaterPCG = CreateDefaultSubobject<UPCGComponent>(TEXT("WaterPCG"));
    VegetationPCG = CreateDefaultSubobject<UPCGComponent>(
        TEXT("VegetationPCG")
    );
    InfrastructurePCG = CreateDefaultSubobject<UPCGComponent>(
        TEXT("InfrastructurePCG")
    );

    ConfigurePCGComponent(TerrainPCG, nullptr);
    ConfigurePCGComponent(WaterPCG, nullptr);
    ConfigurePCGComponent(VegetationPCG, nullptr);
    ConfigurePCGComponent(InfrastructurePCG, nullptr);
}

void AProceduralTerrainGenerator::ConfigurePCGComponent(
    UPCGComponent* Component,
    UPCGGraphInterface* Graph
)
{
    if (!Component)
    {
        return;
    }

    Component->Seed = WorldSeed;
    Component->bIsComponentPartitioned = true;
    Component->bRegenerateInEditor = false;
    if (Graph)
    {
        Component->SetGraphLocal(Graph);
    }
}

void AProceduralTerrainGenerator::GenerateWorldRules()
{
    MountainCentres.Reset();
    MountainRadii.Reset();
    MountainHeights.Reset();
    Watercourses.Reset();

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
    PCGBounds->SetBoxExtent(FVector(
        Length * 0.5f,
        HalfWidth,
        FMath::Max(200000.0f, MountainRelief * 2.0f)
    ));
}

void AProceduralTerrainGenerator::Regenerate()
{
#if WITH_EDITOR
    if (IsRunningGame())
    {
        UE_LOG(
            LogTemp,
            Warning,
            TEXT("Avenor world regeneration is an editor operation.")
        );
        return;
    }

    Modify();
    GenerateWorldRules();
    RegenerateTerrain();
    RegenerateWater();
    RegenerateVegetation();
    RegenerateInfrastructure();

    UE_LOG(
        LogTemp,
        Display,
        TEXT(
            "Avenor world definition regenerated. Native Landscape and Water "
            "actors were left intact."
        )
    );
#endif
}

void AProceduralTerrainGenerator::GeneratePCGComponent(
    UPCGComponent* Component,
    UPCGGraphInterface* Graph,
    const TCHAR* DisplayName
)
{
#if WITH_EDITOR
    if (!Component || !Graph)
    {
        UE_LOG(
            LogTemp,
            Display,
            TEXT("Avenor PCG skipped %s: no graph assigned."),
            DisplayName
        );
        return;
    }

    ConfigurePCGComponent(Component, Graph);
    Component->CleanupLocal(true);
    Component->GenerateLocal(true);
#endif
}

void AProceduralTerrainGenerator::RegenerateTerrain()
{
    GeneratePCGComponent(TerrainPCG, TerrainGraph, TEXT("Terrain"));
}

void AProceduralTerrainGenerator::RegenerateWater()
{
    GeneratePCGComponent(WaterPCG, WaterGraph, TEXT("Water"));
}

void AProceduralTerrainGenerator::RegenerateVegetation()
{
    GeneratePCGComponent(
        VegetationPCG,
        VegetationGraph,
        TEXT("Vegetation")
    );
}

void AProceduralTerrainGenerator::RegenerateInfrastructure()
{
    GeneratePCGComponent(
        InfrastructurePCG,
        InfrastructureGraph,
        TEXT("Infrastructure")
    );
}

void AProceduralTerrainGenerator::ClearPCG()
{
#if WITH_EDITOR
    UPCGComponent* Components[] = {
        TerrainPCG,
        WaterPCG,
        VegetationPCG,
        InfrastructurePCG
    };
    for (UPCGComponent* Component : Components)
    {
        if (Component)
        {
            Component->CleanupLocal(true);
        }
    }
#endif
}

void AProceduralTerrainGenerator::ClearGeneratedTerrain()
{
    ClearPCG();
    Watercourses.Reset();
    MountainCentres.Reset();
    MountainRadii.Reset();
    MountainHeights.Reset();

    UE_LOG(
        LogTemp,
        Display,
        TEXT(
            "Cleared Avenor PCG output and rule data. The native Landscape "
            "and Water actors were not deleted."
        )
    );
}

void AProceduralTerrainGenerator::ClearVegetation()
{
#if WITH_EDITOR
    if (VegetationPCG)
    {
        VegetationPCG->CleanupLocal(true);
    }
#endif
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
    const FVector2D Warp(
        FMath::PerlinNoise2D((P + SeedOffset * 0.37f) / 850000.0f),
        FMath::PerlinNoise2D((P + SeedOffset * 0.61f) / 850000.0f)
    );
    const FVector2D WarpedP = P + Warp * 140000.0f;

    const float Broad = FMath::PerlinNoise2D(
        (WarpedP + SeedOffset) / 600000.0f
    );
    const float Medium = FMath::PerlinNoise2D(
        (WarpedP + SeedOffset * 1.73f) / 180000.0f
    );
    const float Detail = FMath::PerlinNoise2D(
        (WarpedP + SeedOffset * 2.41f) / 65000.0f
    );
    const float RollingNoise = FMath::PerlinNoise2D(
        (WarpedP + SeedOffset * 3.19f) / 280000.0f
    );
    const float Ridges =
        (1.0f - FMath::Abs(RollingNoise)) * 2.0f - 1.0f;

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
        Broad * Relief * 0.52f +
        Medium * Relief * 0.28f +
        Detail * Relief * 0.10f +
        Ridges * Relief * 0.10f;

    for (int32 Index = 0; Index < MountainCentres.Num(); ++Index)
    {
        const FVector2D Delta = P - MountainCentres[Index];
        const float NormalisedDistance =
            Delta.Size() / MountainRadii[Index];
        if (NormalisedDistance >= 1.0f)
        {
            continue;
        }

        // Smooth quintic envelope: zero slope at the mountain boundary and
        // no flat/clamped summit.
        const float T = 1.0f - NormalisedDistance;
        const float Envelope =
            T * T * T * (T * (T * 6.0f - 15.0f) + 10.0f);
        const float LocalVariation = 0.78f + 0.22f * (
            0.5f + 0.5f * FMath::PerlinNoise2D(
                (P + SeedOffset * (Index + 3.0f)) / 250000.0f
            )
        );
        const float DistantFactor = FMath::SmoothStep(
            MountainMinimumDistance * 0.75f,
            MountainMinimumDistance,
            Distance
        );
        Height += MountainHeights[Index] * Envelope * LocalVariation *
            DistantFactor;
    }

    const float SpineReliefWeight = FMath::SmoothStep(
        SpineLevelHalfWidth,
        FMath::Max(SpineLevelHalfWidth + 1.0f, SpineLevelTransitionEnd),
        Distance
    );
    return Height * SpineReliefWeight;
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
                const float Weight =
                    FMath::Square(1.0f - Distance / ValleyWidth);
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
        if (FVector2D::Distance(Point, Watercourse.LakeCentre) <=
            Watercourse.LakeRadius)
        {
            HighestSurface = FMath::Max(
                HighestSurface,
                Watercourse.LakeSurfaceHeight
            );
            bFoundWater = true;
        }

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
            if (Distance > Watercourse.Width * 0.5f)
            {
                continue;
            }

            const float Surface = FMath::Lerp(
                Watercourse.SurfaceHeights[Index],
                Watercourse.SurfaceHeights[Index + 1],
                Alpha
            );
            HighestSurface = FMath::Max(HighestSurface, Surface);
            bFoundWater = true;
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

void AProceduralTerrainGenerator::GenerateWatercourses()
{
    FRandomStream Random(WorldSeed ^ 0x6A09E667);
    for (int32 RiverIndex = 0; RiverIndex < RiverCount; ++RiverIndex)
    {
        FGeneratedWatercourse Watercourse;
        Watercourse.Width =
            RiverBaseWidth * Random.FRandRange(0.75f, 1.55f);

        const float SourceSide = RiverIndex % 2 == 0 ? 1.0f : -1.0f;
        FVector2D Current = FVector2D::ZeroVector;
        float CurrentHeight = -TNumericLimits<float>::Max();

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

        Watercourse.LakeCentre = Current;
        Watercourse.LakeRadius =
            Random.FRandRange(50000.0f, 120000.0f);
        Watercourse.LakeSurfaceHeight =
            CurrentHeight - RiverCarveDepth * 0.25f;

        const float Step = FMath::Max(
            20000.0f,
            HalfWidth / FMath::Max(
                8.0f,
                RiverControlPointCount * 0.72f
            )
        );
        const FVector2D DrainageTarget(
            Random.FRandRange(-Length * 0.4f, Length * 0.4f),
            -SourceSide * HalfWidth * 0.75f
        );
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
            Watercourse.SurfaceHeights.Add(
                Watercourse.SurfaceHeights.IsEmpty()
                ? DesiredSurface
                : FMath::Min(
                    DesiredSurface,
                    Watercourse.SurfaceHeights.Last() - 5.0f
                )
            );

            FVector2D BestPoint = Current;
            FVector2D BestDirection = PreviousDirection;
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

                const float TurnPenalty =
                    (1.0f - FVector2D::DotProduct(
                        PreviousDirection,
                        Direction
                    )) * 250.0f;
                const float DrainageBias = FVector2D::Distance(
                    Candidate,
                    DrainageTarget
                ) * 0.0025f;
                const float Score = EvaluateBaseHeight(
                    Candidate.X,
                    Candidate.Y
                ) + TurnPenalty + DrainageBias;
                if (Score < BestScore)
                {
                    BestScore = Score;
                    BestPoint = Candidate;
                    BestDirection = Direction;
                }
            }

            if (BestPoint.Equals(Current, 1.0f))
            {
                break;
            }
            Current = BestPoint;
            PreviousDirection = BestDirection;
        }

        if (Watercourse.SpineSpacePoints.Num() >= 2)
        {
            Watercourses.Add(MoveTemp(Watercourse));
        }
    }
}
