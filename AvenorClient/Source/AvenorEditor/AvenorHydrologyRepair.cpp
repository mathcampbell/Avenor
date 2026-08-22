#include "AvenorStripTerrainGenerator.h"
#include "AvenorTerrainData.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "WaterBodyRiverActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyHeightmapSettings.h"
#include "WaterBodyWeightmapSettings.h"
#include "WaterCurveSettings.h"

namespace UE::Avenor::HydrologyRepair
{
static constexpr TCHAR RepairMarker[] = TEXT("|hydrologyTopology=v2");

static double SegmentDistance(
    const FVector2D& Point,
    const FVector& A3,
    const FVector& B3,
    double* OutAlpha = nullptr)
{
    const FVector2D A(A3);
    const FVector2D B(B3);
    const FVector2D Segment = B - A;
    const double LengthSquared = Segment.SizeSquared();
    const double Alpha = LengthSquared > UE_DOUBLE_SMALL_NUMBER
        ? FMath::Clamp(
            FVector2D::DotProduct(Point - A, Segment) / LengthSquared,
            0.0,
            1.0)
        : 0.0;
    if (OutAlpha)
    {
        *OutAlpha = Alpha;
    }
    return FVector2D::Distance(Point, A + Segment * Alpha);
}

static bool IsNearWorldBoundary(
    const UAvenorTerrainData& Data,
    const FVector2D& Position)
{
    const double Margin = FMath::Max(1000.0, Data.CellSize * 1.6);
    return Position.X <= Data.WorldBounds.Min.X + Margin
        || Position.X >= Data.WorldBounds.Max.X - Margin
        || Position.Y <= Data.WorldBounds.Min.Y + Margin
        || Position.Y >= Data.WorldBounds.Max.Y - Margin;
}

static bool IsCredibleHeadwater(
    const UAvenorTerrainData& Data,
    const FAvenorBakedRiverReach& River,
    FAvenorTerrainHeightChunkCache& Cache)
{
    if (River.StartLakeIndex != INDEX_NONE || River.Points.IsEmpty())
    {
        return true;
    }

    FAvenorTerrainSample Sample;
    if (!Data.SampleTerrain(FVector2D(River.Points[0]), Sample, Cache))
    {
        return false;
    }

    const double ElevationMetres = FMath::Max(0.0f, Sample.Height) / 100.0;
    const double ReliefSupport = FMath::Max(
        static_cast<double>(Sample.Mountain),
        static_cast<double>(Sample.Hill) * 0.82);

    // Permanent headwaters should normally emerge from relief: uplands,
    // mountains, steep ground, or genuinely high terrain. Large lowland
    // catchments remain possible, but a small arbitrary lowland cell can no
    // longer become the visible beginning of a river just because it crossed
    // one accumulation threshold.
    const bool bReliefSource = ReliefSupport > 0.20
        || Sample.Slope > 0.016f
        || (ElevationMetres > 320.0 && Sample.Slope > 0.008f)
        || ElevationMetres > 650.0;

    const bool bLargeLowlandCatchment = River.DrainageArea >= 14.0
        && Sample.Slope > 0.0025f;

    return bReliefSource || bLargeLowlandCatchment;
}

static int32 FindDownstreamJoin(
    const TArray<FAvenorBakedRiverReach>& Rivers,
    int32 SourceIndex,
    double JoinTolerance,
    FVector& OutJoinPoint)
{
    if (!Rivers.IsValidIndex(SourceIndex)
        || Rivers[SourceIndex].Points.IsEmpty())
    {
        return INDEX_NONE;
    }

    const FAvenorBakedRiverReach& Source = Rivers[SourceIndex];
    const FVector& End = Source.Points.Last();
    const FVector2D EndXY(End);
    double BestScore = TNumericLimits<double>::Max();
    int32 BestReach = INDEX_NONE;
    FVector BestPoint = FVector::ZeroVector;

    // Prefer an exact reach-start confluence. ExtractRivers normally splits
    // the network at junctions, so this is the common case and keeps the river
    // graph topologically explicit.
    for (int32 TargetIndex = 0; TargetIndex < Rivers.Num(); ++TargetIndex)
    {
        if (TargetIndex == SourceIndex || Rivers[TargetIndex].Points.IsEmpty())
        {
            continue;
        }
        const FAvenorBakedRiverReach& Target = Rivers[TargetIndex];
        if (Target.DrainageArea + 0.01 < Source.DrainageArea * 0.72)
        {
            continue;
        }
        const FVector& Start = Target.Points[0];
        const double Distance = FVector2D::Distance(EndXY, FVector2D(Start));
        if (Distance <= JoinTolerance
            && Start.Z <= End.Z + 150.0
            && Distance < BestScore)
        {
            BestScore = Distance;
            BestReach = TargetIndex;
            BestPoint = Start;
        }
    }

    if (BestReach != INDEX_NONE)
    {
        OutJoinPoint = BestPoint;
        return BestReach;
    }

    // Recovery path for a junction that was simplified onto the interior of a
    // downstream spline. It is still required to join a larger/equal river and
    // the target water level must be downhill from the tributary endpoint.
    for (int32 TargetIndex = 0; TargetIndex < Rivers.Num(); ++TargetIndex)
    {
        if (TargetIndex == SourceIndex || Rivers[TargetIndex].Points.Num() < 2)
        {
            continue;
        }
        const FAvenorBakedRiverReach& Target = Rivers[TargetIndex];
        if (Target.DrainageArea + 0.01 < Source.DrainageArea * 0.72)
        {
            continue;
        }
        for (int32 SegmentIndex = 0;
             SegmentIndex + 1 < Target.Points.Num();
             ++SegmentIndex)
        {
            double Alpha = 0.0;
            const double Distance = SegmentDistance(
                EndXY,
                Target.Points[SegmentIndex],
                Target.Points[SegmentIndex + 1],
                &Alpha);
            if (Distance > JoinTolerance || Distance >= BestScore)
            {
                continue;
            }
            const FVector Candidate = FMath::Lerp(
                Target.Points[SegmentIndex],
                Target.Points[SegmentIndex + 1],
                Alpha);
            if (Candidate.Z > End.Z + 150.0)
            {
                continue;
            }
            BestScore = Distance;
            BestReach = TargetIndex;
            BestPoint = Candidate;
        }
    }

    if (BestReach != INDEX_NONE)
    {
        OutJoinPoint = BestPoint;
    }
    return BestReach;
}

static void EnforceDownhillToEnd(FAvenorBakedRiverReach& River)
{
    if (River.Points.Num() < 2)
    {
        return;
    }
    constexpr double MinimumGradient = 0.00020;
    for (int32 Index = River.Points.Num() - 2; Index >= 0; --Index)
    {
        const double Distance = FVector2D::Distance(
            FVector2D(River.Points[Index]),
            FVector2D(River.Points[Index + 1]));
        River.Points[Index].Z = FMath::Max(
            River.Points[Index].Z,
            River.Points[Index + 1].Z + Distance * MinimumGradient);
    }
}

static TArray<int32> BuildIncomingCounts(
    const TArray<FAvenorBakedRiverReach>& Rivers,
    double JoinTolerance)
{
    TArray<int32> Incoming;
    Incoming.Init(0, Rivers.Num());
    for (int32 SourceIndex = 0; SourceIndex < Rivers.Num(); ++SourceIndex)
    {
        if (Rivers[SourceIndex].Points.IsEmpty())
        {
            continue;
        }
        const FVector2D End(Rivers[SourceIndex].Points.Last());
        for (int32 TargetIndex = 0; TargetIndex < Rivers.Num(); ++TargetIndex)
        {
            if (TargetIndex == SourceIndex || Rivers[TargetIndex].Points.IsEmpty())
            {
                continue;
            }
            if (FVector2D::Distance(
                    End,
                    FVector2D(Rivers[TargetIndex].Points[0]))
                <= JoinTolerance)
            {
                ++Incoming[TargetIndex];
                break;
            }
        }
    }
    return Incoming;
}

static bool RepairRiverTopology(UAvenorTerrainData& Data)
{
    if (Data.Rivers.IsEmpty())
    {
        return false;
    }

    bool bChanged = false;
    const double JoinTolerance = FMath::Clamp(
        Data.CellSize * 1.45,
        5000.0,
        22000.0);

    // First make every non-lake, non-boundary reach terminate on a downstream
    // river. A dangling interior reach is invalid hydrology and is removed if
    // no downhill confluence can be recovered.
    TArray<bool> Keep;
    Keep.Init(true, Data.Rivers.Num());
    for (int32 Index = 0; Index < Data.Rivers.Num(); ++Index)
    {
        FAvenorBakedRiverReach& River = Data.Rivers[Index];
        if (River.Points.Num() < 2)
        {
            Keep[Index] = false;
            bChanged = true;
            continue;
        }

        if (Data.Lakes.IsValidIndex(River.EndLakeIndex))
        {
            River.Points.Last().Z = Data.Lakes[River.EndLakeIndex].SurfaceHeight;
            EnforceDownhillToEnd(River);
            continue;
        }
        if (IsNearWorldBoundary(Data, FVector2D(River.Points.Last())))
        {
            continue;
        }

        FVector JoinPoint;
        const int32 Downstream = FindDownstreamJoin(
            Data.Rivers, Index, JoinTolerance, JoinPoint);
        if (Downstream == INDEX_NONE)
        {
            Keep[Index] = false;
            bChanged = true;
            continue;
        }

        if (!River.Points.Last().Equals(JoinPoint, 1.0))
        {
            River.Points.Last() = JoinPoint;
            EnforceDownhillToEnd(River);
            bChanged = true;
        }
    }

    TArray<FAvenorBakedRiverReach> ValidRivers;
    ValidRivers.Reserve(Data.Rivers.Num());
    for (int32 Index = 0; Index < Data.Rivers.Num(); ++Index)
    {
        if (Keep[Index])
        {
            ValidRivers.Add(MoveTemp(Data.Rivers[Index]));
        }
    }
    Data.Rivers = MoveTemp(ValidRivers);

    // Remove implausible exposed lowland starts. Re-evaluate after each pass so
    // pruning a bad tributary cannot leave a tiny downstream reach appearing to
    // spring from nowhere. Large lowland catchments remain valid.
    FAvenorTerrainHeightChunkCache TerrainCache;
    for (int32 Pass = 0; Pass < 3 && !Data.Rivers.IsEmpty(); ++Pass)
    {
        const TArray<int32> Incoming = BuildIncomingCounts(
            Data.Rivers, JoinTolerance * 0.45);
        TArray<FAvenorBakedRiverReach> Pruned;
        Pruned.Reserve(Data.Rivers.Num());
        bool bRemovedThisPass = false;
        for (int32 Index = 0; Index < Data.Rivers.Num(); ++Index)
        {
            const FAvenorBakedRiverReach& River = Data.Rivers[Index];
            const bool bHeadwater = Incoming[Index] == 0
                && River.StartLakeIndex == INDEX_NONE;
            if (bHeadwater
                && !IsCredibleHeadwater(Data, River, TerrainCache))
            {
                bRemovedThisPass = true;
                bChanged = true;
                continue;
            }
            Pruned.Add(River);
        }
        Data.Rivers = MoveTemp(Pruned);
        if (!bRemovedThisPass)
        {
            break;
        }
    }

    // A final topology pass after pruning guarantees that every retained reach
    // ends at a lake, map edge, or a larger/equal river going downhill.
    Keep.Init(true, Data.Rivers.Num());
    for (int32 Index = 0; Index < Data.Rivers.Num(); ++Index)
    {
        FAvenorBakedRiverReach& River = Data.Rivers[Index];
        if (Data.Lakes.IsValidIndex(River.EndLakeIndex)
            || IsNearWorldBoundary(Data, FVector2D(River.Points.Last())))
        {
            continue;
        }
        FVector JoinPoint;
        if (FindDownstreamJoin(Data.Rivers, Index, JoinTolerance, JoinPoint)
            == INDEX_NONE)
        {
            Keep[Index] = false;
            bChanged = true;
            continue;
        }
        River.Points.Last() = JoinPoint;
        EnforceDownhillToEnd(River);
    }
    ValidRivers.Reset();
    for (int32 Index = 0; Index < Data.Rivers.Num(); ++Index)
    {
        if (Keep[Index])
        {
            ValidRivers.Add(MoveTemp(Data.Rivers[Index]));
        }
    }
    Data.Rivers = MoveTemp(ValidRivers);

    // Broad valleys are already produced by erosion. The active Water Body is
    // only the wetted channel plus a small riparian shoulder; it must not carve
    // a 100-300 metre artificial trench around a ten-metre tributary.
    for (FAvenorBakedRiverReach& River : Data.Rivers)
    {
        const double OldBank = River.BankWidth;
        const double OldValley = River.ValleyHalfWidth;
        const double Bank = FMath::Clamp(
            River.Width * (River.bIsCanyon ? 0.42 : 0.28),
            100.0,
            River.bIsCanyon ? 3000.0 : 1800.0);
        River.BankWidth = Bank;
        if (!River.bIsCanyon)
        {
            River.ValleyHalfWidth = FMath::Max(
                River.Width * 0.55 + Bank,
                River.Width * 0.72);
            River.ValleyDepth = FMath::Min(
                River.ValleyDepth,
                FMath::Max(River.Depth * 0.65, 120.0));
        }
        if (!FMath::IsNearlyEqual(OldBank, River.BankWidth, 1.0)
            || !FMath::IsNearlyEqual(OldValley, River.ValleyHalfWidth, 1.0))
        {
            bChanged = true;
        }
    }

    return bChanged;
}

struct FRiverBounds
{
    const FAvenorBakedRiverReach* River = nullptr;
    FBox2D Bounds = FBox2D(ForceInit);
};

static TArray<FRiverBounds> BuildRiverBounds(const UAvenorTerrainData& Data)
{
    TArray<FRiverBounds> Result;
    Result.Reserve(Data.Rivers.Num());
    for (const FAvenorBakedRiverReach& River : Data.Rivers)
    {
        if (River.Points.Num() < 2)
        {
            continue;
        }
        FRiverBounds& Item = Result.AddDefaulted_GetRef();
        Item.River = &River;
        for (const FVector& Point : River.Points)
        {
            Item.Bounds += FVector2D(Point);
        }
        Item.Bounds = Item.Bounds.ExpandBy(
            River.Width * 0.5 + River.BankWidth + Data.CellSize);
    }
    return Result;
}

static bool RewriteWaterTexture(
    UTexture2D* Texture,
    const FBox2D& Bounds,
    const UAvenorTerrainData& Data,
    const TArray<FRiverBounds>& RiverBounds)
{
    if (!Texture || !Bounds.bIsValid
        || Texture->Source.GetFormat(0) != TSF_BGRA8)
    {
        return false;
    }

    const int32 Width = Texture->Source.GetSizeX();
    const int32 Height = Texture->Source.GetSizeY();
    if (Width <= 0 || Height <= 0)
    {
        return false;
    }

    TArray64<uint8> Raw;
    if (!Texture->Source.GetMipData(Raw, 0, nullptr)
        || Raw.Num() != static_cast<int64>(Width) * Height * sizeof(FColor))
    {
        return false;
    }

    TArray<FColor> Pixels;
    Pixels.SetNumUninitialized(Width * Height);
    FMemory::Memcpy(Pixels.GetData(), Raw.GetData(), Raw.Num());

    const FVector2D Size = Bounds.GetSize();
    const double PixelX = Size.X / Width;
    const double PixelY = Size.Y / Height;
    const double PixelDiameter = FMath::Max(PixelX, PixelY);
    const double PixelRadius = 0.5 * FMath::Sqrt(
        PixelX * PixelX + PixelY * PixelY);

    for (int32 Y = 0; Y < Height; ++Y)
    {
        for (int32 X = 0; X < Width; ++X)
        {
            const int32 PixelIndex = Y * Width + X;
            FColor& Pixel = Pixels[PixelIndex];
            Pixel.R = 0;
            Pixel.G = 0;

            const FVector2D Position(
                Bounds.Min.X + (X + 0.5) * PixelX,
                Bounds.Min.Y + (Y + 0.5) * PixelY);
            double BedMask = 0.0;
            double BankMask = 0.0;
            for (const FRiverBounds& Item : RiverBounds)
            {
                if (!Item.River || !Item.Bounds.IsInside(Position))
                {
                    continue;
                }
                const FAvenorBakedRiverReach& River = *Item.River;
                double Distance = TNumericLimits<double>::Max();
                for (int32 SegmentIndex = 0;
                     SegmentIndex + 1 < River.Points.Num();
                     ++SegmentIndex)
                {
                    Distance = FMath::Min(
                        Distance,
                        SegmentDistance(
                            Position,
                            River.Points[SegmentIndex],
                            River.Points[SegmentIndex + 1]));
                }

                const double HalfWidth = FMath::Max(50.0, River.Width * 0.5);
                const double BankOuter = HalfWidth + FMath::Max(0.0, River.BankWidth);
                if (Distance <= HalfWidth + PixelRadius)
                {
                    const double GeometricCoverage = FMath::Clamp(
                        (HalfWidth + PixelRadius - Distance)
                            / FMath::Max(1.0, 2.0 * PixelRadius),
                        0.0,
                        1.0);
                    const double WidthCoverage = FMath::Clamp(
                        River.Width / FMath::Max(1.0, PixelDiameter),
                        0.0,
                        1.0);
                    BedMask = FMath::Max(
                        BedMask,
                        GeometricCoverage * WidthCoverage);
                }
                if (Distance > HalfWidth - PixelRadius
                    && Distance <= BankOuter + PixelRadius)
                {
                    const double BankCoverage = FMath::Clamp(
                        (2.0 * River.BankWidth)
                            / FMath::Max(1.0, PixelDiameter),
                        0.0,
                        1.0);
                    const double DistanceFade = 1.0 - FMath::Clamp(
                        (Distance - HalfWidth)
                            / FMath::Max(1.0, River.BankWidth + PixelRadius),
                        0.0,
                        1.0);
                    BankMask = FMath::Max(
                        BankMask,
                        BankCoverage * DistanceFade);
                }
            }

            Pixel.R = static_cast<uint8>(FMath::RoundToInt(
                FMath::Clamp(BedMask, 0.0, 1.0) * 255.0));
            Pixel.G = static_cast<uint8>(FMath::RoundToInt(
                FMath::Clamp(BankMask, 0.0, 1.0) * 255.0));
        }
    }

    Texture->Modify();
    Texture->Source.Init(
        Width,
        Height,
        1,
        1,
        TSF_BGRA8,
        reinterpret_cast<const uint8*>(Pixels.GetData()));
    Texture->UpdateResource();
    Texture->MarkPackageDirty();
    return true;
}

static bool ClearLegacyRiverBiomePixels(UTexture2D* Texture)
{
    if (!Texture || Texture->Source.GetFormat(0) != TSF_BGRA8)
    {
        return false;
    }
    const int32 Width = Texture->Source.GetSizeX();
    const int32 Height = Texture->Source.GetSizeY();
    TArray64<uint8> Raw;
    if (Width <= 0 || Height <= 0
        || !Texture->Source.GetMipData(Raw, 0, nullptr)
        || Raw.Num() != static_cast<int64>(Width) * Height * sizeof(FColor))
    {
        return false;
    }

    TArray<FColor> Pixels;
    Pixels.SetNumUninitialized(Width * Height);
    FMemory::Memcpy(Pixels.GetData(), Raw.GetData(), Raw.Num());
    const FColor Riverbed = UAvenorTerrainData::GetBiomeColour(
        EAvenorBiomeClass::Riverbed);
    const FColor Riverbank = UAvenorTerrainData::GetBiomeColour(
        EAvenorBiomeClass::Riverbank);
    bool bChanged = false;
    for (FColor& Pixel : Pixels)
    {
        const bool bRiverbed = Pixel.R == Riverbed.R
            && Pixel.G == Riverbed.G && Pixel.B == Riverbed.B;
        const bool bRiverbank = Pixel.R == Riverbank.R
            && Pixel.G == Riverbank.G && Pixel.B == Riverbank.B;
        if (Pixel.A > 0 && (bRiverbed || bRiverbank))
        {
            Pixel = FColor(0, 0, 0, 0);
            bChanged = true;
        }
    }
    if (!bChanged)
    {
        return false;
    }

    Texture->Modify();
    Texture->Source.Init(
        Width,
        Height,
        1,
        1,
        TSF_BGRA8,
        reinterpret_cast<const uint8*>(Pixels.GetData()));
    Texture->UpdateResource();
    Texture->MarkPackageDirty();
    return true;
}

static void RepairMaterialRiverMasks(UAvenorTerrainData& Data)
{
    UEditorAssetSubsystem* AssetSubsystem = GEditor
        ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()
        : nullptr;
    if (!AssetSubsystem)
    {
        return;
    }

    const TArray<FRiverBounds> Bounds = BuildRiverBounds(Data);
    auto SaveIfChanged = [&](UTexture2D* Texture, bool bChanged)
    {
        if (Texture && bChanged)
        {
            AssetSubsystem->SaveLoadedAsset(Texture, false);
        }
    };

    UTexture2D* WorldWater =
        Data.WorldClimateMaps.WaterSurfaceTexture.LoadSynchronous();
    SaveIfChanged(
        WorldWater,
        RewriteWaterTexture(
            WorldWater,
            Data.WorldClimateMaps.WorldBounds,
            Data,
            Bounds));
    UTexture2D* WorldLocal =
        Data.WorldClimateMaps.LocalBiomeTexture.LoadSynchronous();
    SaveIfChanged(WorldLocal, ClearLegacyRiverBiomePixels(WorldLocal));

    for (FAvenorClimateTileReference& Tile : Data.ClimateTiles)
    {
        UTexture2D* Water = Tile.WaterSurfaceTexture.LoadSynchronous();
        SaveIfChanged(
            Water,
            RewriteWaterTexture(
                Water,
                Tile.WorldBounds,
                Data,
                Bounds));
        UTexture2D* Local = Tile.LocalBiomeTexture.LoadSynchronous();
        SaveIfChanged(Local, ClearLegacyRiverBiomePixels(Local));
    }
}

static bool ApplyNarrowRiverActorSettings(
    AAvenorStripTerrainGenerator& Generator,
    const UAvenorTerrainData& Data)
{
    UWorld* World = Generator.GetWorld();
    if (!World)
    {
        return false;
    }

    const FName OwnerTag(*FString::Printf(
        TEXT("AvenorStripOwner_%s"),
        *Generator.GetFName().ToString()));
    bool bChanged = false;
    for (TActorIterator<AWaterBodyRiver> It(World); It; ++It)
    {
        AWaterBodyRiver* Water = *It;
        if (!Water || !Water->Tags.Contains(OwnerTag))
        {
            continue;
        }
        const FString Label = Water->GetActorLabel();
        const FString Prefix = TEXT("Avenor_Strip_River_");
        if (!Label.StartsWith(Prefix))
        {
            continue;
        }
        const int32 ReachIndex = FCString::Atoi(
            *Label.RightChop(Prefix.Len())) - 1;
        if (!Data.Rivers.IsValidIndex(ReachIndex))
        {
            continue;
        }

        const FAvenorBakedRiverReach& Reach = Data.Rivers[ReachIndex];
        const float Bank = static_cast<float>(FMath::Clamp(
            Reach.BankWidth,
            100.0,
            Reach.bIsCanyon ? 3000.0 : 1800.0));

        FWaterCurveSettings& Curve = const_cast<FWaterCurveSettings&>(
            Water->GetWaterCurveSettings());
        FWaterBodyHeightmapSettings& Heightmap =
            const_cast<FWaterBodyHeightmapSettings&>(
                Water->GetWaterHeightmapSettings());

        const bool bNeedsChange =
            !FMath::IsNearlyEqual(Curve.CurveRampWidth, Bank, 1.0f)
            || !FMath::IsNearlyEqual(
                Heightmap.FalloffSettings.FalloffWidth, Bank, 1.0f)
            || Heightmap.FalloffSettings.EdgeOffset > 1.0f;
        if (!bNeedsChange)
        {
            continue;
        }

        Water->Modify();
        Curve.bUseCurveChannel = true;
        Curve.ChannelDepth = static_cast<float>(FMath::Max(100.0, Reach.Depth));
        Curve.ChannelEdgeOffset = 0.0f;
        Curve.CurveRampWidth = Bank;

        Heightmap.FalloffSettings.FalloffMode = EWaterBrushFalloffMode::Width;
        Heightmap.FalloffSettings.FalloffWidth = Bank;
        Heightmap.FalloffSettings.EdgeOffset = 0.0f;
        Heightmap.Effects.SmoothBlending.InnerSmoothDistance = Bank * 0.16f;
        Heightmap.Effects.SmoothBlending.OuterSmoothDistance = Bank * 0.28f;

        TMap<FName, FWaterBodyWeightmapSettings>& Weightmaps =
            const_cast<TMap<FName, FWaterBodyWeightmapSettings>&>(
                Water->GetLayerWeightmapSettings());
        if (FWaterBodyWeightmapSettings* RiverBank =
            Weightmaps.Find(TEXT("RiverBank")))
        {
            RiverBank->EdgeOffset = Bank;
            RiverBank->FalloffWidth = Bank;
        }
        if (FWaterBodyWeightmapSettings* RiverBed =
            Weightmaps.Find(TEXT("RiverBed")))
        {
            RiverBed->EdgeOffset = 0.0f;
            RiverBed->FalloffWidth = FMath::Max(100.0f, Bank * 0.35f);
        }

        Water->PostEditChange();
        bChanged = true;
    }
    return bChanged;
}

class FHydrologyRepairTicker
{
public:
    FHydrologyRepairTicker()
    {
        Handle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateRaw(this, &FHydrologyRepairTicker::Tick),
            0.75f);
    }

    ~FHydrologyRepairTicker()
    {
        if (Handle.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(Handle);
        }
    }

private:
    bool Tick(float)
    {
        if (!GEditor || bBusy)
        {
            return true;
        }
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World)
        {
            return true;
        }

        TGuardValue<bool> BusyGuard(bBusy, true);
        for (TActorIterator<AAvenorStripTerrainGenerator> It(World); It; ++It)
        {
            AAvenorStripTerrainGenerator* Generator = *It;
            if (!Generator)
            {
                continue;
            }
            UAvenorTerrainData* Data =
                Generator->BakedTerrainData.LoadSynchronous();
            if (!Data || !Data->HasValidData())
            {
                continue;
            }

            const bool bNeedsTopologyRepair =
                !Data->GenerationSettingsSnapshot.Contains(RepairMarker);
            if (bNeedsTopologyRepair)
            {
                Data->Modify();
                RepairRiverTopology(*Data);
                RepairMaterialRiverMasks(*Data);
                Data->GenerationSettingsSnapshot += RepairMarker;
                Data->MarkPackageDirty();

                if (UEditorAssetSubsystem* AssetSubsystem =
                    GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
                {
                    AssetSubsystem->SaveLoadedAsset(Data, false);
                }

                // The bake has already finished by the time the ticker sees the
                // new asset. Rebuild from the repaired baked data rather than
                // generating geography again.
                Generator->RebuildWorldFromBakedData();
            }

            if (ApplyNarrowRiverActorSettings(*Generator, *Data))
            {
                Generator->RefreshMeshTerrainInPlace();
            }
        }
        return true;
    }

    FTSTicker::FDelegateHandle Handle;
    bool bBusy = false;
};

static FHydrologyRepairTicker GHydrologyRepairTicker;
} // namespace UE::Avenor::HydrologyRepair
