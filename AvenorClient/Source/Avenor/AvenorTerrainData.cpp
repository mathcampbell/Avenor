#include "AvenorTerrainData.h"

#include "Serialization/ArchiveLoadCompressedProxy.h"

namespace
{
constexpr uint32 TerrainChunkMagic = 0x41564431; // AVD1
constexpr int32 TerrainChunkPayloadVersion = 3;

// Lake actors use explicit closed Catmull-Rom/Hermite tangents. Sample the
// same curve here so terrain carving, water tests and Spine bridge detection
// all use the visible shoreline rather than its straight control polygon.
static FVector2D EvaluateLakeBoundary(
    const TArray<FVector>& Shoreline,
    int32 SegmentIndex,
    double Alpha
)
{
    const int32 Count = Shoreline.Num();
    const int32 AIndex = SegmentIndex;
    const int32 BIndex = (SegmentIndex + 1) % Count;
    const FVector2D A(Shoreline[AIndex]);
    const FVector2D B(Shoreline[BIndex]);
    const FVector2D TangentA = (
        FVector2D(Shoreline[(AIndex + 1) % Count])
        - FVector2D(Shoreline[(AIndex - 1 + Count) % Count])
    ) * 0.5;
    const FVector2D TangentB = (
        FVector2D(Shoreline[(BIndex + 1) % Count])
        - FVector2D(Shoreline[(BIndex - 1 + Count) % Count])
    ) * 0.5;
    const double A2 = Alpha * Alpha;
    const double A3 = A2 * Alpha;
    return A * (2.0 * A3 - 3.0 * A2 + 1.0)
        + TangentA * (A3 - 2.0 * A2 + Alpha)
        + B * (-2.0 * A3 + 3.0 * A2)
        + TangentB * (A3 - A2);
}

static void QueryLakeBoundary(
    const TArray<FVector>& Shoreline,
    const FVector2D& Position,
    bool& bOutInside,
    double& OutDistance
)
{
    bOutInside = false;
    OutDistance = TNumericLimits<double>::Max();
    if (Shoreline.Num() < 3)
    {
        return;
    }
    constexpr int32 Subdivisions = 8;
    FVector2D Previous(Shoreline[0]);
    for (int32 SegmentIndex = 0; SegmentIndex < Shoreline.Num(); ++SegmentIndex)
    {
        for (int32 Step = 1; Step <= Subdivisions; ++Step)
        {
            const FVector2D Current = EvaluateLakeBoundary(
                Shoreline,
                SegmentIndex,
                static_cast<double>(Step) / Subdivisions
            );
            const FVector2D Segment = Current - Previous;
            const double LengthSquared = Segment.SizeSquared();
            const double Projection = LengthSquared > UE_DOUBLE_SMALL_NUMBER
                ? FMath::Clamp(FVector2D::DotProduct(
                    Position - Previous, Segment) / LengthSquared, 0.0, 1.0)
                : 0.0;
            OutDistance = FMath::Min(
                OutDistance,
                (Position - (Previous + Segment * Projection)).Size()
            );
            const bool bCrosses = (Previous.Y > Position.Y)
                != (Current.Y > Position.Y);
            if (bCrosses && Position.X < (Current.X - Previous.X)
                    * (Position.Y - Previous.Y)
                    / (Current.Y - Previous.Y) + Previous.X)
            {
                bOutInside = !bOutInside;
            }
            Previous = Current;
        }
    }
}
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

bool UAvenorTerrainData::LoadSampleChunk(
    const FIntPoint& ChunkCoordinate,
    FAvenorTerrainSampleChunk& OutChunk
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
    if (Archive.GetError() || Magic != TerrainChunkMagic
        || Version != TerrainChunkPayloadVersion)
    {
        return false;
    }

    TArray<float> DiscardFloat;
    TArray<int32> DiscardInt;
    Archive << OutChunk.Height;
    Archive << DiscardFloat; // Resistance
    Archive << OutChunk.Mountain;
    Archive << OutChunk.Hill;
    Archive << OutChunk.Desert;
    Archive << OutChunk.Plains;
    Archive << DiscardFloat; // FilledHeight
    Archive << OutChunk.Accumulation;
    Archive << OutChunk.Slope;
    Archive << OutChunk.Temperature;
    Archive << OutChunk.Moisture;
    Archive << OutChunk.Biome;
    Archive << DiscardInt;   // ReceiverA
    Archive << DiscardInt;   // ReceiverB
    Archive << DiscardFloat; // ReceiverWeightA
    Archive << DiscardInt;   // FillParent
    Archive << DiscardInt;   // LakeIndex

    OutChunk.StartCell = Chunk->StartCell;
    OutChunk.CellCount = Chunk->CellCount;
    const int32 Expected = Chunk->CellCount.X * Chunk->CellCount.Y;
    return !Archive.GetError()
        && OutChunk.Height.Num() == Expected
        && OutChunk.Mountain.Num() == Expected
        && OutChunk.Hill.Num() == Expected
        && OutChunk.Desert.Num() == Expected
        && OutChunk.Plains.Num() == Expected
        && OutChunk.Accumulation.Num() == Expected
        && OutChunk.Slope.Num() == Expected
        && OutChunk.Temperature.Num() == Expected
        && OutChunk.Moisture.Num() == Expected
        && OutChunk.Biome.Num() == Expected;
}

bool UAvenorTerrainData::SampleBaseHeight(
    const FVector2D& WorldPosition,
    float& OutHeight,
    FAvenorTerrainHeightChunkCache& Cache
) const
{
    FAvenorTerrainSample Sample;
    if (!SampleTerrain(WorldPosition, Sample, Cache))
    {
        return false;
    }
    OutHeight = Sample.Height;
    return true;
}

bool UAvenorTerrainData::SampleFinalHeight(
    const FVector2D& WorldPosition,
    float& OutHeight,
    FAvenorTerrainHeightChunkCache& Cache,
    bool* bOutWaterAffected
) const
{
    if (bOutWaterAffected)
    {
        *bOutWaterAffected = false;
    }
    if (!SampleBaseHeight(WorldPosition, OutHeight, Cache))
    {
        return false;
    }

    // Water Body modifiers carve after the broad terrain modifier. Mirror
    // their authoritative spline datum here so non-mesh consumers (notably
    // the Spine parcel checker) do not continue to see the pre-carve ground.
    for (const FAvenorBakedRiverReach& River : Rivers)
    {
        for (int32 Index = 0; Index + 1 < River.Points.Num(); ++Index)
        {
            const FVector& A3 = River.Points[Index];
            const FVector& B3 = River.Points[Index + 1];
            const FVector2D A(A3);
            const FVector2D B(B3);
            const FVector2D Segment = B - A;
            const double LengthSquared = Segment.SizeSquared();
            const double Alpha = LengthSquared > UE_DOUBLE_SMALL_NUMBER
                ? FMath::Clamp(FVector2D::DotProduct(WorldPosition - A, Segment)
                    / LengthSquared, 0.0, 1.0)
                : 0.0;
            const double Distance = (WorldPosition - (A + Segment * Alpha)).Size();
            const double CarveHalfWidth = FMath::Max(
                River.Width * 0.5 + 100.0,
                River.Width * 0.5 + River.BankWidth
            );
            if (Distance >= CarveHalfWidth)
            {
                continue;
            }
            if (bOutWaterAffected)
            {
                *bOutWaterAffected = true;
            }
            const double WaterZ = FMath::Lerp(A3.Z, B3.Z, Alpha);
            const double CrossAlpha = FMath::Clamp(
                Distance / CarveHalfWidth, 0.0, 1.0
            );
            const double BedZ = WaterZ - River.Depth;
            const float CarvedZ = static_cast<float>(FMath::Lerp(
                BedZ,
                static_cast<double>(OutHeight),
                FMath::SmoothStep(0.0, 1.0, CrossAlpha)
            ));
            OutHeight = FMath::Min(OutHeight, CarvedZ);
        }
    }

    // Lake modifiers are polygonal rather than spline based. Use the signed
    // distance to their shoreline so consumers see both the lake bed and the
    // exterior bank blend that the native Mesh Partition modifier creates.
    for (const FAvenorBakedLakeBasin& Lake : Lakes)
    {
        if (Lake.Shoreline.Num() < 3)
        {
            continue;
        }

        bool bInside = false;
        double DistanceToShore = 0.0;
        QueryLakeBoundary(
            Lake.Shoreline, WorldPosition, bInside, DistanceToShore
        );

        const double BankWidth = FMath::Max(0.0, Lake.BankBlendWidth);
        if (!bInside && DistanceToShore >= BankWidth)
        {
            continue;
        }
        if (bOutWaterAffected)
        {
            *bOutWaterAffected = true;
        }

        if (bInside)
        {
            const double RampWidth = FMath::Max(1.0, Lake.DepthRampWidth);
            const double DepthAlpha = FMath::SmoothStep(
                0.0, 1.0,
                FMath::Clamp(DistanceToShore / RampWidth, 0.0, 1.0)
            );
            const double BedDepth = FMath::Max(
                0.0,
                Lake.ModifierBedDepth > 0.0
                    ? Lake.ModifierBedDepth
                    : Lake.MaximumDepth
            );
            const float LakeGroundZ = static_cast<float>(
                Lake.SurfaceHeight - BedDepth * DepthAlpha
            );
            OutHeight = FMath::Min(OutHeight, LakeGroundZ);
        }
        else if (BankWidth > UE_DOUBLE_SMALL_NUMBER)
        {
            const double BankAlpha = FMath::SmoothStep(
                0.0, 1.0,
                FMath::Clamp(DistanceToShore / BankWidth, 0.0, 1.0)
            );
            const float BankZ = static_cast<float>(FMath::Lerp(
                Lake.SurfaceHeight,
                static_cast<double>(OutHeight),
                BankAlpha
            ));
            OutHeight = FMath::Min(OutHeight, BankZ);
        }
    }
    return true;
}

bool UAvenorTerrainData::SampleWaterSurface(
    const FVector2D& WorldPosition,
    float& OutSurfaceHeight
) const
{
    bool bFoundWater = false;
    OutSurfaceHeight = -TNumericLimits<float>::Max();

    for (const FAvenorBakedRiverReach& River : Rivers)
    {
        const double WaterHalfWidth = FMath::Max(0.0, River.Width * 0.5);
        for (int32 Index = 0; Index + 1 < River.Points.Num(); ++Index)
        {
            const FVector& A3 = River.Points[Index];
            const FVector& B3 = River.Points[Index + 1];
            const FVector2D A(A3);
            const FVector2D B(B3);
            const FVector2D Segment = B - A;
            const double LengthSquared = Segment.SizeSquared();
            const double Alpha = LengthSquared > UE_DOUBLE_SMALL_NUMBER
                ? FMath::Clamp(FVector2D::DotProduct(
                    WorldPosition - A, Segment) / LengthSquared, 0.0, 1.0)
                : 0.0;
            if ((WorldPosition - (A + Segment * Alpha)).Size()
                <= WaterHalfWidth)
            {
                OutSurfaceHeight = FMath::Max(
                    OutSurfaceHeight,
                    static_cast<float>(FMath::Lerp(A3.Z, B3.Z, Alpha))
                );
                bFoundWater = true;
            }
        }
    }

    for (const FAvenorBakedLakeBasin& Lake : Lakes)
    {
        if (Lake.Shoreline.Num() < 3)
        {
            continue;
        }
        bool bInside = false;
        double IgnoredDistance = 0.0;
        QueryLakeBoundary(
            Lake.Shoreline, WorldPosition, bInside, IgnoredDistance
        );
        if (bInside)
        {
            OutSurfaceHeight = FMath::Max(
                OutSurfaceHeight,
                static_cast<float>(Lake.SurfaceHeight)
            );
            bFoundWater = true;
        }
    }

    return bFoundWater;
}

bool UAvenorTerrainData::SampleTerrain(
    const FVector2D& WorldPosition,
    FAvenorTerrainSample& OutSample,
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

    auto ReadCell = [&](int32 X, int32 Y, FAvenorTerrainSample& Value)
    {
        const FIntPoint Coordinate(X / ChunkCellSize, Y / ChunkCellSize);
        FAvenorTerrainSampleChunk* SampleChunk = Cache.Chunks.Find(Coordinate);
        if (!SampleChunk)
        {
            FAvenorTerrainSampleChunk Loaded;
            if (!LoadSampleChunk(Coordinate, Loaded))
            {
                return false;
            }
            SampleChunk = &Cache.Chunks.Add(Coordinate, MoveTemp(Loaded));
        }
        const int32 LocalX = X - SampleChunk->StartCell.X;
        const int32 LocalY = Y - SampleChunk->StartCell.Y;
        const int32 LocalIndex = LocalY * SampleChunk->CellCount.X + LocalX;
        if (!SampleChunk->Height.IsValidIndex(LocalIndex))
        {
            return false;
        }
        Value.Height = SampleChunk->Height[LocalIndex];
        Value.Mountain = SampleChunk->Mountain[LocalIndex];
        Value.Hill = SampleChunk->Hill[LocalIndex];
        Value.Desert = SampleChunk->Desert[LocalIndex];
        Value.Plains = SampleChunk->Plains[LocalIndex];
        Value.Accumulation = SampleChunk->Accumulation[LocalIndex];
        Value.Slope = SampleChunk->Slope[LocalIndex];
        Value.Temperature = SampleChunk->Temperature[LocalIndex];
        Value.Moisture = SampleChunk->Moisture[LocalIndex];
        Value.Biome = static_cast<EAvenorBiomeClass>(
            SampleChunk->Biome[LocalIndex]
        );
        return true;
    };

    FAvenorTerrainSample S00;
    FAvenorTerrainSample S10;
    FAvenorTerrainSample S01;
    FAvenorTerrainSample S11;
    if (!ReadCell(X0, Y0, S00) || !ReadCell(X1, Y0, S10)
        || !ReadCell(X0, Y1, S01) || !ReadCell(X1, Y1, S11))
    {
        return false;
    }
    const float AlphaX = static_cast<float>(FMath::Clamp(GridX - X0, 0.0, 1.0));
    const float AlphaY = static_cast<float>(FMath::Clamp(GridY - Y0, 0.0, 1.0));
    auto Bilinear = [AlphaX, AlphaY](float V00, float V10, float V01, float V11)
    {
        return FMath::Lerp(
            FMath::Lerp(V00, V10, AlphaX),
            FMath::Lerp(V01, V11, AlphaX),
            AlphaY
        );
    };
    OutSample.Height = Bilinear(S00.Height, S10.Height, S01.Height, S11.Height);
    OutSample.Mountain = Bilinear(S00.Mountain, S10.Mountain, S01.Mountain, S11.Mountain);
    OutSample.Hill = Bilinear(S00.Hill, S10.Hill, S01.Hill, S11.Hill);
    OutSample.Desert = Bilinear(S00.Desert, S10.Desert, S01.Desert, S11.Desert);
    OutSample.Plains = Bilinear(S00.Plains, S10.Plains, S01.Plains, S11.Plains);
    OutSample.Accumulation = Bilinear(S00.Accumulation, S10.Accumulation, S01.Accumulation, S11.Accumulation);
    OutSample.Slope = Bilinear(S00.Slope, S10.Slope, S01.Slope, S11.Slope);
    OutSample.Temperature = Bilinear(
        S00.Temperature, S10.Temperature, S01.Temperature, S11.Temperature
    );
    OutSample.Moisture = Bilinear(
        S00.Moisture, S10.Moisture, S01.Moisture, S11.Moisture
    );
    const bool bRight = AlphaX >= 0.5f;
    const bool bTop = AlphaY >= 0.5f;
    OutSample.Biome = bTop
        ? (bRight ? S11.Biome : S01.Biome)
        : (bRight ? S10.Biome : S00.Biome);
    return true;
}

FColor UAvenorTerrainData::GetBiomeColour(EAvenorBiomeClass Biome)
{
    switch (Biome)
    {
    case EAvenorBiomeClass::ColdDry:         return FColor(126, 145, 151);
    case EAvenorBiomeClass::ColdMoist:       return FColor(35, 82, 67);
    case EAvenorBiomeClass::TemperateDry:    return FColor(174, 164, 83);
    case EAvenorBiomeClass::TemperateMoist:  return FColor(60, 122, 57);
    case EAvenorBiomeClass::WarmDry:         return FColor(176, 125, 68);
    case EAvenorBiomeClass::WarmMoist:       return FColor(39, 139, 73);
    case EAvenorBiomeClass::HotDry:          return FColor(219, 181, 92);
    case EAvenorBiomeClass::HotWet:          return FColor(18, 105, 58);
    case EAvenorBiomeClass::AlpineTundra:    return FColor(151, 157, 137);
    case EAvenorBiomeClass::SnowIce:         return FColor(232, 244, 248);
    case EAvenorBiomeClass::Wetland:         return FColor(52, 116, 113);
    case EAvenorBiomeClass::Oasis:           return FColor(42, 171, 151);
    default:                                 return FColor::Magenta;
    }
}

float UAvenorTerrainData::SampleRiverWeight(
    const FVector2D& WorldPosition,
    FAvenorTerrainHeightChunkCache& Cache
) const
{
    if (Cache.RiverBounds.Num() != Rivers.Num())
    {
        Cache.RiverBounds.Reset(Rivers.Num());
        for (const FAvenorBakedRiverReach& River : Rivers)
        {
            FBox2D Bounds(ForceInit);
            for (const FVector& Point : River.Points)
            {
                Bounds += FVector2D(Point);
            }
            Cache.RiverBounds.Add(Bounds.ExpandBy(River.ValleyHalfWidth));
        }
    }
    float Weight = 0.0f;
    for (int32 RiverIndex = 0; RiverIndex < Rivers.Num(); ++RiverIndex)
    {
        if (!Cache.RiverBounds[RiverIndex].IsInside(WorldPosition))
        {
            continue;
        }
        const FAvenorBakedRiverReach& River = Rivers[RiverIndex];
        for (int32 Index = 0; Index + 1 < River.Points.Num(); ++Index)
        {
            const FVector2D A(River.Points[Index]);
            const FVector2D B(River.Points[Index + 1]);
            const FVector2D Segment = B - A;
            const double LengthSquared = Segment.SizeSquared();
            const double Alpha = LengthSquared > UE_DOUBLE_SMALL_NUMBER
                ? FMath::Clamp(FVector2D::DotProduct(WorldPosition - A, Segment)
                    / LengthSquared, 0.0, 1.0)
                : 0.0;
            const double Distance = (WorldPosition - (A + Segment * Alpha)).Size();
            if (Distance <= River.ValleyHalfWidth)
            {
                Weight = FMath::Max(
                    Weight,
                    static_cast<float>(1.0 - Distance / River.ValleyHalfWidth)
                );
            }
        }
    }
    return Weight;
}

float UAvenorTerrainData::SampleLakeWeight(
    const FVector2D& WorldPosition,
    FAvenorTerrainHeightChunkCache& Cache
) const
{
    if (Cache.LakeBounds.Num() != Lakes.Num())
    {
        Cache.LakeBounds.Reset(Lakes.Num());
        for (const FAvenorBakedLakeBasin& Lake : Lakes)
        {
            FBox2D Bounds(ForceInit);
            for (const FVector& Point : Lake.Shoreline)
            {
                Bounds += FVector2D(Point);
            }
            Cache.LakeBounds.Add(Bounds);
        }
    }
    for (int32 LakeIndex = 0; LakeIndex < Lakes.Num(); ++LakeIndex)
    {
        if (!Cache.LakeBounds[LakeIndex].IsInside(WorldPosition))
        {
            continue;
        }
        const FAvenorBakedLakeBasin& Lake = Lakes[LakeIndex];
        bool bInside = false;
        for (int32 Index = 0, Previous = Lake.Shoreline.Num() - 1;
             Index < Lake.Shoreline.Num(); Previous = Index++)
        {
            const FVector& A = Lake.Shoreline[Index];
            const FVector& B = Lake.Shoreline[Previous];
            const bool bCrosses = (A.Y > WorldPosition.Y) != (B.Y > WorldPosition.Y);
            if (bCrosses && WorldPosition.X < (B.X - A.X)
                    * (WorldPosition.Y - A.Y) / (B.Y - A.Y) + A.X)
            {
                bInside = !bInside;
            }
        }
        if (bInside)
        {
            return 1.0f;
        }
    }
    return 0.0f;
}
