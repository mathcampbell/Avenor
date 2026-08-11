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

    /** Native Water Body bank blend width beyond the channel edge. */
    UPROPERTY(VisibleAnywhere, Category = "River")
    double BankWidth = 12000.0;

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

    /** Exact bed depth requested from the native Lake Modifier. */
    UPROPERTY(VisibleAnywhere, Category = "Lake")
    double ModifierBedDepth = 3000.0;

    UPROPERTY(VisibleAnywhere, Category = "Lake")
    double BankBlendWidth = 24000.0;

    UPROPERTY(VisibleAnywhere, Category = "Lake")
    double DepthRampWidth = 7500.0;
};

/** One world-space sample in the engineered Spine terrain layer. */
USTRUCT(BlueprintType)
struct AVENOR_API FAvenorBakedSpineSample
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, Category = "Spine")
    FVector2D Centre = FVector2D::ZeroVector;

    UPROPERTY(VisibleAnywhere, Category = "Spine")
    float Chainage = 0.0f;

    UPROPERTY(VisibleAnywhere, Category = "Spine")
    float NaturalTerrainZ = 0.0f;

    UPROPERTY(VisibleAnywhere, Category = "Spine")
    float RoadDatumZ = 0.0f;

    UPROPERTY(VisibleAnywhere, Category = "Spine")
    TArray<float> LeftDevelopmentProfileZ;

    UPROPERTY(VisibleAnywhere, Category = "Spine")
    TArray<float> RightDevelopmentProfileZ;

    UPROPERTY(VisibleAnywhere, Category = "Spine")
    bool bDevelopmentSuitable = true;
};

/** Independently replaceable engineered layer stored beside base geography. */
USTRUCT(BlueprintType)
struct AVENOR_API FAvenorBakedSpineLayer
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, Category = "Spine")
    FDateTime GeneratedAtUtc;

    UPROPERTY(VisibleAnywhere, Category = "Spine")
    float FlatHalfWidth = 0.0f;

    UPROPERTY(VisibleAnywhere, Category = "Spine")
    float DevelopmentHalfWidth = 0.0f;

    UPROPERTY(VisibleAnywhere, Category = "Spine")
    float TransitionHalfWidth = 0.0f;

    UPROPERTY(VisibleAnywhere, Category = "Spine")
    TArray<FAvenorBakedSpineSample> Samples;

    bool HasValidData() const;
};

/** Values required to build one Mesh Terrain vertex from baked geography. */
struct AVENOR_API FAvenorTerrainSample
{
    float Height = 0.0f;
    float Slope = 0.0f;
    float Accumulation = 0.0f;
    float Mountain = 0.0f;
    float Hill = 0.0f;
    float Desert = 0.0f;
    float Plains = 0.0f;
};

/** Expanded fields from one independently compressed terrain chunk. */
struct AVENOR_API FAvenorTerrainSampleChunk
{
    FIntPoint StartCell = FIntPoint::ZeroValue;
    FIntPoint CellCount = FIntPoint::ZeroValue;
    TArray<float> Height;
    TArray<float> Mountain;
    TArray<float> Hill;
    TArray<float> Desert;
    TArray<float> Plains;
    TArray<float> Accumulation;
    TArray<float> Slope;
};

/** Per-operation cache; only chunks actually sampled are expanded. */
struct AVENOR_API FAvenorTerrainHeightChunkCache
{
    TMap<FIntPoint, FAvenorTerrainSampleChunk> Chunks;
    TArray<FBox2D> RiverBounds;
    TArray<FBox2D> LakeBounds;
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

    UPROPERTY(VisibleAnywhere, Category = "Avenor|Layers|Spine")
    FAvenorBakedSpineLayer SpineLayer;

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

    bool SampleBaseHeight(
        const FVector2D& WorldPosition,
        float& OutHeight,
        FAvenorTerrainHeightChunkCache& Cache
    ) const;

    /** Samples the visible post-water ground used for development checks. */
    bool SampleFinalHeight(
        const FVector2D& WorldPosition,
        float& OutHeight,
        FAvenorTerrainHeightChunkCache& Cache,
        bool* bOutWaterAffected = nullptr
    ) const;

    /** Returns the water surface only when the point is over actual water. */
    bool SampleWaterSurface(
        const FVector2D& WorldPosition,
        float& OutSurfaceHeight
    ) const;

    bool SampleTerrain(
        const FVector2D& WorldPosition,
        FAvenorTerrainSample& OutSample,
        FAvenorTerrainHeightChunkCache& Cache
    ) const;

    float SampleRiverWeight(
        const FVector2D& WorldPosition,
        FAvenorTerrainHeightChunkCache& Cache
    ) const;
    float SampleLakeWeight(
        const FVector2D& WorldPosition,
        FAvenorTerrainHeightChunkCache& Cache
    ) const;

private:
    bool LoadSampleChunk(
        const FIntPoint& ChunkCoordinate,
        FAvenorTerrainSampleChunk& OutChunk
    ) const;
};
