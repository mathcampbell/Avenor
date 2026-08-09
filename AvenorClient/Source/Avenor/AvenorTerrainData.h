#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "AvenorTerrainData.generated.h"

USTRUCT(BlueprintType)
struct AVENOR_API FAvenorTerrainDataChunk
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, Category = "Chunk")
    FIntPoint ChunkCoordinate = FIntPoint::ZeroValue;

    UPROPERTY(VisibleAnywhere, Category = "Chunk")
    FIntPoint StartCell = FIntPoint::ZeroValue;

    UPROPERTY(VisibleAnywhere, Category = "Chunk")
    FIntPoint CellCount = FIntPoint::ZeroValue;

    UPROPERTY(VisibleAnywhere, Category = "Chunk")
    int64 UncompressedBytes = 0;

    UPROPERTY()
    TArray<uint8> CompressedPayload;
};

USTRUCT(BlueprintType)
struct AVENOR_API FAvenorBakedRiverReach
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, Category = "River")
    TArray<FVector> Points;

    UPROPERTY(VisibleAnywhere, Category = "River")
    double Width = 500.0;

    UPROPERTY(VisibleAnywhere, Category = "River")
    double Depth = 250.0;

    UPROPERTY(VisibleAnywhere, Category = "River")
    double ValleyHalfWidth = 15000.0;

    UPROPERTY(VisibleAnywhere, Category = "River")
    double ValleyDepth = 1500.0;

    UPROPERTY(VisibleAnywhere, Category = "River")
    double CrossSectionExponent = 1.0;

    UPROPERTY(VisibleAnywhere, Category = "River")
    double ChannelSteepness = 2.2;

    UPROPERTY(VisibleAnywhere, Category = "River")
    double DrainageArea = 0.0;

    UPROPERTY(VisibleAnywhere, Category = "River")
    int32 StartLakeIndex = INDEX_NONE;

    UPROPERTY(VisibleAnywhere, Category = "River")
    int32 EndLakeIndex = INDEX_NONE;

    UPROPERTY(VisibleAnywhere, Category = "River")
    bool bIsCanyon = false;
};

USTRUCT(BlueprintType)
struct AVENOR_API FAvenorBakedLakeBasin
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, Category = "Lake")
    TArray<FVector> Shoreline;

    UPROPERTY(VisibleAnywhere, Category = "Lake")
    double ShorelineHeight = 0.0;

    UPROPERTY(VisibleAnywhere, Category = "Lake")
    double SurfaceHeight = 0.0;

    UPROPERTY(VisibleAnywhere, Category = "Lake")
    double MaximumDepth = 500.0;

    UPROPERTY(VisibleAnywhere, Category = "Lake")
    double BankBlendWidth = 24000.0;

    UPROPERTY(VisibleAnywhere, Category = "Lake")
    double DepthRampWidth = 7500.0;
};

/**
 * Durable, engine-independent source geography for an Avenor strip region.
 * Mesh Terrain geometry and Water Body actors are derived output from this
 * asset; procedural erosion and hydrology are run only by an explicit bake.
 */
UCLASS(BlueprintType)
class AVENOR_API UAvenorTerrainData : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    static constexpr int32 CurrentFormatVersion = 1;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Version")
    int32 FormatVersion = CurrentFormatVersion;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Version")
    int32 GeneratorAlgorithmVersion = 1;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Version")
    FString SettingsHash;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Version", meta = (MultiLine = "true"))
    FString GenerationSettingsSnapshot;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Version")
    FDateTime GeneratedAtUtc;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|World")
    int32 Seed = 0;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|World")
    FBox WorldBounds = FBox(ForceInit);

    UPROPERTY(VisibleAnywhere, Category = "Avenor|World")
    int32 Columns = 0;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|World")
    int32 Rows = 0;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|World")
    double CellSize = 0.0;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Chunks")
    int32 ChunkCellSize = 128;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Chunks")
    TArray<FAvenorTerrainDataChunk> Chunks;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Hydrology")
    TArray<FAvenorBakedRiverReach> Rivers;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Hydrology")
    TArray<FAvenorBakedLakeBasin> Lakes;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Hydrology")
    TArray<FVector> OceanBoundary;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Statistics")
    int32 RequestedMountainRanges = 0;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Statistics")
    int32 PlacedMountainRanges = 0;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Statistics")
    int32 AuthoritativeRiverCells = 0;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Statistics")
    int32 RiverSeedCells = 0;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Statistics")
    int32 RiverContinuationCells = 0;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Statistics")
    int32 RejectedShortRiverSystems = 0;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Statistics")
    int32 RiverTerminusLakeCandidates = 0;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Statistics")
    int32 AcceptedRiverTerminusLakes = 0;

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Statistics")
    int32 AcceptedOptionalLakes = 0;

    bool HasValidData() const;
};
