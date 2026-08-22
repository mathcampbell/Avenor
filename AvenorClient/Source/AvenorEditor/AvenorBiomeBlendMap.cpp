#include "AvenorTerrainData.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "HAL/IConsoleManager.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::Avenor::BiomeBlend
{
static TAutoConsoleVariable<float> CVarBlendWidthCm(
    TEXT("avenor.BiomeBlendWidthCm"),
    100000.0f,
    TEXT("World-space width in centimetres used when baking Avenor base-biome transitions. Default 100000 = 1 km."),
    ECVF_Default
);

static uint8 UnitToByte(double Value)
{
    return static_cast<uint8>(FMath::RoundToInt(FMath::Clamp(Value, 0.0, 1.0) * 255.0));
}

static EAvenorBiomeClass ClassifyClimateBiome(double Temperature, double Moisture)
{
    const bool bMoist = Moisture >= 0.5;
    if (Temperature < 0.25)
        return bMoist ? EAvenorBiomeClass::ColdMoist : EAvenorBiomeClass::ColdDry;
    if (Temperature < 0.50)
        return bMoist ? EAvenorBiomeClass::TemperateMoist : EAvenorBiomeClass::TemperateDry;
    if (Temperature < 0.75)
        return bMoist ? EAvenorBiomeClass::WarmMoist : EAvenorBiomeClass::WarmDry;
    return bMoist ? EAvenorBiomeClass::HotWet : EAvenorBiomeClass::HotDry;
}

static uint8 ClassifyBaseBiomeIndex(const FColor& ClimatePixel)
{
    // B/A are the final local climate after altitude/exposure correction.  The
    // regional R/G channels remain available for macro-climate diagnostics.
    const double Temperature = ClimatePixel.B / 255.0;
    const double Moisture = ClimatePixel.A / 255.0;
    const int32 TemperatureBand = FMath::Clamp(FMath::FloorToInt(Temperature * 4.0), 0, 3);
    const int32 MoistureBand = Moisture >= 0.5 ? 1 : 0;
    return static_cast<uint8>(TemperatureBand * 2 + MoistureBand);
}

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

static bool IsHydrologyOverride(EAvenorBiomeClass Biome)
{
    return Biome == EAvenorBiomeClass::Wetland
        || Biome == EAvenorBiomeClass::Oasis
        || Biome == EAvenorBiomeClass::Riverbed
        || Biome == EAvenorBiomeClass::Riverbank
        || Biome == EAvenorBiomeClass::Lakebed
        || Biome == EAvenorBiomeClass::Lakeshore;
}

static void RefreshTexturePixels(UTexture2D& Texture, const TArray<FColor>& Pixels)
{
    const int32 Width = Texture.Source.GetSizeX();
    const int32 Height = Texture.Source.GetSizeY();
    if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
    {
        return;
    }
    Texture.Modify();
    Texture.Source.Init(
        Width,
        Height,
        1,
        1,
        TSF_BGRA8,
        reinterpret_cast<const uint8*>(Pixels.GetData())
    );
    Texture.SRGB = false;
    Texture.CompressionSettings = TC_VectorDisplacementmap;
    Texture.MipGenSettings = TMGS_NoMipmaps;
    Texture.UpdateResource();
    Texture.MarkPackageDirty();
}

static bool RebuildLocalClimateMap(
    UAvenorTerrainData& TerrainData,
    const FBox2D& WorldBounds,
    UTexture2D* ClimateTexture,
    UTexture2D* LocalBiomeTexture
)
{
    if (!ClimateTexture || !LocalBiomeTexture
        || ClimateTexture->Source.GetFormat(0) != TSF_BGRA8
        || LocalBiomeTexture->Source.GetFormat(0) != TSF_BGRA8)
    {
        return false;
    }
    const int32 Width = ClimateTexture->Source.GetSizeX();
    const int32 Height = ClimateTexture->Source.GetSizeY();
    if (Width <= 0 || Height <= 0
        || LocalBiomeTexture->Source.GetSizeX() != Width
        || LocalBiomeTexture->Source.GetSizeY() != Height
        || !WorldBounds.bIsValid)
    {
        return false;
    }

    TArray64<uint8> ClimateRaw;
    TArray64<uint8> LocalRaw;
    if (!ClimateTexture->Source.GetMipData(ClimateRaw, 0, nullptr)
        || !LocalBiomeTexture->Source.GetMipData(LocalRaw, 0, nullptr)
        || ClimateRaw.Num() != static_cast<int64>(Width) * Height * sizeof(FColor)
        || LocalRaw.Num() != static_cast<int64>(Width) * Height * sizeof(FColor))
    {
        return false;
    }

    TArray<FColor> ClimatePixels;
    TArray<FColor> LocalPixels;
    ClimatePixels.SetNumUninitialized(Width * Height);
    LocalPixels.SetNumUninitialized(Width * Height);
    FMemory::Memcpy(ClimatePixels.GetData(), ClimateRaw.GetData(), ClimateRaw.Num());
    FMemory::Memcpy(LocalPixels.GetData(), LocalRaw.GetData(), LocalRaw.Num());

    const FVector2D Size = WorldBounds.GetSize();
    FAvenorTerrainHeightChunkCache TerrainCache;
    for (int32 Y = 0; Y < Height; ++Y)
    {
        for (int32 X = 0; X < Width; ++X)
        {
            const int32 Pixel = Y * Width + X;
            FColor& Climate = ClimatePixels[Pixel];
            FColor& Local = LocalPixels[Pixel];
            const FVector2D Position(
                WorldBounds.Min.X + (static_cast<double>(X) + 0.5) / Width * Size.X,
                WorldBounds.Min.Y + (static_cast<double>(Y) + 0.5) / Height * Size.Y
            );
            FAvenorTerrainSample Terrain;
            if (!TerrainData.SampleTerrain(Position, Terrain, TerrainCache))
            {
                continue;
            }

            const double RegionalTemperature = Climate.R / 255.0;
            const double RegionalMoisture = Climate.G / 255.0;
            const double ElevationMetres = FMath::Max(0.0, static_cast<double>(Terrain.Height)) / 100.0;
            const double ElevationFraction = FMath::Clamp(
                ElevationMetres / 2400.0, 0.0, 1.65
            );
            const double MountainExposure = FMath::Clamp(
                static_cast<double>(Terrain.Mountain), 0.0, 1.0
            );
            const double SlopeExposure = FMath::Clamp(
                static_cast<double>(Terrain.Slope) / 0.18, 0.0, 1.0
            );
            const double LocalTemperature = FMath::Clamp(
                RegionalTemperature
                    - ElevationFraction * 0.52
                    - MountainExposure * ElevationFraction * 0.07,
                0.0, 1.0
            );
            const double LocalMoisture = FMath::Clamp(
                RegionalMoisture
                    - ElevationFraction * 0.16
                    - MountainExposure * 0.055
                    - SlopeExposure * 0.055,
                0.0, 1.0
            );
            Climate.B = UnitToByte(LocalTemperature);
            Climate.A = UnitToByte(LocalMoisture);

            EAvenorBiomeClass Derived = ClassifyClimateBiome(
                LocalTemperature, LocalMoisture
            );
            const bool bRuggedHigh = ElevationMetres > 800.0
                && (Terrain.Slope > 0.025f || Terrain.Mountain > 0.28f);
            const bool bVeryHigh = ElevationMetres > 1450.0;
            const bool bAlpineCandidate = bVeryHigh
                || bRuggedHigh
                || (Terrain.Hill > 0.32f && ElevationMetres > 1050.0);
            if (LocalTemperature < 0.10
                && (bVeryHigh
                    || (Terrain.Mountain > 0.34f && ElevationMetres > 1200.0)))
            {
                Derived = EAvenorBiomeClass::SnowIce;
            }
            else if (LocalTemperature < 0.29 && bAlpineCandidate)
            {
                Derived = EAvenorBiomeClass::AlpineTundra;
            }

            const EAvenorBiomeClass RegionalBiome = ClassifyClimateBiome(
                RegionalTemperature, RegionalMoisture
            );
            const bool bExistingOverride = Local.A > 0;
            const EAvenorBiomeClass Existing = bExistingOverride
                ? ClosestBiomeColour(Local)
                : RegionalBiome;
            if (bExistingOverride && IsHydrologyOverride(Existing))
            {
                continue;
            }
            if (Derived != RegionalBiome)
            {
                Local = UAvenorTerrainData::GetBiomeColour(Derived);
                Local.A = 255;
            }
            else
            {
                Local = FColor(0, 0, 0, 0);
            }
        }
    }

    RefreshTexturePixels(*ClimateTexture, ClimatePixels);
    RefreshTexturePixels(*LocalBiomeTexture, LocalPixels);
    ClimateTexture->Filter = TF_Bilinear;
    LocalBiomeTexture->Filter = TF_Nearest;
    return true;
}

static UTexture2D* CreateOrUpdateBlendTexture(
    const FString& PackageName,
    const FString& AssetName,
    int32 Width,
    int32 Height,
    const TArray<FColor>& Pixels
)
{
    if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
    {
        return nullptr;
    }

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
    UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *ObjectPath);
    bool bCreated = false;
    if (!Texture)
    {
        UPackage* Package = CreatePackage(*PackageName);
        Texture = NewObject<UTexture2D>(
            Package,
            *AssetName,
            RF_Public | RF_Standalone | RF_Transactional
        );
        bCreated = Texture != nullptr;
    }
    if (!Texture)
    {
        return nullptr;
    }

    Texture->Modify();
    Texture->Source.Init(
        Width,
        Height,
        1,
        1,
        TSF_BGRA8,
        reinterpret_cast<const uint8*>(Pixels.GetData())
    );
    Texture->SRGB = false;
    Texture->CompressionSettings = TC_VectorDisplacementmap;
    Texture->MipGenSettings = TMGS_NoMipmaps;
    Texture->Filter = TF_Nearest;
    Texture->NeverStream = false;
    Texture->UpdateResource();
    Texture->MarkPackageDirty();
    if (bCreated)
    {
        FAssetRegistryModule::AssetCreated(Texture);
    }
    return Texture;
}

static bool BuildBlendPixels(
    UTexture2D& ClimateTexture,
    double CellSize,
    TArray<FColor>& OutPixels
)
{
    const int32 Width = ClimateTexture.Source.GetSizeX();
    const int32 Height = ClimateTexture.Source.GetSizeY();
    if (Width <= 0 || Height <= 0 || ClimateTexture.Source.GetFormat(0) != TSF_BGRA8)
    {
        return false;
    }

    TArray64<uint8> RawMip;
    if (!ClimateTexture.Source.GetMipData(RawMip, 0, nullptr)
        || RawMip.Num() != static_cast<int64>(Width) * Height * sizeof(FColor))
    {
        return false;
    }

    const FColor* ClimatePixels = reinterpret_cast<const FColor*>(RawMip.GetData());
    TArray<uint8> Biomes;
    Biomes.SetNumUninitialized(Width * Height);
    for (int32 Pixel = 0; Pixel < Biomes.Num(); ++Pixel)
    {
        Biomes[Pixel] = ClassifyBaseBiomeIndex(ClimatePixels[Pixel]);
    }

    const double RequestedWidth = FMath::Max(CellSize, static_cast<double>(CVarBlendWidthCm.GetValueOnGameThread()));
    const int32 RadiusCells = FMath::Max(1, FMath::CeilToInt(RequestedWidth / FMath::Max(1.0, CellSize)));
    const double RadiusSquared = static_cast<double>(RadiusCells * RadiusCells);

    OutPixels.SetNumUninitialized(Width * Height);
    for (int32 Y = 0; Y < Height; ++Y)
    {
        for (int32 X = 0; X < Width; ++X)
        {
            const int32 Pixel = Y * Width + X;
            const uint8 BiomeA = Biomes[Pixel];
            uint8 BiomeB = BiomeA;
            double BestDistanceSquared = RadiusSquared + 1.0;

            const int32 MinY = FMath::Max(0, Y - RadiusCells);
            const int32 MaxY = FMath::Min(Height - 1, Y + RadiusCells);
            const int32 MinX = FMath::Max(0, X - RadiusCells);
            const int32 MaxX = FMath::Min(Width - 1, X + RadiusCells);
            for (int32 OtherY = MinY; OtherY <= MaxY; ++OtherY)
            {
                for (int32 OtherX = MinX; OtherX <= MaxX; ++OtherX)
                {
                    const int32 OtherPixel = OtherY * Width + OtherX;
                    if (Biomes[OtherPixel] == BiomeA)
                    {
                        continue;
                    }
                    const double DX = OtherX - X;
                    const double DY = OtherY - Y;
                    const double DistanceSquared = DX * DX + DY * DY;
                    if (DistanceSquared < BestDistanceSquared)
                    {
                        BestDistanceSquared = DistanceSquared;
                        BiomeB = Biomes[OtherPixel];
                    }
                }
            }

            double Blend = 0.0;
            if (BiomeB != BiomeA && BestDistanceSquared <= RadiusSquared)
            {
                const double DistanceToBoundaryCells = FMath::Max(
                    0.0,
                    FMath::Sqrt(BestDistanceSquared) - 0.5
                );
                const double DistanceAlpha = FMath::Clamp(
                    DistanceToBoundaryCells / FMath::Max(0.5, static_cast<double>(RadiusCells)),
                    0.0,
                    1.0
                );
                const double Smoothed = DistanceAlpha * DistanceAlpha * (3.0 - 2.0 * DistanceAlpha);
                Blend = 0.5 * (1.0 - Smoothed);
            }

            OutPixels[Pixel] = FColor(
                UnitToByte(BiomeA / 7.0),
                UnitToByte(BiomeB / 7.0),
                UnitToByte(Blend),
                255
            );
        }
    }
    return true;
}

static void GenerateForTerrainData(UAvenorTerrainData& TerrainData)
{
    UEditorAssetSubsystem* AssetSubsystem = GEditor
        ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()
        : nullptr;
    if (!AssetSubsystem)
    {
        return;
    }

    // Final climate is derived from the finished eroded surface.  Regional
    // climate stays in R/G; altitude/exposure-corrected local climate is B/A.
    UTexture2D* ClimateTexture = TerrainData.WorldClimateMaps.ClimateFilterTexture.LoadSynchronous();
    UTexture2D* LocalBiomeTexture = TerrainData.WorldClimateMaps.LocalBiomeTexture.LoadSynchronous();
    if (ClimateTexture && LocalBiomeTexture)
    {
        RebuildLocalClimateMap(
            TerrainData,
            TerrainData.WorldClimateMaps.WorldBounds,
            ClimateTexture,
            LocalBiomeTexture
        );
        AssetSubsystem->SaveLoadedAsset(ClimateTexture, false);
        AssetSubsystem->SaveLoadedAsset(LocalBiomeTexture, false);
    }

    for (FAvenorClimateTileReference& Tile : TerrainData.ClimateTiles)
    {
        UTexture2D* TileClimate = Tile.ClimateFilterTexture.LoadSynchronous();
        UTexture2D* TileLocal = Tile.LocalBiomeTexture.LoadSynchronous();
        if (TileClimate && TileLocal
            && RebuildLocalClimateMap(
                TerrainData, Tile.WorldBounds, TileClimate, TileLocal))
        {
            AssetSubsystem->SaveLoadedAsset(TileClimate, false);
            AssetSubsystem->SaveLoadedAsset(TileLocal, false);
        }
    }

    ClimateTexture = TerrainData.WorldClimateMaps.ClimateFilterTexture.LoadSynchronous();
    if (!ClimateTexture || TerrainData.WorldClimateMaps.CellSize <= 0.0)
    {
        return;
    }

    TArray<FColor> BlendPixels;
    if (!BuildBlendPixels(*ClimateTexture, TerrainData.WorldClimateMaps.CellSize, BlendPixels))
    {
        UE_LOG(LogTemp, Warning, TEXT("Avenor biome blend map: could not read world climate source texture %s."), *ClimateTexture->GetPathName());
        return;
    }

    FString OwnerSuffix = ClimateTexture->GetName();
    OwnerSuffix.RemoveFromStart(TEXT("T_AvenorWorldClimate_"));
    const FString AssetName = FString::Printf(TEXT("T_AvenorWorldBiomeBlend_%s"), *OwnerSuffix);
    const FString Folder = TEXT("/Game/Avenor/Generated/Climate/World");
    UTexture2D* BlendTexture = CreateOrUpdateBlendTexture(
        FString::Printf(TEXT("%s/%s"), *Folder, *AssetName),
        AssetName,
        ClimateTexture->Source.GetSizeX(),
        ClimateTexture->Source.GetSizeY(),
        BlendPixels
    );
    if (!BlendTexture)
    {
        UE_LOG(LogTemp, Error, TEXT("Avenor biome blend map: failed to create %s."), *AssetName);
        return;
    }

    if (!AssetSubsystem->SaveLoadedAsset(BlendTexture, false))
    {
        UE_LOG(LogTemp, Error, TEXT("Avenor biome blend map: failed to save %s."), *BlendTexture->GetPathName());
        return;
    }

    UE_LOG(
        LogTemp,
        Display,
        TEXT("Avenor biome blend map generated from final local climate: %s | R=BiomeA/7, G=BiomeB/7, B=Blend | width %.0f cm"),
        *BlendTexture->GetPathName(),
        CVarBlendWidthCm.GetValueOnGameThread()
    );
}

static void OnObjectPreSave(UObject* Object, FObjectPreSaveContext SaveContext)
{
    (void)SaveContext;
    if (UAvenorTerrainData* TerrainData = Cast<UAvenorTerrainData>(Object))
    {
        GenerateForTerrainData(*TerrainData);
    }
}

struct FAutoRegisterBiomeBlendMap
{
    FDelegateHandle Handle;

    FAutoRegisterBiomeBlendMap()
    {
        Handle = FCoreUObjectDelegates::OnObjectPreSave.AddStatic(&OnObjectPreSave);
    }

    ~FAutoRegisterBiomeBlendMap()
    {
        if (Handle.IsValid())
        {
            FCoreUObjectDelegates::OnObjectPreSave.Remove(Handle);
        }
    }
};

static FAutoRegisterBiomeBlendMap GAutoRegisterBiomeBlendMap;
} // namespace UE::Avenor::BiomeBlend
