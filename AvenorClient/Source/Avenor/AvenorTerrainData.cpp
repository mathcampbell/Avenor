#include "AvenorTerrainData.h"

#include "Serialization/ArchiveLoadCompressedProxy.h"

namespace
{
constexpr uint32 TerrainChunkMagic = 0x41564431; // AVD1
constexpr int32 TerrainChunkPayloadVersion = 1;
}

bool FAvenorBakedSpineLayer::HasValidData() const
{
    if (Samples.Num() < 2
        || FlatHalfWidth < 0.0f
        || DevelopmentHalfWidth <= FlatHalfWidth
        || TransitionHalfWidth <= DevelopmentHalfWidth)
    {
        return false;
    }
    for (int32 Index = 0; Index < Samples.Num(); ++Index)
    {
        const FAvenorBakedSpineSample& Sample = Samples[Index];
        if ((Index > 0 && Sample.Chainage <= Samples[Index - 1].Chainage)
            || Sample.LeftDevelopmentProfileZ.Num() < 2
            || Sample.LeftDevelopmentProfileZ.Num()
                != Sample.RightDevelopmentProfileZ.Num())
        {
            return false;
        }
    }
    return true;
}

bool UAvenorTerrainData::HasValidData() const
{
    const int64 TotalCells = static_cast<int64>(Columns) * Rows;
    const int32 ExpectedChunkCount = ChunkCellSize > 0
        ? FMath::DivideAndRoundUp(Columns, ChunkCellSize)
            * FMath::DivideAndRoundUp(Rows, ChunkCellSize)
        : 0;
    return FormatVersion == CurrentFormatVersion
        && WorldBounds.IsValid
        && Columns > 1
        && Rows > 1
        && TotalCells <= MAX_int32
        && CellSize > 0.0
        && ChunkCellSize > 0
        && Chunks.Num() == ExpectedChunkCount;
}

bool UAvenorTerrainData::LoadHeightChunk(
    const FIntPoint& ChunkCoordinate,
    TArray<float>& OutHeights
) const
{
    const FAvenorTerrainDataChunk* Chunk = Chunks.FindByPredicate(
        [&ChunkCoordinate](const FAvenorTerrainDataChunk& Candidate)
        {
            return Candidate.ChunkCoordinate == ChunkCoordinate;
        }
    );
    if (!Chunk || Chunk->CompressedPayload.IsEmpty())
    {
        return false;
    }

    FArchiveLoadCompressedProxy Archive(Chunk->CompressedPayload, NAME_Zlib);
    uint32 Magic = 0;
    int32 Version = 0;
    Archive << Magic;
    Archive << Version;
    Archive << OutHeights;
    return !Archive.GetError()
        && Magic == TerrainChunkMagic
        && Version == TerrainChunkPayloadVersion
        && OutHeights.Num() == Chunk->CellCount.X * Chunk->CellCount.Y;
}

bool UAvenorTerrainData::SampleBaseHeight(
    const FVector2D& WorldPosition,
    float& OutHeight,
    FAvenorTerrainHeightChunkCache& Cache
) const
{
    if (!HasValidData()
        || WorldPosition.X < WorldBounds.Min.X
        || WorldPosition.X > WorldBounds.Max.X
        || WorldPosition.Y < WorldBounds.Min.Y
        || WorldPosition.Y > WorldBounds.Max.Y)
    {
        return false;
    }

    const double GridX = (WorldPosition.X - WorldBounds.Min.X) / CellSize - 0.5;
    const double GridY = (WorldPosition.Y - WorldBounds.Min.Y) / CellSize - 0.5;
    const int32 X0 = FMath::Clamp(FMath::FloorToInt(GridX), 0, Columns - 1);
    const int32 Y0 = FMath::Clamp(FMath::FloorToInt(GridY), 0, Rows - 1);
    const int32 X1 = FMath::Min(X0 + 1, Columns - 1);
    const int32 Y1 = FMath::Min(Y0 + 1, Rows - 1);

    auto ReadCell = [&](int32 X, int32 Y, float& Value)
    {
        const FIntPoint Coordinate(X / ChunkCellSize, Y / ChunkCellSize);
        TArray<float>* Heights = Cache.HeightChunks.Find(Coordinate);
        if (!Heights)
        {
            TArray<float> Loaded;
            if (!LoadHeightChunk(Coordinate, Loaded))
            {
                return false;
            }
            Heights = &Cache.HeightChunks.Add(Coordinate, MoveTemp(Loaded));
        }
        const FAvenorTerrainDataChunk* Chunk = Chunks.FindByPredicate(
            [&Coordinate](const FAvenorTerrainDataChunk& Candidate)
            {
                return Candidate.ChunkCoordinate == Coordinate;
            }
        );
        if (!Chunk)
        {
            return false;
        }
        const int32 LocalX = X - Chunk->StartCell.X;
        const int32 LocalY = Y - Chunk->StartCell.Y;
        const int32 LocalIndex = LocalY * Chunk->CellCount.X + LocalX;
        if (!Heights->IsValidIndex(LocalIndex))
        {
            return false;
        }
        Value = (*Heights)[LocalIndex];
        return true;
    };

    float H00 = 0.0f;
    float H10 = 0.0f;
    float H01 = 0.0f;
    float H11 = 0.0f;
    if (!ReadCell(X0, Y0, H00) || !ReadCell(X1, Y0, H10)
        || !ReadCell(X0, Y1, H01) || !ReadCell(X1, Y1, H11))
    {
        return false;
    }
    const float AlphaX = static_cast<float>(FMath::Clamp(GridX - X0, 0.0, 1.0));
    const float AlphaY = static_cast<float>(FMath::Clamp(GridY - Y0, 0.0, 1.0));
    OutHeight = FMath::Lerp(
        FMath::Lerp(H00, H10, AlphaX),
        FMath::Lerp(H01, H11, AlphaX),
        AlphaY
    );
    return true;
}
