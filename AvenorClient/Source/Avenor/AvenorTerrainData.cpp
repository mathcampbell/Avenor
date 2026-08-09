#include "AvenorTerrainData.h"

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
