#include "AvenorStripTerrainGenerator.h"
#include "AvenorTerrainData.h"

#include "Containers/Ticker.h"
#include "DrawDebugHelpers.h"
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

    bool Refresh(UTexture2D* InTexture)
    {
        if (!InTexture || InTexture->Source.GetFormat(0) != TSF_BGRA8)
        {
            Texture = nullptr;
            Pixels.Reset();
            Width = Height = 0;
            return false;
        }

        const int32 NewWidth = InTexture->Source.GetSizeX();
        const int32 NewHeight = InTexture->Source.GetSizeY();
        if (Texture == InTexture
            && Width == NewWidth
            && Height == NewHeight
            && Pixels.Num() == Width * Height)
        {
            return true;
        }

        TArray64<uint8> RawMip;
        if (!InTexture->Source.GetMipData(RawMip, 0, nullptr)
            || RawMip.Num() != static_cast<int64>(NewWidth) * NewHeight * sizeof(FColor))
        {
            Texture = nullptr;
            Pixels.Reset();
            Width = Height = 0;
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
        EAvenorBiomeClass::ColdDry,
        EAvenorBiomeClass::ColdMoist,
        EAvenorBiomeClass::TemperateDry,
        EAvenorBiomeClass::TemperateMoist,
        EAvenorBiomeClass::WarmDry,
        EAvenorBiomeClass::WarmMoist,
        EAvenorBiomeClass::HotDry,
        EAvenorBiomeClass::HotWet,
        EAvenorBiomeClass::AlpineTundra,
        EAvenorBiomeClass::SnowIce,
        EAvenorBiomeClass::Wetland,
        EAvenorBiomeClass::Oasis,
        EAvenorBiomeClass::Riverbed,
        EAvenorBiomeClass::Riverbank,
        EAvenorBiomeClass::Lakebed,
        EAvenorBiomeClass::Lakeshore
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

static const TCHAR* ClassifyFinalLandform(
    const UAvenorTerrainData& Data,
    const FVector2D& Position,
    const FAvenorTerrainSample& Centre,
    FAvenorTerrainHeightChunkCache& Cache
)
{
    // Diagnose the terrain that actually survived erosion rather than simply
    // echoing the original structural mask.  Mountain is a geometric landform;
    // alpine is a separate climate/biome decision.
    const double NearRadius = FMath::Max(60000.0, Data.CellSize * 12.0);
    const double FarRadius = FMath::Max(180000.0, Data.CellSize * 36.0);
    static const FVector2D Directions[] = {
        FVector2D(1,0), FVector2D(-1,0), FVector2D(0,1), FVector2D(0,-1),
        FVector2D(0.70710678,0.70710678), FVector2D(-0.70710678,0.70710678),
        FVector2D(0.70710678,-0.70710678), FVector2D(-0.70710678,-0.70710678)
    };

    double NearMin = Centre.Height;
    double NearMax = Centre.Height;
    double FarMin = Centre.Height;
    double FarMax = Centre.Height;
    for (const FVector2D& Direction : Directions)
    {
        FAvenorTerrainSample Sample;
        if (Data.SampleTerrain(Position + Direction * NearRadius, Sample, Cache))
        {
            NearMin = FMath::Min(NearMin, static_cast<double>(Sample.Height));
            NearMax = FMath::Max(NearMax, static_cast<double>(Sample.Height));
        }
        if (Data.SampleTerrain(Position + Direction * FarRadius, Sample, Cache))
        {
            FarMin = FMath::Min(FarMin, static_cast<double>(Sample.Height));
            FarMax = FMath::Max(FarMax, static_cast<double>(Sample.Height));
        }
    }

    const double NearRelief = FMath::Max(0.0, NearMax - NearMin);
    const double FarRelief = FMath::Max(0.0, FarMax - FarMin);
    const double NearProminence = FMath::Max(0.0, static_cast<double>(Centre.Height) - NearMin);
    const double FarProminence = FMath::Max(0.0, static_cast<double>(Centre.Height) - FarMin);
    const double ElevationMetres = FMath::Max(0.0, static_cast<double>(Centre.Height)) / 100.0;
    const double Prominence = FMath::Max(NearProminence, FarProminence * 0.72);

    const bool bMountain = Centre.Mountain >= 0.34f
        || ((ElevationMetres > 450.0 || Centre.Slope > 0.025f)
            && FarRelief > 28000.0
            && Prominence > 12000.0)
        || (ElevationMetres > 850.0
            && FarRelief > 18000.0
            && Centre.Slope > 0.018f);
    if (bMountain)
    {
        return TEXT("Mountain");
    }

    const bool bUpland = Centre.Hill >= 0.32f
        || (NearRelief > 5000.0 && NearProminence > 3500.0)
        || (ElevationMetres > 250.0
            && (NearRelief > 3500.0 || Centre.Slope > 0.012f));
    if (bUpland)
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
            FTickerDelegate::CreateRaw(this, &FCameraProbe::Tick)
        );
    }

    ~FCameraProbe()
    {
        if (TickHandle.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        }
    }

private:
    bool Tick(float DeltaSeconds)
    {
        if (CVarEnabled.GetValueOnGameThread() == 0 || !GEditor)
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
            return true;
        }

        const FVector Camera = ViewClient->GetViewLocation();
        const FVector2D XY(Camera.X, Camera.Y);
        const FBox2D ClimateBounds = Data->WorldClimateMaps.WorldBounds;
        if (!ClimateBounds.bIsValid || !ClimateBounds.IsInside(XY))
        {
            return true;
        }

        UTexture2D* ClimateTexture = Data->WorldClimateMaps.ClimateFilterTexture.LoadSynchronous();
        UTexture2D* BaseBiomeTexture = Data->WorldClimateMaps.BaseBiomeTexture.LoadSynchronous();
        UTexture2D* LocalBiomeTexture = Data->WorldClimateMaps.LocalBiomeTexture.LoadSynchronous();
        if (!ClimatePixels.Refresh(ClimateTexture)
            || !BaseBiomePixels.Refresh(BaseBiomeTexture)
            || !LocalBiomePixels.Refresh(LocalBiomeTexture))
        {
            return true;
        }

        const FColor* Climate = ClimatePixels.Sample(ClimateBounds, XY);
        const FColor* BaseBiomePixel = BaseBiomePixels.Sample(ClimateBounds, XY);
        const FColor* LocalBiomePixel = LocalBiomePixels.Sample(ClimateBounds, XY);
        if (!Climate || !BaseBiomePixel)
        {
            return true;
        }

        const EAvenorBiomeClass BaseBiome = ClosestBiomeColour(*BaseBiomePixel);
        const bool bHasLocalOverride = LocalBiomePixel && LocalBiomePixel->A > 0;
        const EAvenorBiomeClass FinalBiome = bHasLocalOverride
            ? ClosestBiomeColour(*LocalBiomePixel)
            : BaseBiome;

        const double MacroTemperature = Climate->R / 255.0;
        const double MacroMoisture = Climate->G / 255.0;
        const double LocalTemperature = Climate->B / 255.0;
        const double LocalMoisture = Climate->A / 255.0;

        FAvenorTerrainSample Terrain;
        const bool bTerrain = Data->SampleTerrain(XY, Terrain, TerrainCache);
        const TCHAR* Landform = bTerrain
            ? ClassifyFinalLandform(*Data, XY, Terrain, TerrainCache)
            : TEXT("Unknown");

        const FString Status = FString::Printf(
            TEXT("AVENOR LOCATION PROBE\nBiome: %s%s   Base: %s\nRegional: %s / %s   T %.2f  M %.2f\nLocal: %s / %s   T %.2f  M %.2f\nLandform: %s   Elevation: %s   Drainage: %s\nWorld: X %.2f km   Y %.2f km"),
            *BiomeName(FinalBiome),
            bHasLocalOverride ? TEXT(" (local override)") : TEXT(""),
            *BiomeName(BaseBiome),
            TemperatureLabel(MacroTemperature), MoistureLabel(MacroMoisture),
            MacroTemperature, MacroMoisture,
            TemperatureLabel(LocalTemperature), MoistureLabel(LocalMoisture),
            LocalTemperature, LocalMoisture,
            Landform,
            bTerrain ? *FString::Printf(TEXT("%.0f m"), Terrain.Height / 100.0f) : TEXT("n/a"),
            bTerrain ? *FString::Printf(TEXT("%.1f km2"), Terrain.Accumulation) : TEXT("n/a"),
            Camera.X / 100000.0,
            Camera.Y / 100000.0
        );

        const FVector TextLocation = Camera
            + ViewClient->GetViewRotation().Vector() * 1200.0
            + FVector(0.0, 0.0, 220.0);
        DrawDebugString(
            World,
            TextLocation,
            Status,
            nullptr,
            FColor::White,
            FMath::Max(0.10f, CVarUpdateSeconds.GetValueOnGameThread() * 1.35f),
            true,
            1.05f
        );
        if (GEngine)
        {
            GEngine->AddOnScreenDebugMessage(
                0x0A7E0B10,
                FMath::Max(0.30f, CVarUpdateSeconds.GetValueOnGameThread() * 2.0f),
                FColor::White,
                Status,
                false
            );
        }

        return true;
    }

    FTSTicker::FDelegateHandle TickHandle;
    float Accumulator = 0.0f;
    FTexturePixels ClimatePixels;
    FTexturePixels BaseBiomePixels;
    FTexturePixels LocalBiomePixels;
    FAvenorTerrainHeightChunkCache TerrainCache;
};

static FCameraProbe GCameraProbe;
} // namespace UE::Avenor::BiomeProbe
