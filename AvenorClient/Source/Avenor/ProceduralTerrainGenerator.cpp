#include "ProceduralTerrainGenerator.h"

#include "SpineGenerator.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "Materials/MaterialInterface.h"
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterSplineComponent.h"
#include "WaterZoneActor.h"

AProceduralTerrainGenerator::AProceduralTerrainGenerator()
{
    PrimaryActorTick.bCanEverTick = false;

    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);

    PCGBounds = CreateDefaultSubobject<UBoxComponent>(TEXT("PCGBounds"));
    PCGBounds->SetupAttachment(SceneRoot);
    PCGBounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    PCGBounds->SetHiddenInGame(true);

    VegetationPCG = CreateDefaultSubobject<UPCGComponent>(
        TEXT("VegetationPCG")
    );
    VegetationPCG->bIsComponentPartitioned = true;
    VegetationPCG->bRegenerateInEditor = true;
}

void AProceduralTerrainGenerator::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
#if WITH_EDITOR
    if (bRegenerateOnConstruction && !IsRunningGame())
    {
        Regenerate();
    }
#endif
}

void AProceduralTerrainGenerator::ClearGeneratedTerrain()
{
#if WITH_EDITOR
    if (IsValid(GeneratedLandscape))
    {
        GeneratedLandscape->Modify();
        GeneratedLandscape->Destroy();
        GeneratedLandscape = nullptr;
    }
    for (AWaterBody* WaterBody : GeneratedWaterBodies)
    {
        if (IsValid(WaterBody))
        {
            WaterBody->Modify();
            WaterBody->Destroy();
        }
    }
    GeneratedWaterBodies.Reset();
    if (IsValid(GeneratedWaterZone))
    {
        GeneratedWaterZone->Modify();
        GeneratedWaterZone->Destroy();
        GeneratedWaterZone = nullptr;
    }
#endif

    Watercourses.Reset();
    MountainCentres.Reset();
    MountainRadii.Reset();
    MountainHeights.Reset();
}

void AProceduralTerrainGenerator::Regenerate()
{
#if WITH_EDITOR
    if (IsRunningGame())
    {
        UE_LOG(
            LogTemp,
            Warning,
            TEXT("Avenor Landscape can only be regenerated in the editor.")
        );
        return;
    }

    Modify();
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
    BuildLandscape();
    if (bGenerateNativeWater)
    {
        BuildNativeWater();
    }
    if (bRegenerateVegetationWithLandscape && VegetationGraph)
    {
        RegenerateVegetation();
    }
#else
    UE_LOG(
        LogTemp,
        Warning,
        TEXT("Avenor Landscape generation is editor-only.")
    );
#endif
}

void AProceduralTerrainGenerator::RegenerateVegetation()
{
#if WITH_EDITOR
    if (!VegetationPCG || !VegetationGraph)
    {
        UE_LOG(
            LogTemp,
            Warning,
            TEXT(
                "Assign a PCG Graph to Vegetation Graph before generating "
                "Avenor vegetation."
            )
        );
        return;
    }

    VegetationPCG->Seed = WorldSeed;
    VegetationPCG->SetGraphLocal(VegetationGraph);
    VegetationPCG->CleanupLocal(true);
    VegetationPCG->GenerateLocal(true);
#endif
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
    const float RollingRidgeNoise = FMath::PerlinNoise2D(
        (WarpedP + SeedOffset * 3.19f) / 280000.0f
    );
    const float Ridges =
        (1.0f - FMath::Abs(RollingRidgeNoise)) * 2.0f - 1.0f;

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
        const float Radius = MountainRadii[Index];
        const float NormalisedDistance = Delta.Size() / Radius;
        if (NormalisedDistance < 1.0f)
        {
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

    // The Spine is the world datum. This puts roads/monorail at the expected
    // authored elevation while retaining a broad, gentle development belt.
    const float SpineReliefWeight = FMath::SmoothStep(
        SpineLevelHalfWidth,
        FMath::Max(
            SpineLevelHalfWidth + 1.0f,
            SpineLevelTransitionEnd
        ),
        Distance
    );
    Height *= SpineReliefWeight;
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
        const float LakeDistance = FVector2D::Distance(
            Point,
            Watercourse.LakeCentre
        );
        if (LakeDistance <= Watercourse.LakeRadius)
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

void AProceduralTerrainGenerator::WorldToSpineSpace(
    const FVector& WorldLocation,
    float& OutChainage,
    float& OutLateral
) const
{
    if (Spine)
    {
        float Vertical = 0.0f;
        Spine->GetSpineSpaceForWorldLocation(
            WorldLocation,
            OutChainage,
            OutLateral,
            Vertical
        );
        return;
    }

    const FVector Local = GetActorTransform().InverseTransformPosition(
        WorldLocation
    );
    OutChainage = Local.X;
    OutLateral = Local.Y;
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

        // Guarantee a source lake feeding each generated river. Its outlet is
        // the first river control point, so every lake has flowing drainage.
        Watercourse.LakeCentre = Current;
        Watercourse.LakeRadius = Random.FRandRange(50000.0f, 120000.0f);
        Watercourse.LakeSurfaceHeight =
            CurrentHeight - RiverCarveDepth * 0.25f;

        // Scale the routing step to the world so the configured point count
        // is sufficient to cross from the distant hills through the Spine.
        const float MinimumCrossingStep =
            HalfWidth /
            FMath::Max(8.0f, RiverControlPointCount * 0.72f);
        const float Step = FMath::Max3(
            LandscapeVertexSpacing * 2.0f,
            20000.0f,
            MinimumCrossingStep
        );
        FVector2D PreviousDirection(0.0f, -SourceSide);
        TSet<FIntPoint> Visited;
        const FVector2D DrainageTarget(
            Random.FRandRange(-Length * 0.4f, Length * 0.4f),
            -SourceSide * HalfWidth * 0.75f
        );

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
                // Terrain height remains dominant, but a broad drainage bias
                // prevents rivers wandering forever on their source side.
                const float DrainageBias = FVector2D::Distance(
                    Candidate,
                    DrainageTarget
                ) * 0.0025f;
                const float Score = Height + TurnPenalty + DrainageBias;
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

            FVector2D SpillPoint = Current;
            float SpillHeight = TNumericLimits<float>::Max();
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
                }
                if (RingBestHeight < CurrentHeight)
                {
                    break;
                }
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

void AProceduralTerrainGenerator::BuildLandscape()
{
#if WITH_EDITOR
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    const int32 QuadsPerSection = FMath::Clamp(
        LandscapeQuadsPerSection,
        7,
        255
    );
    constexpr int32 SectionsPerComponent = 1;
    const int32 XComponents = FMath::Max(
        1,
        FMath::CeilToInt(
            Length / (LandscapeVertexSpacing * QuadsPerSection)
        )
    );
    const int32 YComponents = FMath::Max(
        1,
        FMath::CeilToInt(
            (HalfWidth * 2.0f) /
            (LandscapeVertexSpacing * QuadsPerSection)
        )
    );
    const int32 XQuads = XComponents * QuadsPerSection;
    const int32 YQuads = YComponents * QuadsPerSection;
    const int32 XSize = XQuads + 1;
    const int32 YSize = YQuads + 1;
    const float ActualLength = XQuads * LandscapeVertexSpacing;
    const float ActualWidth = YQuads * LandscapeVertexSpacing;

    TArray<float> SampledHeights;
    SampledHeights.SetNumUninitialized(XSize * YSize);
    float MaximumAbsoluteHeight = 0.0f;

    const FVector Centre = GetActorLocation();
    const FVector LandscapeOrigin(
        Centre.X - ActualLength * 0.5f,
        Centre.Y - ActualWidth * 0.5f,
        Centre.Z
    );

    for (int32 Y = 0; Y < YSize; ++Y)
    {
        for (int32 X = 0; X < XSize; ++X)
        {
            const FVector WorldPoint(
                LandscapeOrigin.X + X * LandscapeVertexSpacing,
                LandscapeOrigin.Y + Y * LandscapeVertexSpacing,
                Centre.Z
            );
            float Chainage = 0.0f;
            float Lateral = 0.0f;
            WorldToSpineSpace(WorldPoint, Chainage, Lateral);
            const float Height = EvaluateTerrainHeight(
                Chainage,
                Lateral
            );
            SampledHeights[Y * XSize + X] = Height;
            MaximumAbsoluteHeight = FMath::Max(
                MaximumAbsoluteHeight,
                FMath::Abs(Height)
            );
        }
    }

    // Choose enough vertical range for the actual generated terrain. This
    // prevents the 16-bit height data saturating into perfectly flat summits.
    const float RequiredZScale =
        MaximumAbsoluteHeight * 128.0f / 32760.0f * 1.02f;
    const float EffectiveZScale = FMath::Max(
        LandscapeZScale,
        RequiredZScale
    );
    TArray<uint16> HeightData;
    HeightData.SetNumUninitialized(SampledHeights.Num());
    for (int32 Index = 0; Index < SampledHeights.Num(); ++Index)
    {
        const int32 EncodedHeight = FMath::RoundToInt(
            32768.0f + SampledHeights[Index] * 128.0f / EffectiveZScale
        );
        HeightData[Index] = static_cast<uint16>(
                FMath::Clamp(EncodedHeight, 0, 65535)
        );
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.Name = MakeUniqueObjectName(
        World->PersistentLevel,
        ALandscape::StaticClass(),
        TEXT("AvenorLandscape")
    );
    SpawnParameters.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    ALandscape* Landscape = World->SpawnActor<ALandscape>(
        ALandscape::StaticClass(),
        LandscapeOrigin,
        FRotator::ZeroRotator,
        SpawnParameters
    );
    if (!Landscape)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to create Avenor Landscape."));
        return;
    }

    Landscape->Modify();
    Landscape->SetActorScale3D(FVector(
        LandscapeVertexSpacing,
        LandscapeVertexSpacing,
        EffectiveZScale
    ));
    Landscape->LandscapeMaterial = TerrainMaterial;

    TMap<FGuid, TArray<uint16>> HeightDataPerLayers;
    HeightDataPerLayers.Add(FGuid(), MoveTemp(HeightData));
    TArray<FLandscapeImportLayerInfo> MaterialImportLayers;
    TMap<FGuid, TArray<FLandscapeImportLayerInfo>>
        MaterialLayerDataPerLayers;
    MaterialLayerDataPerLayers.Add(
        FGuid(),
        MoveTemp(MaterialImportLayers)
    );
    const TArray<FLandscapeLayer> ImportLayers;

    Landscape->Import(
        FGuid::NewGuid(),
        0,
        0,
        XQuads,
        YQuads,
        SectionsPerComponent,
        QuadsPerSection,
        HeightDataPerLayers,
        nullptr,
        MaterialLayerDataPerLayers,
        ELandscapeImportAlphamapType::Additive,
        TArrayView<const FLandscapeLayer>(ImportLayers)
    );

    Landscape->StaticLightingLOD = FMath::DivideAndRoundUp(
        FMath::CeilLogTwo(
            (XSize * YSize) / (2048 * 2048) + 1
        ),
        static_cast<uint32>(2)
    );
    Landscape->RegisterAllComponents();
    if (ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo())
    {
        LandscapeInfo->UpdateLayerInfoMap(Landscape);
    }
    Landscape->PostEditChange();
    Landscape->SetActorLabel(TEXT("Avenor Generated Landscape"));
    GeneratedLandscape = Landscape;
    PCGBounds->SetBoxExtent(FVector(
        ActualLength * 0.5f,
        ActualWidth * 0.5f,
        MaximumAbsoluteHeight + 100000.0f
    ));

    UE_LOG(
        LogTemp,
        Display,
        TEXT(
            "Created native Avenor Landscape: %d x %d vertices, "
            "%d x %d components."
        ),
        XSize,
        YSize,
        XComponents,
        YComponents
    );
#endif
}

void AProceduralTerrainGenerator::BuildNativeWater()
{
#if WITH_EDITOR
    UWorld* World = GetWorld();
    if (!World || Watercourses.IsEmpty())
    {
        return;
    }

    const FVector Centre = GetActorLocation();
    FActorSpawnParameters SpawnParameters;
    SpawnParameters.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    GeneratedWaterZone = World->SpawnActor<AWaterZone>(
        AWaterZone::StaticClass(),
        Centre,
        FRotator::ZeroRotator,
        SpawnParameters
    );
    if (GeneratedWaterZone)
    {
        GeneratedWaterZone->SetActorLabel(
            TEXT("Avenor Generated Water Zone")
        );
        if (UBoxComponent* Bounds =
            GeneratedWaterZone->FindComponentByClass<UBoxComponent>())
        {
            Bounds->SetBoxExtent(FVector(
                Length * 0.55f,
                HalfWidth * 1.1f,
                200000.0f
            ));
        }
    }

    for (int32 WaterIndex = 0;
         WaterIndex < Watercourses.Num();
         ++WaterIndex)
    {
        const FGeneratedWatercourse& Watercourse =
            Watercourses[WaterIndex];

        AWaterBodyRiver* River = World->SpawnActorDeferred<AWaterBodyRiver>(
            AWaterBodyRiver::StaticClass(),
            FTransform::Identity,
            nullptr,
            nullptr,
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn
        );
        if (River)
        {
            // Water bodies default to landscape deformation. Disable it before
            // FinishSpawning so they never register a Water edit-layer brush;
            // Avenor's height generator has already carved these channels.
            UWaterBodyComponent* Component =
                River->GetWaterBodyComponent();
            if (Component)
            {
                Component->bAffectsLandscape = false;
            }
            River->SetActorLabel(FString::Printf(
                TEXT("Avenor River %d"),
                WaterIndex + 1
            ));
            UWaterSplineComponent* Spline = River->GetWaterSpline();
            Spline->ClearSplinePoints(false);
            for (int32 PointIndex = 0;
                 PointIndex < Watercourse.SpineSpacePoints.Num();
                 ++PointIndex)
            {
                const FVector2D Point =
                    Watercourse.SpineSpacePoints[PointIndex];
                const float Height =
                    Watercourse.SurfaceHeights.IsValidIndex(PointIndex)
                    ? Watercourse.SurfaceHeights[PointIndex]
                    : EvaluateBaseHeight(Point.X, Point.Y);
                Spline->AddSplinePoint(
                    SpineSpaceToWorld(Point.X, Point.Y, Height),
                    ESplineCoordinateSpace::World,
                    false
                );
                Spline->SetScaleAtSplinePoint(
                    PointIndex,
                    FVector(
                        1.0f,
                        FMath::Max(1.0f, Watercourse.Width / 1000.0f),
                        1.0f
                    ),
                    false
                );
                Spline->SetSplinePointType(
                    PointIndex,
                    ESplinePointType::Curve,
                    false
                );
            }
            Spline->SetClosedLoop(false, true);
            River->FinishSpawning(FTransform::Identity);
            if (Component)
            {
                if (WaterMaterial)
                {
                    Component->SetWaterMaterial(WaterMaterial);
                }
                if (GeneratedWaterZone)
                {
                    Component->SetWaterZoneOverride(
                        TSoftObjectPtr<AWaterZone>(GeneratedWaterZone)
                    );
                }
                Component->OnWaterBodyChanged(true, false, true);
            }
            GeneratedWaterBodies.Add(River);
        }

        if (Watercourse.LakeRadius <= 0.0f)
        {
            continue;
        }

        AWaterBodyLake* Lake = World->SpawnActorDeferred<AWaterBodyLake>(
            AWaterBodyLake::StaticClass(),
            FTransform::Identity,
            nullptr,
            nullptr,
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn
        );
        if (!Lake)
        {
            continue;
        }
        UWaterBodyComponent* LakeComponent =
            Lake->GetWaterBodyComponent();
        if (LakeComponent)
        {
            LakeComponent->bAffectsLandscape = false;
        }
        Lake->SetActorLabel(FString::Printf(
            TEXT("Avenor Lake %d"),
            WaterIndex + 1
        ));
        UWaterSplineComponent* LakeSpline = Lake->GetWaterSpline();
        LakeSpline->ClearSplinePoints(false);
        constexpr int32 LakeSegments = 24;
        for (int32 Segment = 0; Segment < LakeSegments; ++Segment)
        {
            const float Angle =
                2.0f * PI * static_cast<float>(Segment) / LakeSegments;
            const FVector2D Point =
                Watercourse.LakeCentre +
                FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) *
                Watercourse.LakeRadius;
            LakeSpline->AddSplinePoint(
                SpineSpaceToWorld(
                    Point.X,
                    Point.Y,
                    Watercourse.LakeSurfaceHeight
                ),
                ESplineCoordinateSpace::World,
                false
            );
            LakeSpline->SetSplinePointType(
                Segment,
                ESplinePointType::Curve,
                false
            );
        }
        LakeSpline->SetClosedLoop(true, true);
        Lake->FinishSpawning(FTransform::Identity);
        if (LakeComponent)
        {
            if (WaterMaterial)
            {
                LakeComponent->SetWaterMaterial(WaterMaterial);
            }
            if (GeneratedWaterZone)
            {
                LakeComponent->SetWaterZoneOverride(
                    TSoftObjectPtr<AWaterZone>(GeneratedWaterZone)
                );
            }
            LakeComponent->OnWaterBodyChanged(true, false, true);
        }
        GeneratedWaterBodies.Add(Lake);
    }
#endif
}
