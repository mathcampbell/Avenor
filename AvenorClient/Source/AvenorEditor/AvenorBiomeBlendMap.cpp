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

    // Final climate (B/A) and the local-override biome are already baked by
    // AAvenorStripTerrainGenerator's own BuildClimateTextureTiles, straight
    // from the authoritative Data.Temperature/Data.Moisture/Data.Biome -
    // the same values that already carry elevation lapse, rain shadow, and
    // hydrology-aware overrides (Wetland/Oasis/Riverbed/etc). This used to
    // also independently re-derive local temperature/moisture and Alpine/
    // Snow from raw elevation on every asset save (via OnObjectPreSave
    // below) and overwrite that already-correct bake with a second,
    // differently-tuned copy - not rain-shadow-aware, not matching the main
    // generator's thresholds, and silently clobbering the good data moments
    // after it was written. Removed; this function now only builds the
    // derived two-biome blend texture below, reading the bake as-is.
    UTexture2D* ClimateTexture = TerrainData.WorldClimateMaps.ClimateFilterTexture.LoadSynchronous();
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
