#include "SpineGenerator.h"

#include "AvenorTerrainData.h"

#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Engine/World.h"
#include "Math/RotationMatrix.h"
#include "PCGComponent.h"
#include "PCGGraph.h"

TFunction<bool(ASpineGenerator*)>
    AvenorSpineEditorBridge::BindTerrainModifier;
TFunction<void(ASpineGenerator*)>
    AvenorSpineEditorBridge::ClearTerrainModifier;


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
    InfrastructurePCG->GenerationTrigger =
        EPCGComponentGenerationTrigger::GenerateOnDemand;
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
        LoadTerrainAlignmentLayer();
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
    StreetLampPlacements.Reset();

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
    RebuildStreetLampPlacements();

    UE_LOG(
        LogTemp,
        Display,
        TEXT("Avenor Spine: rebuilt %d stations, %d blocks, %d greybox segments, %d guideways, %d piers, %d supports and %d street lamps."),
        StationRecords.Num(),
        BlockRecords.Num(),
        GreyboxSegments.Num(),
        GuidewayPlacements.Num(),
        MonorailPierPlacements.Num(),
        MonorailSupportPlacements.Num(),
        StreetLampPlacements.Num()
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
    const UAvenorTerrainData* Data = TerrainData.LoadSynchronous();
    if (!Data || !Data->HasValidData())
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("Avenor Spine: assign the same valid Terrain Data asset used by the terrain generator.")
        );
        return false;
    }

    const float Start = GetMinimumChainage();
    const float End = GetMaximumChainage();
    // Ten-metre samples keep local roads close to rolling ground between the
    // coarser user-configured alignment stations.
    const float Step = FMath::Min(
        1000.0f,
        FMath::Max(100.0f, AlignmentSampleLength)
    );
    const int32 SampleCount = FMath::Max(
        2,
        FMath::CeilToInt((End - Start) / Step) + 1
    );
    AlignmentSamples.Reserve(SampleCount);

    const float DevelopmentHalfWidth = GetDevelopmentOuterLateral();
    const float DevelopmentProfileStart =
        GetDevelopmentProfileStartLateral();
    const float DevelopmentProfileWidth = FMath::Max(
        1.0f,
        DevelopmentHalfWidth - DevelopmentProfileStart
    );
    const int32 DevelopmentProfileIntervalCount = FMath::Max(
        1,
        FMath::CeilToInt(
            DevelopmentProfileWidth
            / FMath::Min(
                1000.0f,
                FMath::Max(500.0f, DevelopmentProfileSampleSpacing)
            )
        )
    );
    const int32 DevelopmentProfileSampleCount =
        DevelopmentProfileIntervalCount + 1;
    const float DevelopmentProfileStep = DevelopmentProfileWidth
        / static_cast<float>(DevelopmentProfileIntervalCount);

    FAvenorTerrainHeightChunkCache HeightCache;
    TArray<bool> ActualWaterSamples;
    TArray<float> WaterSurfaceHeights;
    ActualWaterSamples.Reserve(SampleCount);
    WaterSurfaceHeights.Reserve(SampleCount);
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

        FSpineAlignmentSample& Sample =
            AlignmentSamples.AddDefaulted_GetRef();
        Sample.Chainage = Chainage;
        Sample.bTerrainHit = Data->SampleBaseHeight(
            FVector2D(BaseLocation), Sample.NaturalTerrainZ, HeightCache
        );
        if (!Sample.bTerrainHit)
        {
            UE_LOG(LogTemp, Error,
                TEXT("Avenor Spine: guide sample at %.0f cm is outside or unreadable in Terrain Data."),
                Chainage);
            AlignmentSamples.Reset();
            return false;
        }
        float FinalCentreZ = Sample.NaturalTerrainZ;
        float WaterSurfaceZ = 0.0f;
        const bool bActualWater = Data->SampleWaterSurface(
            FVector2D(BaseLocation), WaterSurfaceZ
        );
        ActualWaterSamples.Add(bActualWater);
        WaterSurfaceHeights.Add(WaterSurfaceZ);
        Sample.bDevelopmentSuitable = Data->SampleFinalHeight(
            FVector2D(BaseLocation), FinalCentreZ, HeightCache
        )
            && Sample.NaturalTerrainZ - FinalCentreZ
                <= MaximumDevelopmentCarveDepth;

        const FVector BaseRight = FVector::CrossProduct(
            FVector::UpVector,
            BaseForward
        ).GetSafeNormal();
        Sample.LeftDevelopmentProfileZ.SetNumUninitialized(
            DevelopmentProfileSampleCount
        );
        Sample.RightDevelopmentProfileZ.SetNumUninitialized(
            DevelopmentProfileSampleCount
        );
        for (int32 ProfileIndex = 0;
             ProfileIndex < DevelopmentProfileSampleCount;
             ++ProfileIndex)
        {
            const float Lateral = DevelopmentProfileStart
                + DevelopmentProfileStep * ProfileIndex;
            if (!Data->SampleBaseHeight(
                    FVector2D(BaseLocation - BaseRight * Lateral),
                    Sample.LeftDevelopmentProfileZ[ProfileIndex], HeightCache))
            {
                UE_LOG(LogTemp, Error,
                    TEXT("Avenor Spine: left development sample at %.0f cm is outside or unreadable in Terrain Data."),
                    Chainage);
                AlignmentSamples.Reset();
                return false;
            }
            if (!Data->SampleBaseHeight(
                    FVector2D(BaseLocation + BaseRight * Lateral),
                    Sample.RightDevelopmentProfileZ[ProfileIndex], HeightCache))
            {
                UE_LOG(LogTemp, Error,
                    TEXT("Avenor Spine: right development sample at %.0f cm is outside or unreadable in Terrain Data."),
                    Chainage);
                AlignmentSamples.Reset();
                return false;
            }
            float FinalLeftZ = Sample.LeftDevelopmentProfileZ[ProfileIndex];
            float FinalRightZ = Sample.RightDevelopmentProfileZ[ProfileIndex];
            float LateralWaterSurfaceZ = 0.0f;
            const bool bLeftActualWater = Data->SampleWaterSurface(
                FVector2D(BaseLocation - BaseRight * Lateral),
                LateralWaterSurfaceZ
            );
            const bool bRightActualWater = Data->SampleWaterSurface(
                FVector2D(BaseLocation + BaseRight * Lateral),
                LateralWaterSurfaceZ
            );
            Sample.bDevelopmentSuitable &= Data->SampleFinalHeight(
                FVector2D(BaseLocation - BaseRight * Lateral),
                FinalLeftZ, HeightCache
            ) && !bLeftActualWater
                && Sample.LeftDevelopmentProfileZ[ProfileIndex] - FinalLeftZ
                    <= MaximumDevelopmentCarveDepth;
            Sample.bDevelopmentSuitable &= Data->SampleFinalHeight(
                FVector2D(BaseLocation + BaseRight * Lateral),
                FinalRightZ, HeightCache
            ) && !bRightActualWater
                && Sample.RightDevelopmentProfileZ[ProfileIndex] - FinalRightZ
                    <= MaximumDevelopmentCarveDepth;
            // Retained roadside geometry follows the same post-water surface
            // that decided suitability. Unsuitable bays are omitted later.
            Sample.LeftDevelopmentProfileZ[ProfileIndex] = FinalLeftZ;
            Sample.RightDevelopmentProfileZ[ProfileIndex] = FinalRightZ;
        }
        Sample.RoadDatumZ = Sample.NaturalTerrainZ + RoadDatumOffset;
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
        TArray<float> WindowHeights;
        const int32 First = FMath::Max(0, Index - SmoothingRadius);
        const int32 Last = FMath::Min(
            AlignmentSamples.Num() - 1,
            Index + SmoothingRadius
        );
        WindowHeights.Reserve(Last - First + 1);
        for (int32 Neighbor = First; Neighbor <= Last; ++Neighbor)
        {
            const double Weight = static_cast<double>(
                SmoothingRadius + 1 - FMath::Abs(Neighbor - Index)
            );
            Sum += AlignmentSamples[Neighbor].RoadDatumZ * Weight;
            TotalWeight += Weight;
            WindowHeights.Add(AlignmentSamples[Neighbor].RoadDatumZ);
        }
        WindowHeights.Sort();
        const int32 LowerQuartileIndex = FMath::FloorToInt(
            static_cast<float>(WindowHeights.Num() - 1) * 0.25f
        );
        const float BalancedHeight = static_cast<float>(
            Sum / FMath::Max(1.0, TotalWeight)
        );
        SmoothedHeights[Index] = FMath::Lerp(
            BalancedHeight,
            WindowHeights[LowerQuartileIndex],
            FMath::Clamp(AlignmentCutBias, 0.0f, 1.0f)
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
    for (int32 Index = 0; Index < AlignmentSamples.Num(); ++Index)
    {
        AlignmentSamples[Index].RoadDatumZ = SmoothedHeights[Index];
    }

    // Actual water, rather than the much wider shoreline carve/blend, defines
    // bridge spans. Extend each crossing onto dry ground by the editable
    // setback, hold the transport datum level, and omit development there.
    const int32 SetbackSamples = FMath::Max(
        0,
        FMath::CeilToInt(BridgeApproachSetback / FMath::Max(1.0f, Step))
    );
    for (int32 WaterStart = 0; WaterStart < ActualWaterSamples.Num();)
    {
        if (!ActualWaterSamples[WaterStart])
        {
            ++WaterStart;
            continue;
        }
        int32 WaterEnd = WaterStart;
        while (WaterEnd + 1 < ActualWaterSamples.Num()
            && ActualWaterSamples[WaterEnd + 1])
        {
            ++WaterEnd;
        }

        const int32 SpanStart = FMath::Max(0, WaterStart - SetbackSamples);
        const int32 SpanEnd = FMath::Min(
            AlignmentSamples.Num() - 1,
            WaterEnd + SetbackSamples
        );
        float DeckZ = FMath::Max(
            AlignmentSamples[SpanStart].RoadDatumZ,
            AlignmentSamples[SpanEnd].RoadDatumZ
        );
        for (int32 Index = WaterStart; Index <= WaterEnd; ++Index)
        {
            DeckZ = FMath::Max(
                DeckZ,
                WaterSurfaceHeights[Index] + BridgeMinimumClearance
            );
        }
        for (int32 Index = SpanStart; Index <= SpanEnd; ++Index)
        {
            AlignmentSamples[Index].RoadDatumZ = DeckZ;
            AlignmentSamples[Index].bDevelopmentSuitable = false;
        }
        WaterStart = WaterEnd + 1;
    }

    // Smooth every lateral band along the route and across its immediate
    // neighbours. This retains broad natural side-hill form without copying
    // individual terrain bumps into roads and buildable parcel ground.
    TArray<TArray<float>> SmoothedLeftProfiles;
    TArray<TArray<float>> SmoothedRightProfiles;
    SmoothedLeftProfiles.SetNum(AlignmentSamples.Num());
    SmoothedRightProfiles.SetNum(AlignmentSamples.Num());
    const int32 ProfileSmoothingRadius = FMath::Max(
        0,
        FMath::RoundToInt(
            DevelopmentProfileSmoothingDistance / FMath::Max(1.0f, Step)
        )
    );
    for (int32 Index = 0; Index < AlignmentSamples.Num(); ++Index)
    {
        SmoothedLeftProfiles[Index].SetNumUninitialized(
            DevelopmentProfileSampleCount
        );
        SmoothedRightProfiles[Index].SetNumUninitialized(
            DevelopmentProfileSampleCount
        );
        for (int32 ProfileIndex = 0;
             ProfileIndex < DevelopmentProfileSampleCount;
             ++ProfileIndex)
        {
            double LeftSum = 0.0;
            double RightSum = 0.0;
            double TotalWeight = 0.0;
            const int32 FirstChainage = FMath::Max(
                0,
                Index - ProfileSmoothingRadius
            );
            const int32 LastChainage = FMath::Min(
                AlignmentSamples.Num() - 1,
                Index + ProfileSmoothingRadius
            );
            for (int32 Neighbor = FirstChainage;
                 Neighbor <= LastChainage;
                 ++Neighbor)
            {
                const double ChainageWeight = static_cast<double>(
                    ProfileSmoothingRadius + 1
                    - FMath::Abs(Neighbor - Index)
                );
                const int32 FirstLateral = FMath::Max(
                    0,
                    ProfileIndex - 1
                );
                const int32 LastLateral = FMath::Min(
                    DevelopmentProfileSampleCount - 1,
                    ProfileIndex + 1
                );
                for (int32 LateralNeighbor = FirstLateral;
                     LateralNeighbor <= LastLateral;
                     ++LateralNeighbor)
                {
                    const double LateralWeight = LateralNeighbor
                            == ProfileIndex
                        ? 2.0
                        : 1.0;
                    const double Weight = ChainageWeight * LateralWeight;
                    LeftSum += AlignmentSamples[Neighbor]
                        .LeftDevelopmentProfileZ[LateralNeighbor] * Weight;
                    RightSum += AlignmentSamples[Neighbor]
                        .RightDevelopmentProfileZ[LateralNeighbor] * Weight;
                    TotalWeight += Weight;
                }
            }
            SmoothedLeftProfiles[Index][ProfileIndex] =
                static_cast<float>(LeftSum / FMath::Max(1.0, TotalWeight));
            SmoothedRightProfiles[Index][ProfileIndex] =
                static_cast<float>(RightSum / FMath::Max(1.0, TotalWeight));
        }
    }

    // Pin both road-facing edges to the shared Spine datum, then walk outward
    // independently on each side. Each band follows its smoothed natural
    // height as far as the configured gentle cross-grade allows.
    const float MaximumProfileStep = FMath::Max(
        0.0f,
        MaximumDevelopmentCrossGrade
    ) * DevelopmentProfileStep;
    for (int32 Index = 0; Index < AlignmentSamples.Num(); ++Index)
    {
        FSpineAlignmentSample& Sample = AlignmentSamples[Index];
        Sample.LeftDevelopmentProfileZ[0] = Sample.RoadDatumZ;
        Sample.RightDevelopmentProfileZ[0] = Sample.RoadDatumZ;
        for (int32 ProfileIndex = 1;
             ProfileIndex < DevelopmentProfileSampleCount;
             ++ProfileIndex)
        {
            Sample.LeftDevelopmentProfileZ[ProfileIndex] = FMath::Clamp(
                SmoothedLeftProfiles[Index][ProfileIndex],
                Sample.LeftDevelopmentProfileZ[ProfileIndex - 1]
                    - MaximumProfileStep,
                Sample.LeftDevelopmentProfileZ[ProfileIndex - 1]
                    + MaximumProfileStep
            );
            Sample.RightDevelopmentProfileZ[ProfileIndex] = FMath::Clamp(
                SmoothedRightProfiles[Index][ProfileIndex],
                Sample.RightDevelopmentProfileZ[ProfileIndex - 1]
                    - MaximumProfileStep,
                Sample.RightDevelopmentProfileZ[ProfileIndex - 1]
                    + MaximumProfileStep
            );
        }
    }

    LastMaximumCutDepth = 0.0f;
    LastMaximumFillHeight = 0.0f;
    LastStructureCandidateCount = 0;
    float MaximumLateralRise = 0.0f;
    float MaximumLateralFall = 0.0f;
    for (int32 Index = 0; Index < AlignmentSamples.Num(); ++Index)
    {
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
        for (const TArray<float>* Profile : {
                 &AlignmentSamples[Index].LeftDevelopmentProfileZ,
                 &AlignmentSamples[Index].RightDevelopmentProfileZ})
        {
            for (const float ProfileZ : *Profile)
            {
                MaximumLateralRise = FMath::Max(
                    MaximumLateralRise,
                    ProfileZ - AlignmentSamples[Index].RoadDatumZ
                );
                MaximumLateralFall = FMath::Max(
                    MaximumLateralFall,
                    AlignmentSamples[Index].RoadDatumZ - ProfileZ
                );
            }
        }
    }

    UE_LOG(
        LogTemp,
        Display,
        TEXT("Avenor Spine: solved %d terrain-data samples at %.0f cm spacing from %d streamed chunks, %.1f%% maximum road grade; lateral profile rises %.1f m and falls %.1f m; maximum cut %.1f m, fill %.1f m, %d structure candidates."),
        AlignmentSamples.Num(),
        Step,
        HeightCache.Chunks.Num(),
        MaximumRoadGrade * 100.0f,
        MaximumLateralRise / 100.0f,
        MaximumLateralFall / 100.0f,
        LastMaximumCutDepth / 100.0f,
        LastMaximumFillHeight / 100.0f,
        LastStructureCandidateCount
    );
    return true;
}

bool ASpineGenerator::StoreTerrainAlignmentLayer()
{
    UAvenorTerrainData* Data = TerrainData.LoadSynchronous();
    if (!Data || AlignmentSamples.Num() < 2)
    {
        return false;
    }
    Data->Modify();
    FAvenorBakedSpineLayer& Layer = Data->SpineLayer;
    Layer.GeneratedAtUtc = FDateTime::UtcNow();
    Layer.FlatHalfWidth = GetDevelopmentProfileStartLateral();
    Layer.DevelopmentHalfWidth = GetDevelopmentOuterLateral();
    Layer.TransitionHalfWidth = Layer.DevelopmentHalfWidth
        + FMath::Max(100.0f, CorridorTransitionHalfWidth);
    Layer.Samples.Reset(AlignmentSamples.Num());
    for (const FSpineAlignmentSample& Source : AlignmentSamples)
    {
        FVector Location;
        FVector Forward;
        GetBaseSplineFrameAtChainage(Source.Chainage, Location, Forward);
        FAvenorBakedSpineSample& Target = Layer.Samples.AddDefaulted_GetRef();
        Target.Centre = FVector2D(Location);
        Target.Chainage = Source.Chainage;
        Target.NaturalTerrainZ = Source.NaturalTerrainZ;
        Target.RoadDatumZ = Source.RoadDatumZ;
        Target.LeftDevelopmentProfileZ = Source.LeftDevelopmentProfileZ;
        Target.RightDevelopmentProfileZ = Source.RightDevelopmentProfileZ;
        Target.bDevelopmentSuitable = Source.bDevelopmentSuitable;
    }
    Data->MarkPackageDirty();
    return Layer.HasValidData();
}

void ASpineGenerator::LoadTerrainAlignmentLayer()
{
    const UAvenorTerrainData* Data = TerrainData.LoadSynchronous();
    if (!Data || !Data->SpineLayer.HasValidData())
    {
        return;
    }
    AlignmentSamples.Reset(Data->SpineLayer.Samples.Num());
    for (const FAvenorBakedSpineSample& Source : Data->SpineLayer.Samples)
    {
        FSpineAlignmentSample& Target = AlignmentSamples.AddDefaulted_GetRef();
        Target.Chainage = Source.Chainage;
        Target.NaturalTerrainZ = Source.NaturalTerrainZ;
        Target.RoadDatumZ = Source.RoadDatumZ;
        Target.EarthworkDelta = Source.RoadDatumZ - Source.NaturalTerrainZ;
        Target.LeftDevelopmentProfileZ = Source.LeftDevelopmentProfileZ;
        Target.RightDevelopmentProfileZ = Source.RightDevelopmentProfileZ;
        Target.bDevelopmentSuitable = Source.bDevelopmentSuitable;
        Target.bTerrainHit = true;
    }
}

bool ASpineGenerator::BindTerrainModifier()
{
#if WITH_EDITOR
    return AvenorSpineEditorBridge::BindTerrainModifier
        ? AvenorSpineEditorBridge::BindTerrainModifier(this)
        : false;
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
    if (!StoreTerrainAlignmentLayer())
    {
        AlignmentSamples.Reset();
        UE_LOG(LogTemp, Error,
            TEXT("Avenor Spine: could not store the solved Spine layer in Terrain Data."));
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
        TEXT("Avenor Spine: graded the %.0f cm road reservation across %.0f cm of development per side, with a %.0f cm outer blend."),
        CorridorFlatHalfWidth,
        GetDevelopmentOuterLateral(),
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
    if (AvenorSpineEditorBridge::ClearTerrainModifier)
    {
        AvenorSpineEditorBridge::ClearTerrainModifier(this);
    }
    AlignmentSamples.Reset();
    if (UAvenorTerrainData* Data = TerrainData.LoadSynchronous())
    {
        Data->Modify();
        Data->SpineLayer = FAvenorBakedSpineLayer();
        Data->MarkPackageDirty();
    }
    LastMaximumCutDepth = 0.0f;
    LastMaximumFillHeight = 0.0f;
    LastStructureCandidateCount = 0;

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
        InfrastructurePCG->GenerationTrigger =
            EPCGComponentGenerationTrigger::GenerateOnDemand;
    }

    bPartitionedGeneration = false;
    DistrictsBeforeStationZero = 0;
    DistrictsAfterStationZero = 1;
    DevelopmentRowsPerSide = 1;
    AlignmentSampleLength = 2500.0f;
    SpineReservationWidth = 5400.0f;
    HighwayCarriagewayWidth = 800.0f;
    SpinePavementWidth = 500.0f;
    LocalCarriagewayWidth = 700.0f;
    StationCarriagewayWidth = 1000.0f;
    RoadThickness = 20.0f;
    PavementKerbHeight = 15.0f;
    if (AvenorSpineEditorBridge::ClearTerrainModifier)
    {
        AvenorSpineEditorBridge::ClearTerrainModifier(this);
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

float ASpineGenerator::GetResolvedLocalPavementWidth() const
{
    return FMath::Max(
        0.0f,
        (GetResolvedLocalStreetWidth() - LocalCarriagewayWidth) * 0.5f
    );
}

float ASpineGenerator::GetResolvedStationPavementWidth() const
{
    return FMath::Max(
        0.0f,
        (StationStreetWidth - StationCarriagewayWidth) * 0.5f
    );
}

float ASpineGenerator::GetSpineCentralPublicRealmWidth() const
{
    return FMath::Max(
        0.0f,
        SpineReservationWidth
            - 2.0f * (HighwayCarriagewayWidth + SpinePavementWidth)
    );
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
    const UAvenorTerrainData* Data = TerrainData.LoadSynchronous();
    if (!Data || !Data->SpineLayer.HasValidData())
    {
        return FBox(ForceInit);
    }

    FBox Bounds(ForceInit);
    for (const FAvenorBakedSpineSample& Sample : Data->SpineLayer.Samples)
    {
        Bounds += FVector(Sample.Centre, Sample.RoadDatumZ);
    }
    const float HorizontalExtent = Data->SpineLayer.TransitionHalfWidth;
    const FVector Expansion(
        HorizontalExtent,
        HorizontalExtent,
        1000000.0f
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
    FVector Location = Frame.GetLocation()
        + Frame.GetUnitAxis(EAxis::Y) * LateralOffset;
    Location.Z = EvaluateDevelopmentSurfaceZ(Chainage, LateralOffset)
        + VerticalOffset;
    return Location;
}

float ASpineGenerator::EvaluateDevelopmentSurfaceZ(
    float Chainage,
    float Lateral
) const
{
    const float RoadZ = EvaluateRoadDatumZ(Chainage);
    const float ProfileStart = GetDevelopmentProfileStartLateral();
    const float DevelopmentOuter = GetDevelopmentOuterLateral();
    const float Distance = FMath::Abs(Lateral);
    if (AlignmentSamples.Num() == 0 || Distance <= ProfileStart)
    {
        return RoadZ;
    }

    auto EvaluateSampleProfile = [&](const FSpineAlignmentSample& Sample)
    {
        const TArray<float>& Profile = Lateral >= 0.0f
            ? Sample.RightDevelopmentProfileZ
            : Sample.LeftDevelopmentProfileZ;
        if (Profile.Num() < 2)
        {
            return Sample.RoadDatumZ;
        }
        const float CrossAlpha = FMath::Clamp(
            (Distance - ProfileStart)
                / FMath::Max(1.0f, DevelopmentOuter - ProfileStart),
            0.0f,
            1.0f
        );
        const float Position = CrossAlpha * (Profile.Num() - 1);
        const int32 ProfileIndex = FMath::Min(
            FMath::FloorToInt(Position),
            Profile.Num() - 2
        );
        return FMath::Lerp(
            Profile[ProfileIndex],
            Profile[ProfileIndex + 1],
            Position - ProfileIndex
        );
    };

    if (Chainage <= AlignmentSamples[0].Chainage)
    {
        return EvaluateSampleProfile(AlignmentSamples[0]);
    }
    if (Chainage >= AlignmentSamples.Last().Chainage)
    {
        return EvaluateSampleProfile(AlignmentSamples.Last());
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
            return FMath::Lerp(
                EvaluateSampleProfile(A),
                EvaluateSampleProfile(B),
                Alpha
            );
        }
    }
    return RoadZ;
}

bool ASpineGenerator::IsDevelopmentSuitableAtChainage(float Chainage) const
{
    if (AlignmentSamples.IsEmpty())
    {
        return true;
    }
    if (Chainage <= AlignmentSamples[0].Chainage)
    {
        return AlignmentSamples[0].bDevelopmentSuitable;
    }
    for (int32 Index = 1; Index < AlignmentSamples.Num(); ++Index)
    {
        if (Chainage <= AlignmentSamples[Index].Chainage)
        {
            // A gap is unsuitable until both bracketing final-terrain samples
            // are clear; this prevents clipped parcel or road fragments.
            return AlignmentSamples[Index - 1].bDevelopmentSuitable
                && AlignmentSamples[Index].bDevelopmentSuitable;
        }
    }
    return AlignmentSamples.Last().bDevelopmentSuitable;
}

FTransform ASpineGenerator::GetDevelopmentSurfaceTransformAtChainage(
    float Chainage,
    float Lateral
) const
{
    const float Probe = FMath::Max(100.0f, AlignmentSampleLength * 0.25f);
    const FVector Centre = GetSpineLocationAtChainage(Chainage, Lateral);
    const FVector Along = (
        GetSpineLocationAtChainage(Chainage + Probe, Lateral)
        - GetSpineLocationAtChainage(Chainage - Probe, Lateral)
    ).GetSafeNormal();
    const FVector Across = (
        GetSpineLocationAtChainage(Chainage, Lateral + Probe)
        - GetSpineLocationAtChainage(Chainage, Lateral - Probe)
    ).GetSafeNormal();
    FVector Up = FVector::CrossProduct(Along, Across).GetSafeNormal();
    if (Up.Z < 0.0f)
    {
        Up *= -1.0f;
    }
    const FVector SafeAlong = Along.IsNearlyZero()
        ? GetSpineTransformAtChainage(Chainage).GetUnitAxis(EAxis::X)
        : Along;
    if (Up.IsNearlyZero())
    {
        Up = FVector::UpVector;
    }
    return FTransform(
        FRotationMatrix::MakeFromXZ(SafeAlong, Up).ToQuat(),
        Centre
    );
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

    const float CarriagewayOffset = GetSpineCarriagewayCentreOffset();
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

float ASpineGenerator::GetSpineCarriagewayCentreOffset() const
{
    return GetSpineCentralPublicRealmWidth() * 0.5f
        + HighwayCarriagewayWidth * 0.5f;
}

float ASpineGenerator::GetSpineOuterKerbLateral() const
{
    return SpineReservationWidth * 0.5f - SpinePavementWidth;
}

float ASpineGenerator::GetDevelopmentOuterLateral() const
{
    const float LocalStreetWidth = GetResolvedLocalStreetWidth();
    return SpineReservationWidth * 0.5f
        + DevelopmentRowsPerSide * (BlockSize + LocalStreetWidth);
}

float ASpineGenerator::GetDevelopmentProfileStartLateral() const
{
    // The central public realm, carriageways and five-metre boulevard
    // pavements fill the complete Spine reservation. The first parcel begins
    // directly at its outer edge, with no frontage road or exposed gap.
    return FMath::Max(
        CorridorFlatHalfWidth,
        SpineReservationWidth * 0.5f
    );
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
            const bool bBaySuitable =
                IsDevelopmentSuitableAtChainage(Chainage - BlockSize * 0.5f)
                && IsDevelopmentSuitableAtChainage(Chainage)
                && IsDevelopmentSuitableAtChainage(Chainage + BlockSize * 0.5f);
            if (!bBaySuitable)
            {
                continue;
            }
            for (int32 RowIndex = 0;
                 RowIndex < DevelopmentRowsPerSide;
                 ++RowIndex)
            {
                const float RowCentre = SpineReservationWidth * 0.5f
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
                    Block.Transform =
                        GetDevelopmentSurfaceTransformAtChainage(
                            Chainage,
                            Block.Lateral
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
    const float CarriagewayOffset = GetSpineCarriagewayCentreOffset();
    const float SpineHalfWidth = SpineReservationWidth * 0.5f;
    const float PavementThickness = RoadThickness + PavementKerbHeight;

    for (float Chainage = Start; Chainage < End; Chainage += Step)
    {
        const float Next = FMath::Min(Chainage + Step, End);
        const int32 DistrictIndex = FMath::FloorToInt(
            Chainage / StationSpacing
        );

        AddGreyboxSpan(
            TEXT("SpinePublicRealm"),
            Chainage,
            Next,
            0.0f,
            0.0f,
            PavementThickness * 0.5f,
            GetSpineCentralPublicRealmWidth(),
            PavementThickness,
            DistrictIndex,
            0
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

            if (IsDevelopmentSuitableAtChainage((Chainage + Next) * 0.5f))
            {
                AddGreyboxSpan(
                    TEXT("SpinePavement"),
                    Chainage,
                    Next,
                    Side * (SpineHalfWidth - SpinePavementWidth * 0.5f),
                    Side * (SpineHalfWidth - SpinePavementWidth * 0.5f),
                    PavementThickness * 0.5f,
                    SpinePavementWidth,
                    PavementThickness,
                    DistrictIndex,
                    Side
                );
            }
        }
    }

    const float LocalStreetWidth = GetResolvedLocalStreetWidth();
    const float LocalPavementWidth = GetResolvedLocalPavementWidth();
    const float StationPavementWidth = GetResolvedStationPavementWidth();
    const float InnerLateral = SpineReservationWidth * 0.5f;
    const float OuterLateral = GetDevelopmentOuterLateral();

    auto AddCrossStreetGreybox = [this, PavementThickness](
        FName RoadKind,
        FName PavementKind,
        float RoadCentreChainage,
        float StartLateral,
        float EndLateral,
        float CarriagewayWidth,
        float PavementWidth,
        int32 DistrictIndex,
        int32 Side)
    {
        if (!IsDevelopmentSuitableAtChainage(RoadCentreChainage))
        {
            return;
        }
        AddGreyboxSpan(
            RoadKind,
            RoadCentreChainage,
            RoadCentreChainage,
            StartLateral,
            EndLateral,
            RoadThickness * 0.5f,
            CarriagewayWidth,
            RoadThickness,
            DistrictIndex,
            Side
        );

        for (const int32 EdgeSide : {-1, 1})
        {
            const float PavementChainage = RoadCentreChainage
                + EdgeSide * (CarriagewayWidth * 0.5f
                    + PavementWidth * 0.5f);
            AddGreyboxSpan(
                PavementKind,
                PavementChainage,
                PavementChainage,
                StartLateral,
                EndLateral,
                PavementThickness * 0.5f,
                PavementWidth,
                PavementThickness,
                DistrictIndex,
                Side
            );
        }
    };

    for (int32 DistrictIndex = -DistrictsBeforeStationZero;
         DistrictIndex < DistrictsAfterStationZero;
         ++DistrictIndex)
    {
        const float DistrictStart =
            static_cast<float>(DistrictIndex) * StationSpacing;
        for (const int32 Side : {-1, 1})
        {
            AddCrossStreetGreybox(
                TEXT("StationStreet"),
                TEXT("StationPavement"),
                DistrictStart,
                Side * GetSpineOuterKerbLateral(),
                Side * OuterLateral,
                StationCarriagewayWidth,
                StationPavementWidth,
                DistrictIndex,
                Side
            );
            for (int32 StreetIndex = 1;
                 StreetIndex < BlocksPerDistrict;
                 ++StreetIndex)
            {
                const float StreetChainage = DistrictStart
                    + GetInternalStreetCentreOffset(StreetIndex);
                AddCrossStreetGreybox(
                    TEXT("LocalStreet"),
                    TEXT("LocalPavement"),
                    StreetChainage,
                    Side * GetSpineOuterKerbLateral(),
                    Side * OuterLateral,
                    LocalCarriagewayWidth,
                    LocalPavementWidth,
                    DistrictIndex,
                    Side
                );
            }

            // The Spine serves the first row directly. The first parallel
            // local street therefore sits beyond that 100 m block, followed
            // by one at the outer edge of every additional generated row.
            for (int32 Boundary = 1;
                 Boundary <= DevelopmentRowsPerSide;
                 ++Boundary)
            {
                const float Lateral = Side * (
                    InnerLateral
                    + static_cast<float>(Boundary) * BlockSize
                    + (static_cast<float>(Boundary) - 0.5f)
                        * LocalStreetWidth
                );
                for (float Chainage = DistrictStart;
                     Chainage < DistrictStart + StationSpacing;
                     Chainage += Step)
                {
                    const float Next = FMath::Min(
                        Chainage + Step,
                        DistrictStart + StationSpacing
                    );
                    if (!IsDevelopmentSuitableAtChainage(
                            (Chainage + Next) * 0.5f))
                    {
                        continue;
                    }
                    AddGreyboxSpan(
                        TEXT("LocalStreet"),
                        Chainage,
                        Next,
                        Lateral,
                        Lateral,
                        RoadThickness * 0.5f,
                        LocalCarriagewayWidth,
                        RoadThickness,
                        DistrictIndex,
                        Side
                    );

                    for (const int32 EdgeSide : {-1, 1})
                    {
                        const float PavementLateral = Lateral
                            + EdgeSide * (LocalCarriagewayWidth * 0.5f
                                + LocalPavementWidth * 0.5f);
                        AddGreyboxSpan(
                            TEXT("LocalPavement"),
                            Chainage,
                            Next,
                            PavementLateral,
                            PavementLateral,
                            PavementThickness * 0.5f,
                            LocalPavementWidth,
                            PavementThickness,
                            DistrictIndex,
                            Side
                        );
                    }
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
        AddCrossStreetGreybox(
            TEXT("StationStreet"),
            TEXT("StationPavement"),
            TerminalChainage,
            Side * GetSpineOuterKerbLateral(),
            Side * OuterLateral,
            StationCarriagewayWidth,
            StationPavementWidth,
            DistrictsAfterStationZero,
            Side
        );
    }
}

void ASpineGenerator::AddStreetLampPlacement(
    FName RoadKind,
    int32 RoadIndex,
    int32 RoadSide,
    int32 EdgeSide,
    float Chainage,
    float Lateral,
    float RoadCentreChainage,
    float RoadCentreLateral
)
{
    if (!IsDevelopmentSuitableAtChainage(Chainage))
    {
        // The highway/monorail bridge alignment remains continuous, but its
        // ordinary roadside furniture waits for the bespoke bridge design.
        return;
    }
    FVector Location = GetSpineLocationAtChainage(Chainage, Lateral);
    Location.Z += RoadThickness + PavementKerbHeight;
    const FVector RoadCentre = GetSpineLocationAtChainage(
        RoadCentreChainage,
        RoadCentreLateral
    );
    FVector Facing = RoadCentre - Location;
    Facing.Z = 0.0f;
    if (!Facing.Normalize())
    {
        return;
    }

    FRotator Rotation = Facing.Rotation();
    Rotation.Yaw += StreetLampYawOffset;

    FSpineStreetLampPlacement& Placement =
        StreetLampPlacements.AddDefaulted_GetRef();
    Placement.RoadKind = RoadKind;
    Placement.RoadIndex = RoadIndex;
    Placement.RoadSide = RoadSide;
    Placement.EdgeSide = EdgeSide;
    Placement.Chainage = Chainage;
    Placement.Lateral = Lateral;
    Placement.Transform = FTransform(
        Rotation,
        Location,
        FVector::OneVector
    );
}

void ASpineGenerator::RebuildStreetLampPlacements()
{
    StreetLampPlacements.Reset();
    if (!bGenerateStreetLamps)
    {
        return;
    }

    const float Spacing = FMath::Max(500.0f, StreetLampSpacing);
    const float Setback = FMath::Max(0.0f, StreetLampSetback);
    const float JunctionClearance = FMath::Max(
        0.0f,
        StreetLampJunctionClearance
    );
    const float Start = GetMinimumChainage();
    const float End = GetMaximumChainage();
    const float CarriagewayOffset = GetSpineCarriagewayCentreOffset();
    const float LocalStreetWidth = GetResolvedLocalStreetWidth();

    auto IsSpineJunction = [this, LocalStreetWidth, JunctionClearance](
        float Chainage)
    {
        for (int32 DistrictIndex = -DistrictsBeforeStationZero;
             DistrictIndex <= DistrictsAfterStationZero;
             ++DistrictIndex)
        {
            const float DistrictStart =
                static_cast<float>(DistrictIndex) * StationSpacing;
            if (FMath::Abs(Chainage - DistrictStart)
                <= StationStreetWidth * 0.5f + JunctionClearance)
            {
                return true;
            }

            if (DistrictIndex == DistrictsAfterStationZero)
            {
                continue;
            }
            for (int32 StreetIndex = 1;
                 StreetIndex < BlocksPerDistrict;
                 ++StreetIndex)
            {
                const float StreetChainage = DistrictStart
                    + GetInternalStreetCentreOffset(StreetIndex);
                if (FMath::Abs(Chainage - StreetChainage)
                    <= LocalStreetWidth * 0.5f + JunctionClearance)
                {
                    return true;
                }
            }
        }
        return false;
    };

    // Only the parcel-facing edge of each Spine carriageway needs a separate
    // lamp line; the paved monorail realm already occupies the inner edge.
    // The two boulevard sides are staggered by half an interval.
    for (const int32 RoadSide : {-1, 1})
    {
        const float RoadCentreLateral = RoadSide * CarriagewayOffset;
        const int32 EdgeSide = RoadSide;
        const float Lateral = RoadCentreLateral
            + EdgeSide * (HighwayCarriagewayWidth * 0.5f + Setback);
        const float Phase = RoadSide > 0 ? Spacing * 0.5f : 0.0f;
        float Chainage = Phase + Spacing * static_cast<float>(
            FMath::CeilToInt((Start - Phase) / Spacing)
        );
        for (; Chainage <= End + KINDA_SMALL_NUMBER; Chainage += Spacing)
        {
            if (!IsSpineJunction(Chainage))
            {
                AddStreetLampPlacement(
                    TEXT("Highway"),
                    RoadSide,
                    RoadSide,
                    EdgeSide,
                    Chainage,
                    Lateral,
                    Chainage,
                    RoadCentreLateral
                );
            }
        }
    }

    if (LocalStreetWidth <= KINDA_SMALL_NUMBER)
    {
        return;
    }

    const float InnerLateral = SpineReservationWidth * 0.5f;

    // Longitudinal local roads begin after the first parcel row; there is no
    // duplicate frontage road beside the Spine. Generate inside each exact
    // 100 m bay so lamp points never land in cross-streets.
    for (const int32 RoadSide : {-1, 1})
    {
        for (int32 Boundary = 1;
             Boundary <= DevelopmentRowsPerSide;
             ++Boundary)
        {
            const float RoadCentreLateral = RoadSide * (
                InnerLateral
                + static_cast<float>(Boundary) * BlockSize
                + (static_cast<float>(Boundary) - 0.5f)
                    * LocalStreetWidth
            );
            const int32 RoadIndex = RoadSide
                * (Boundary + 1);

            for (const int32 EdgeSide : {-1, 1})
            {
                const float Lateral = RoadCentreLateral
                    + EdgeSide * (LocalCarriagewayWidth * 0.5f + Setback);
                const float Stagger = EdgeSide > 0
                    ? Spacing * 0.5f
                    : 0.0f;

                for (int32 DistrictIndex = -DistrictsBeforeStationZero;
                     DistrictIndex < DistrictsAfterStationZero;
                     ++DistrictIndex)
                {
                    const float DistrictStart =
                        static_cast<float>(DistrictIndex) * StationSpacing;
                    for (int32 BayIndex = 0;
                         BayIndex < BlocksPerDistrict;
                         ++BayIndex)
                    {
                        const float ClearStart = DistrictStart
                            + StationStreetWidth * 0.5f
                            + BayIndex * (BlockSize + LocalStreetWidth);
                        const float ClearEnd = ClearStart + BlockSize;
                        for (float Chainage = ClearStart
                                 + JunctionClearance + Stagger;
                             Chainage <= ClearEnd - JunctionClearance
                                 + KINDA_SMALL_NUMBER;
                             Chainage += Spacing)
                        {
                            AddStreetLampPlacement(
                                TEXT("LocalLongitudinal"),
                                RoadIndex,
                                RoadSide,
                                EdgeSide,
                                Chainage,
                                Lateral,
                                Chainage,
                                RoadCentreLateral
                            );
                        }
                    }
                }
            }
        }
    }

    auto AddCrossStreet = [this, Spacing, Setback, JunctionClearance,
                           LocalStreetWidth, InnerLateral](
        FName RoadKind,
        int32 RoadIndex,
        int32 RoadSide,
        float RoadCentreChainage,
        float RoadWidth)
    {
        for (const int32 EdgeSide : {-1, 1})
        {
            const float Chainage = RoadCentreChainage
                + EdgeSide * (RoadWidth * 0.5f + Setback);
            const float Stagger = EdgeSide > 0
                ? Spacing * 0.5f
                : 0.0f;

            for (int32 RowIndex = 0;
                 RowIndex < DevelopmentRowsPerSide;
                 ++RowIndex)
            {
                const float ClearStart = InnerLateral
                    + RowIndex * (BlockSize + LocalStreetWidth);
                const float ClearEnd = ClearStart + BlockSize;
                for (float Distance = ClearStart
                         + JunctionClearance + Stagger;
                     Distance <= ClearEnd - JunctionClearance
                         + KINDA_SMALL_NUMBER;
                     Distance += Spacing)
                {
                    const float Lateral = RoadSide * Distance;
                    AddStreetLampPlacement(
                        RoadKind,
                        RoadIndex,
                        RoadSide,
                        EdgeSide,
                        Chainage,
                        Lateral,
                        RoadCentreChainage,
                        Lateral
                    );
                }
            }
        }
    };

    // Cross-streets: the station road at every district boundary plus the
    // eight smaller streets between its nine parcel bays.
    for (int32 DistrictIndex = -DistrictsBeforeStationZero;
         DistrictIndex < DistrictsAfterStationZero;
         ++DistrictIndex)
    {
        const float DistrictStart =
            static_cast<float>(DistrictIndex) * StationSpacing;
        for (const int32 RoadSide : {-1, 1})
        {
            AddCrossStreet(
                TEXT("StationStreet"),
                DistrictIndex,
                RoadSide,
                DistrictStart,
                StationCarriagewayWidth
            );
            for (int32 StreetIndex = 1;
                 StreetIndex < BlocksPerDistrict;
                 ++StreetIndex)
            {
                AddCrossStreet(
                    TEXT("LocalCrossStreet"),
                    DistrictIndex * BlocksPerDistrict + StreetIndex,
                    RoadSide,
                    DistrictStart
                        + GetInternalStreetCentreOffset(StreetIndex),
                    LocalCarriagewayWidth
                );
            }
        }
    }

    const float TerminalChainage = GetMaximumChainage();
    for (const int32 RoadSide : {-1, 1})
    {
        AddCrossStreet(
            TEXT("StationStreet"),
            DistrictsAfterStationZero,
            RoadSide,
            TerminalChainage,
            StationCarriagewayWidth
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
