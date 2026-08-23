#include "AvenorStripTerrainGenerator.h"
#include "AvenorTerrainData.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "LevelEditorViewport.h"
#include "UnrealEdGlobals.h"

namespace UE::Avenor::BiomeProbe
{
static TAutoConsoleVariable<int32> CVarEnabled(
    TEXT("avenor.BiomeProbe"),
    1,
    TEXT("Show the Avenor editor-camera biome/climate probe. 1 = on, 0 = off."),
    ECVF_Default
);

static TAutoConsoleVariable<float> CVarUpdateSeconds(
    TEXT("avenor.BiomeProbeUpdateSeconds"),
    0.20f,
    TEXT("Update interval for the editor-camera biome probe."),
    ECVF_Default
);

struct FTexturePixels
{
    UTexture2D* Texture = nullptr;
    int32 Width = 0;
    int32 Height = 0;
    TArray<FColor> Pixels;

    void Reset()
    {
        Texture = nullptr;
        Width = 0;
        Height = 0;
        Pixels.Reset();
    }

    bool Refresh(UTexture2D* InTexture)
    {
        if (!InTexture || InTexture->Source.GetFormat(0) != TSF_BGRA8)
        {
            Reset();
            return false;
        }
        const int32 NewWidth = InTexture->Source.GetSizeX();
        const int32 NewHeight = InTexture->Source.GetSizeY();
        if (Texture == InTexture && Width == NewWidth && Height == NewHeight
            && Pixels.Num() == Width * Height)
        {
            return true;
        }
        TArray64<uint8> RawMip;
        if (!InTexture->Source.GetMipData(RawMip, 0, nullptr)
            || RawMip.Num() != static_cast<int64>(NewWidth) * NewHeight * sizeof(FColor))
        {
            Reset();
            return false;
        }
        Texture = InTexture;
        Width = NewWidth;
        Height = NewHeight;
        Pixels.SetNumUninitialized(Width * Height);
        FMemory::Memcpy(Pixels.GetData(), RawMip.GetData(), RawMip.Num());
        return true;
    }

    const FColor* Sample(const FBox2D& WorldBounds, const FVector2D& Position) const
    {
        if (!WorldBounds.bIsValid || Width <= 0 || Height <= 0 || Pixels.IsEmpty())
        {
            return nullptr;
        }
        const FVector2D Size = WorldBounds.GetSize();
        if (Size.X <= UE_DOUBLE_SMALL_NUMBER || Size.Y <= UE_DOUBLE_SMALL_NUMBER
            || !WorldBounds.IsInside(Position))
        {
            return nullptr;
        }
        const double U = FMath::Clamp((Position.X - WorldBounds.Min.X) / Size.X, 0.0, 0.999999);
        const double V = FMath::Clamp((Position.Y - WorldBounds.Min.Y) / Size.Y, 0.0, 0.999999);
        const int32 X = FMath::Clamp(FMath::FloorToInt(U * Width), 0, Width - 1);
        const int32 Y = FMath::Clamp(FMath::FloorToInt(V * Height), 0, Height - 1);
        return &Pixels[Y * Width + X];
    }
};

static EAvenorBiomeClass ClosestBiomeColour(const FColor& Colour)
{
    static const EAvenorBiomeClass Biomes[] = {
        EAvenorBiomeClass::ColdDry, EAvenorBiomeClass::ColdMoist,
        EAvenorBiomeClass::TemperateDry, EAvenorBiomeClass::TemperateMoist,
        EAvenorBiomeClass::WarmDry, EAvenorBiomeClass::WarmMoist,
        EAvenorBiomeClass::HotDry, EAvenorBiomeClass::HotWet,
        EAvenorBiomeClass::AlpineTundra, EAvenorBiomeClass::SnowIce,
        EAvenorBiomeClass::Wetland, EAvenorBiomeClass::Oasis,
        EAvenorBiomeClass::Riverbed, EAvenorBiomeClass::Riverbank,
        EAvenorBiomeClass::Lakebed, EAvenorBiomeClass::Lakeshore
    };
    int64 BestDistance = MAX_int64;
    EAvenorBiomeClass Best = EAvenorBiomeClass::TemperateMoist;
    for (EAvenorBiomeClass Biome : Biomes)
    {
        const FColor Candidate = UAvenorTerrainData::GetBiomeColour(Biome);
        const int64 DR = static_cast<int64>(Colour.R) - Candidate.R;
        const int64 DG = static_cast<int64>(Colour.G) - Candidate.G;
        const int64 DB = static_cast<int64>(Colour.B) - Candidate.B;
        const int64 Distance = DR * DR + DG * DG + DB * DB;
        if (Distance < BestDistance)
        {
            BestDistance = Distance;
            Best = Biome;
        }
    }
    return Best;
}

static FString BiomeName(EAvenorBiomeClass Biome)
{
    if (const UEnum* Enum = StaticEnum<EAvenorBiomeClass>())
    {
        return Enum->GetDisplayNameTextByValue(static_cast<int64>(Biome)).ToString();
    }
    return TEXT("Unknown");
}

static const TCHAR* TemperatureLabel(double Value)
{
    if (Value < 0.25) return TEXT("Cold");
    if (Value < 0.50) return TEXT("Temperate");
    if (Value < 0.75) return TEXT("Warm");
    return TEXT("Hot");
}

static const TCHAR* MoistureLabel(double Value)
{
    if (Value < 0.33) return TEXT("Dry");
    if (Value < 0.66) return TEXT("Moist");
    return TEXT("Wet");
}

static const TCHAR* ClassifyFinalLandform(const FAvenorTerrainSample& Centre)
{
    const double ElevationMetres = FMath::Max(0.0f, Centre.Height) / 100.0;
    if (Centre.Mountain >= 0.32f
        || (ElevationMetres > 700.0 && Centre.Slope > 0.014f))
    {
        return TEXT("Mountain");
    }
    if (Centre.Hill >= 0.28f
        || Centre.Slope > 0.010f
        || ElevationMetres > 280.0)
    {
        return TEXT("Hills / upland");
    }
    if (Centre.Desert >= 0.45f)
    {
        return TEXT("Arid lowland / plateau");
    }
    return TEXT("Lowland / plains");
}

class FCameraProbe
{
public:
    FCameraProbe()
    {
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateRaw(this, &FCameraProbe::Tick));
    }
    ~FCameraProbe()
    {
        if (TickHandle.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        }
    }

private:
    void ResetBakedCaches(const UAvenorTerrainData* Data)
    {
        // Do not retain or reset decompressed terrain chunks on this static
        // live-coding object. FAvenorTerrainSampleChunk lives in the runtime
        // module; destroying a TMap of those chunks from a replaced editor
        // patch can cross module generations and crash during Live Coding.
        // Terrain sampling instead uses a short-lived cache inside each tick.
        ClimatePixels.Reset();
        BaseBiomePixels.Reset();
        LocalBiomePixels.Reset();
        CachedData = Data;
        CachedGeneratedAtUtc = Data ? Data->GeneratedAtUtc : FDateTime();
        CachedSettingsHash = Data ? Data->SettingsHash : FString();
    }

    bool Tick(float DeltaSeconds)
    {
        if (CVarEnabled.GetValueOnGameThread() == 0 || !GEditor || !GEngine)
        {
            return true;
        }
        Accumulator += DeltaSeconds;
        if (Accumulator < FMath::Max(0.05f, CVarUpdateSeconds.GetValueOnGameThread()))
        {
            return true;
        }
        Accumulator = 0.0f;

        UWorld* World = GEditor->GetEditorWorldContext().World();
        FLevelEditorViewportClient* ViewClient = GCurrentLevelEditingViewportClient;
        if (!World || !ViewClient)
        {
            return true;
        }

        AAvenorStripTerrainGenerator* Generator = nullptr;
        for (TActorIterator<AAvenorStripTerrainGenerator> It(World); It; ++It)
        {
            Generator = *It;
            break;
        }
        if (!Generator)
        {
            return true;
        }

        UAvenorTerrainData* Data = Generator->BakedTerrainData.LoadSynchronous();
        if (!Data || !Data->HasValidData())
        {
            ResetBakedCaches(nullptr);
            GEngine->AddOnScreenDebugMessage(
                0x0A7E0B10, 0.5f, FColor::White,
                TEXT("AVENOR LOCATION PROBE\nBaked terrain data unavailable"), false);
            return true;
        }

        if (CachedData.Get() != Data
            || CachedGeneratedAtUtc != Data->GeneratedAtUtc
            || CachedSettingsHash != Data->SettingsHash)
        {
            ResetBakedCaches(Data);
        }

        // Intentional semantics: report the world-space XY directly beneath
        // the editor camera, not the centre of the view or a cursor raycast.
        const FVector Camera = ViewClient->GetViewLocation();
        const FVector2D XY(Camera.X, Camera.Y);

        // Keep decompressed terrain data local to this tick. Apart from being
        // live-coding safe, this guarantees a rebake can never leave the probe
        // serving stale chunks from the previous geography.
        FAvenorTerrainHeightChunkCache TerrainCache;
        FAvenorTerrainSample Terrain;
        const bool bTerrain = Data->SampleTerrain(XY, Terrain, TerrainCache);
        float FinalHeight = Terrain.Height;
        bool bWaterAffected = false;
        if (bTerrain)
        {
            Data->SampleFinalHeight(XY, FinalHeight, TerrainCache, &bWaterAffected);
        }

        const FBox2D ClimateBounds = Data->WorldClimateMaps.WorldBounds;
        UTexture2D* ClimateTexture = Data->WorldClimateMaps.ClimateFilterTexture.LoadSynchronous();
        UTexture2D* BaseBiomeTexture = Data->WorldClimateMaps.BaseBiomeTexture.LoadSynchronous();
        UTexture2D* LocalBiomeTexture = Data->WorldClimateMaps.LocalBiomeTexture.LoadSynchronous();
        const bool bClimateReady = ClimateBounds.bIsValid
            && ClimateBounds.IsInside(XY)
            && ClimatePixels.Refresh(ClimateTexture)
            && BaseBiomePixels.Refresh(BaseBiomeTexture)
            && LocalBiomePixels.Refresh(LocalBiomeTexture);

        FString Status;
        if (bClimateReady)
        {
            const FColor* Climate = ClimatePixels.Sample(ClimateBounds, XY);
            const FColor* BaseBiomePixel = BaseBiomePixels.Sample(ClimateBounds, XY);
            const FColor* LocalBiomePixel = LocalBiomePixels.Sample(ClimateBounds, XY);
            if (Climate && BaseBiomePixel)
            {
                const EAvenorBiomeClass BaseBiome = ClosestBiomeColour(*BaseBiomePixel);
                const bool bHasLocalOverride = LocalBiomePixel && LocalBiomePixel->A > 0;
                const EAvenorBiomeClass FinalBiome = bHasLocalOverride
                    ? ClosestBiomeColour(*LocalBiomePixel)
                    : BaseBiome;
                const double MacroTemperature = Climate->R / 255.0;
                const double MacroMoisture = Climate->G / 255.0;
                const double LocalTemperature = Climate->B / 255.0;
                const double LocalMoisture = Climate->A / 255.0;
                Status = FString::Printf(
                    TEXT("AVENOR LOCATION PROBE\nBiome: %s%s   Base: %s\nRegional: %s / %s   T %.2f  M %.2f\nLocal: %s / %s   T %.2f  M %.2f\nLandform: %s   Elevation: %s%s   Drainage: %s\nWorld: X %.2f km   Y %.2f km"),
                    *BiomeName(FinalBiome),
                    bHasLocalOverride ? TEXT(" (local override)") : TEXT(""),
                    *BiomeName(BaseBiome),
                    TemperatureLabel(MacroTemperature), MoistureLabel(MacroMoisture),
                    MacroTemperature, MacroMoisture,
                    TemperatureLabel(LocalTemperature), MoistureLabel(LocalMoisture),
                    LocalTemperature, LocalMoisture,
                    bTerrain ? ClassifyFinalLandform(Terrain) : TEXT("Unknown"),
                    bTerrain ? *FString::Printf(TEXT("%.0f m"), FinalHeight / 100.0f) : TEXT("n/a"),
                    bWaterAffected ? TEXT(" (water)") : TEXT(""),
                    bTerrain ? *FString::Printf(TEXT("%.1f km2"), Terrain.Accumulation) : TEXT("n/a"),
                    Camera.X / 100000.0, Camera.Y / 100000.0);
            }
        }

        if (Status.IsEmpty())
        {
            Status = bTerrain
                ? FString::Printf(
                    TEXT("AVENOR LOCATION PROBE\nClimate maps temporarily unavailable\nLandform: %s   Elevation: %.0f m%s   Drainage: %.1f km2\nSlope: %.3f   Mountain %.2f   Hill %.2f\nWorld: X %.2f km   Y %.2f km"),
                    ClassifyFinalLandform(Terrain), FinalHeight / 100.0f,
                    bWaterAffected ? TEXT(" (water)") : TEXT(""),
                    Terrain.Accumulation, Terrain.Slope, Terrain.Mountain, Terrain.Hill,
                    Camera.X / 100000.0, Camera.Y / 100000.0)
                : FString::Printf(
                    TEXT("AVENOR LOCATION PROBE\nOutside baked terrain\nWorld: X %.2f km   Y %.2f km"),
                    Camera.X / 100000.0, Camera.Y / 100000.0);
        }

        GEngine->AddOnScreenDebugMessage(
            0x0A7E0B10,
            FMath::Max(0.35f, CVarUpdateSeconds.GetValueOnGameThread() * 2.0f),
            FColor::White,
            Status,
            false);
        return true;
    }

    FTSTicker::FDelegateHandle TickHandle;
    float Accumulator = 0.0f;
    TWeakObjectPtr<const UAvenorTerrainData> CachedData;
    FDateTime CachedGeneratedAtUtc;
    FString CachedSettingsHash;
    FTexturePixels ClimatePixels;
    FTexturePixels BaseBiomePixels;
    FTexturePixels LocalBiomePixels;
};

static FCameraProbe GCameraProbe;
} // namespace UE::Avenor::BiomeProbe
