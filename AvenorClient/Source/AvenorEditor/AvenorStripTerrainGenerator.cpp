#include "AvenorStripTerrainGenerator.h"

#include "ActorFactories/ActorFactory.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "MeshPartition.h"
#include "MeshPartitionMeshView.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/DateTime.h"
#include "Misc/MessageDialog.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyOceanActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterSplineComponent.h"
#include "WaterSplineMetadata.h"

#include <queue>
#include <vector>

namespace UE::Avenor::Strip
{
static const FName GeneratedWaterTag(TEXT("AvenorStripWater"));
static const FName ElevationChannel(TEXT("Elevation"));
static const FName SlopeChannel(TEXT("Slope"));
static const FName WetnessChannel(TEXT("Wetness"));
static const FName RiverChannel(TEXT("River"));
static const FName LakeChannel(TEXT("Lake"));

static FName MakeWaterOwnerTag(const AAvenorStripTerrainGenerator& Generator)
{
    return FName(*FString::Printf(
        TEXT("AvenorStripOwner_%s"),
        *Generator.GetFName().ToString()
    ));
}

static bool IsWaterOwnerTag(const FName& Tag)
{
    return Tag.ToString().StartsWith(TEXT("AvenorStripOwner_"));
}

// ---------------------------------------------------------------------
// Numeric primitives. Deliberately small, deliberately boring - every one
// of these is easy to reason about in isolation, which is the point.
// ---------------------------------------------------------------------

static double Smooth01(double Value)
{
    const double T = FMath::Clamp(Value, 0.0, 1.0);
    return T * T * (3.0 - 2.0 * T);
}

static double Quintic(double Value)
{
    const double T = FMath::Clamp(Value, 0.0, 1.0);
    return T * T * T * (T * (T * 6.0 - 15.0) + 10.0);
}

static double Noise(
    const FVector2D& Position,
    double Scale,
    const FVector2D& Offset
)
{
    return FMath::PerlinNoise2D(
        (Position + Offset) / FMath::Max(1.0, Scale)
    );
}

static double Fbm(
    const FVector2D& Position,
    double Scale,
    const FVector2D& Offset,
    int32 Octaves,
    double Gain = 0.5,
    double Lacunarity = 2.0
)
{
    double Sum = 0.0;
    double Weight = 1.0;
    double WeightSum = 0.0;
    double FrequencyScale = FMath::Max(1.0, Scale);
    for (int32 Octave = 0; Octave < Octaves; ++Octave)
    {
        Sum += Noise(Position, FrequencyScale, Offset) * Weight;
        WeightSum += Weight;
        Weight *= Gain;
        FrequencyScale /= Lacunarity;
    }
    return WeightSum > 0.0 ? Sum / WeightSum : 0.0;
}

static double RidgedFbm(
    const FVector2D& Position,
    double Scale,
    const FVector2D& Offset,
    int32 Octaves
)
{
    double Sum = 0.0;
    double Weight = 1.0;
    double WeightSum = 0.0;
    double FrequencyScale = FMath::Max(1.0, Scale);
    for (int32 Octave = 0; Octave < Octaves; ++Octave)
    {
        double Ridge = 1.0 - FMath::Abs(Noise(Position, FrequencyScale, Offset));
        Ridge *= Ridge;
        Sum += Ridge * Weight;
        WeightSum += Weight;
        Weight *= 0.52;
        FrequencyScale /= 2.05;
    }
    return WeightSum > 0.0 ? Sum / WeightSum : 0.0;
}

static FVector2D Rotate90(const FVector2D& Vector)
{
    return FVector2D(-Vector.Y, Vector.X);
}

static double SegmentDistance(
    const FVector2D& Point,
    const FVector2D& A,
    const FVector2D& B,
    double* OutAlpha = nullptr
)
{
    const FVector2D Segment = B - A;
    const double LengthSquared = Segment.SizeSquared();
    const double Alpha = LengthSquared > UE_DOUBLE_KINDA_SMALL_NUMBER
        ? FMath::Clamp(
            FVector2D::DotProduct(Point - A, Segment) / LengthSquared,
            0.0,
            1.0
        )
        : 0.0;
    if (OutAlpha)
    {
        *OutAlpha = Alpha;
    }
    return FVector2D::Distance(Point, A + Segment * Alpha);
}

static bool IsInsidePolygon(
    const FVector2D& Point,
    const TArray<FVector>& Polygon,
    double* OutEdgeDistance = nullptr
)
{
    bool bInside = false;
    double EdgeDistance = TNumericLimits<double>::Max();
    for (int32 Index = 0; Index < Polygon.Num(); ++Index)
    {
        const FVector2D A(Polygon[Index]);
        const FVector2D B(Polygon[(Index + 1) % Polygon.Num()]);
        EdgeDistance = FMath::Min(EdgeDistance, SegmentDistance(Point, A, B));
        if ((A.Y > Point.Y) != (B.Y > Point.Y))
        {
            const double CrossingX = A.X +
                (Point.Y - A.Y) * (B.X - A.X) /
                    FMath::Max(UE_DOUBLE_KINDA_SMALL_NUMBER, FMath::Abs(B.Y - A.Y)) *
                    FMath::Sign(B.Y - A.Y);
            if (Point.X < CrossingX)
            {
                bInside = !bInside;
            }
        }
    }
    if (OutEdgeDistance)
    {
        *OutEdgeDistance = EdgeDistance;
    }
    return bInside;
}

static TArray<FVector> ChaikinSmooth(
    const TArray<FVector>& Input,
    bool bClosed,
    int32 Iterations
)
{
    TArray<FVector> Points = Input;
    for (int32 Iteration = 0;
         Iteration < Iterations && Points.Num() >= (bClosed ? 3 : 2);
         ++Iteration)
    {
        TArray<FVector> Result;
        if (!bClosed)
        {
            Result.Add(Points[0]);
        }
        const int32 SegmentCount = bClosed ? Points.Num() : Points.Num() - 1;
        Result.Reserve(SegmentCount * 2 + 2);
        for (int32 Index = 0; Index < SegmentCount; ++Index)
        {
            const FVector& A = Points[Index];
            const FVector& B = Points[(Index + 1) % Points.Num()];
            Result.Add(FMath::Lerp(A, B, 0.25));
            Result.Add(FMath::Lerp(A, B, 0.75));
        }
        if (!bClosed)
        {
            Result.Add(Points.Last());
        }
        Points = MoveTemp(Result);
    }
    return Points;
}

static TArray<FVector> ResamplePolyline(
    const TArray<FVector>& Input,
    double Spacing,
    bool bClosed
)
{
    if (Input.Num() < 2)
    {
        return Input;
    }
    const int32 SegmentCount = bClosed ? Input.Num() : Input.Num() - 1;
    TArray<double> SegmentLengths;
    SegmentLengths.SetNum(SegmentCount);
    double TotalLength = 0.0;
    for (int32 Index = 0; Index < SegmentCount; ++Index)
    {
        SegmentLengths[Index] = FVector2D::Distance(
            FVector2D(Input[Index]),
            FVector2D(Input[(Index + 1) % Input.Num()])
        );
        TotalLength += SegmentLengths[Index];
    }
    if (TotalLength <= UE_DOUBLE_KINDA_SMALL_NUMBER)
    {
        return Input;
    }
    const int32 SampleCount = FMath::Max(
        bClosed ? 3 : 2,
        FMath::CeilToInt(TotalLength / FMath::Max(1.0, Spacing)) + (bClosed ? 0 : 1)
    );
    TArray<FVector> Result;
    Result.Reserve(SampleCount);
    int32 SegmentIndex = 0;
    double SegmentStart = 0.0;
    for (int32 Sample = 0; Sample < SampleCount; ++Sample)
    {
        const double Distance = bClosed
            ? TotalLength * static_cast<double>(Sample) / SampleCount
            : TotalLength * static_cast<double>(Sample) / FMath::Max(1, SampleCount - 1);
        while (SegmentIndex + 1 < SegmentCount &&
               SegmentStart + SegmentLengths[SegmentIndex] < Distance)
        {
            SegmentStart += SegmentLengths[SegmentIndex];
            ++SegmentIndex;
        }
        const double Alpha = FMath::Clamp(
            (Distance - SegmentStart) / FMath::Max(1.0, SegmentLengths[SegmentIndex]),
            0.0,
            1.0
        );
        Result.Add(FMath::Lerp(
            Input[SegmentIndex],
            Input[(SegmentIndex + 1) % Input.Num()],
            Alpha
        ));
    }
    return Result;
}

static void AddBroadMeanders(
    TArray<FVector>& Points,
    double CellSize,
    double Strength,
    double LowlandFraction,
    int32 Seed,
    const FBox& Bounds
)
{
    if (Points.Num() < 4 || Strength <= 0.0)
    {
        return;
    }
    TArray<double> Lengths;
    Lengths.SetNum(Points.Num());
    Lengths[0] = 0.0;
    for (int32 Index = 1; Index < Points.Num(); ++Index)
    {
        Lengths[Index] = Lengths[Index - 1] + FVector2D::Distance(
            FVector2D(Points[Index - 1]), FVector2D(Points[Index])
        );
    }
    const double TotalLength = Lengths.Last();
    if (TotalLength < CellSize * 4.0)
    {
        return;
    }
    FRandomStream Random(Seed);
    const double Amplitude = FMath::Min(TotalLength * 0.075, CellSize * 3.8) *
        Strength * FMath::Lerp(0.12, 1.0, LowlandFraction);
    const double Wavelength = Random.FRandRange(10.0, 22.0) * CellSize;
    const double PhaseA = Random.FRandRange(-PI, PI);
    const double PhaseB = Random.FRandRange(-PI, PI);
    for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
    {
        const double Distance = Lengths[Index];
        const double Alpha = Distance / TotalLength;
        const FVector2D Tangent = FVector2D(
            Points[Index + 1].X - Points[Index - 1].X,
            Points[Index + 1].Y - Points[Index - 1].Y
        ).GetSafeNormal();
        const FVector2D Normal = Rotate90(Tangent);
        const double Fade = FMath::Sin(PI * Alpha);
        const double Wave =
            0.72 * FMath::Sin(2.0 * PI * Distance / Wavelength + PhaseA) +
            0.28 * FMath::Sin(2.0 * PI * Distance / (Wavelength * 2.37) + PhaseB);
        const FVector2D Offset = Normal * Amplitude * Fade * Wave;
        Points[Index].X = FMath::Clamp(Points[Index].X + Offset.X, Bounds.Min.X + CellSize, Bounds.Max.X - CellSize);
        Points[Index].Y = FMath::Clamp(Points[Index].Y + Offset.Y, Bounds.Min.Y + CellSize, Bounds.Max.Y - CellSize);
    }
}

// ---------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------

struct FRiverReach
{
    TArray<FVector> Points;
    double Width = 500.0;
    double Depth = 250.0;
    double ValleyHalfWidth = 15000.0;
    double ValleyDepth = 1500.0;
    double CrossSectionExponent = 1.0;
    double DrainageArea = 0.0;
    FBox2D Bounds = FBox2D(ForceInit);
};

struct FLakeBasin
{
    TArray<FVector> Shoreline;
    double ShorelineHeight = 0.0;
    double SurfaceHeight = 0.0;
    double MaximumDepth = 500.0;
    double BankBlendWidth = 24000.0;
    double DepthRampWidth = 7500.0;
    FBox2D Bounds = FBox2D(ForceInit);
};

struct FPriorityCell
{
    double Height = 0.0;
    int32 Cell = INDEX_NONE;
    bool operator>(const FPriorityCell& Other) const { return Height > Other.Height; }
};

} // namespace UE::Avenor::Strip

struct FAvenorStripData
{
    FBox Bounds = FBox(ForceInit);
    int32 Columns = 0;
    int32 Rows = 0;
    double CellSize = 10000.0;
    TArray<double> Height;
    TArray<double> FilledHeight;
    TArray<double> Accumulation;
    TArray<double> Slope;
    TArray<int32> ReceiverA;
    TArray<int32> ReceiverB;
    TArray<double> ReceiverWeightA;
    TArray<int32> FillParent;
    TArray<int32> LakeIndex;
    TArray<UE::Avenor::Strip::FRiverReach> Rivers;
    TArray<UE::Avenor::Strip::FLakeBasin> Lakes;
    TArray<FVector> OceanBoundary;

    int32 Index(int32 X, int32 Y) const { return Y * Columns + X; }
    bool IsValid(int32 X, int32 Y) const { return X >= 0 && X < Columns && Y >= 0 && Y < Rows; }
    bool IsBoundary(int32 X, int32 Y) const { return X == 0 || Y == 0 || X == Columns - 1 || Y == Rows - 1; }

    FVector2D CellPosition(int32 Cell) const
    {
        const int32 X = Cell % Columns;
        const int32 Y = Cell / Columns;
        return FVector2D(
            Bounds.Min.X + (static_cast<double>(X) + 0.5) * CellSize,
            Bounds.Min.Y + (static_cast<double>(Y) + 0.5) * CellSize
        );
    }

    double CellAreaSquareKilometres() const { return CellSize * CellSize / 10000000000.0; }

    double SampleGrid(const TArray<double>& Values, const FVector2D& Position) const
    {
        if (Values.Num() != Columns * Rows || Columns < 2 || Rows < 2)
        {
            return 0.0;
        }
        const double GX = (Position.X - Bounds.Min.X) / CellSize - 0.5;
        const double GY = (Position.Y - Bounds.Min.Y) / CellSize - 0.5;
        const int32 X0 = FMath::Clamp(FMath::FloorToInt(GX), 0, Columns - 1);
        const int32 Y0 = FMath::Clamp(FMath::FloorToInt(GY), 0, Rows - 1);
        const int32 X1 = FMath::Min(X0 + 1, Columns - 1);
        const int32 Y1 = FMath::Min(Y0 + 1, Rows - 1);
        const double AX = FMath::Clamp(GX - X0, 0.0, 1.0);
        const double AY = FMath::Clamp(GY - Y0, 0.0, 1.0);
        return FMath::Lerp(
            FMath::Lerp(Values[Index(X0, Y0)], Values[Index(X1, Y0)], AX),
            FMath::Lerp(Values[Index(X0, Y1)], Values[Index(X1, Y1)], AX),
            AY
        );
    }

    double SampleHeight(const FVector2D& Position) const;
    float SampleChannel(FName Channel, const FVector2D& Position) const;
};

namespace UE::Avenor::Strip
{
static constexpr int32 NeighborX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
static constexpr int32 NeighborY[8] = {0, 1, 1, 1, 0, -1, -1, -1};

// ---------------------------------------------------------------------
// Erosion. Thermal relaxation for scree/talus slopes, then stream-power
// incision that actually carves valleys following the real drainage
// network - this is what makes valleys, canyons and the river tree
// geomorphologically consistent with each other, rather than three
// independent, disconnected systems.
// ---------------------------------------------------------------------

static void ApplyThermalErosion(
    FAvenorStripData& Data,
    int32 Iterations,
    double Strength,
    double TalusAngleDegrees
)
{
    if (Iterations <= 0 || Strength <= 0.0)
    {
        return;
    }
    const double TalusDrop = FMath::Tan(FMath::DegreesToRadians(TalusAngleDegrees)) * Data.CellSize;
    TArray<double> Delta;
    Delta.SetNumZeroed(Data.Height.Num());
    for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
    {
        FMemory::Memzero(Delta.GetData(), Delta.Num() * sizeof(double));
        for (int32 Y = 1; Y + 1 < Data.Rows; ++Y)
        {
            for (int32 X = 1; X + 1 < Data.Columns; ++X)
            {
                const int32 Cell = Data.Index(X, Y);
                double TotalExcess = 0.0;
                double Excess[8] = {};
                for (int32 Direction = 0; Direction < 8; ++Direction)
                {
                    const int32 Neighbor = Data.Index(X + NeighborX[Direction], Y + NeighborY[Direction]);
                    const double Distance = (NeighborX[Direction] != 0 && NeighborY[Direction] != 0)
                        ? Data.CellSize * 1.4142135623730951 : Data.CellSize;
                    const double AllowedDrop = TalusDrop * Distance / Data.CellSize;
                    Excess[Direction] = FMath::Max(0.0, Data.Height[Cell] - Data.Height[Neighbor] - AllowedDrop);
                    TotalExcess += Excess[Direction];
                }
                if (TotalExcess <= 0.0)
                {
                    continue;
                }
                const double Transfer = FMath::Min(TotalExcess * Strength * 0.18, Data.CellSize * 0.25);
                Delta[Cell] -= Transfer;
                for (int32 Direction = 0; Direction < 8; ++Direction)
                {
                    if (Excess[Direction] <= 0.0)
                    {
                        continue;
                    }
                    const int32 Neighbor = Data.Index(X + NeighborX[Direction], Y + NeighborY[Direction]);
                    Delta[Neighbor] += Transfer * Excess[Direction] / TotalExcess;
                }
            }
        }
        for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
        {
            Data.Height[Cell] += Delta[Cell];
        }
    }
}

static void PriorityFlood(FAvenorStripData& Data, double Epsilon)
{
    const int32 CellCount = Data.Height.Num();
    Data.FilledHeight = Data.Height;
    Data.FillParent.Init(INDEX_NONE, CellCount);
    TArray<bool> Visited;
    Visited.Init(false, CellCount);
    std::priority_queue<FPriorityCell, std::vector<FPriorityCell>, std::greater<FPriorityCell>> Queue;
    auto AddBoundary = [&](int32 X, int32 Y)
    {
        const int32 Cell = Data.Index(X, Y);
        if (!Visited[Cell])
        {
            Visited[Cell] = true;
            Queue.push({Data.FilledHeight[Cell], Cell});
        }
    };
    for (int32 X = 0; X < Data.Columns; ++X)
    {
        AddBoundary(X, 0);
        AddBoundary(X, Data.Rows - 1);
    }
    for (int32 Y = 1; Y + 1 < Data.Rows; ++Y)
    {
        AddBoundary(0, Y);
        AddBoundary(Data.Columns - 1, Y);
    }
    while (!Queue.empty())
    {
        const FPriorityCell Current = Queue.top();
        Queue.pop();
        const int32 X = Current.Cell % Data.Columns;
        const int32 Y = Current.Cell / Data.Columns;
        for (int32 Direction = 0; Direction < 8; ++Direction)
        {
            const int32 NX = X + NeighborX[Direction];
            const int32 NY = Y + NeighborY[Direction];
            if (!Data.IsValid(NX, NY))
            {
                continue;
            }
            const int32 Neighbor = Data.Index(NX, NY);
            if (Visited[Neighbor])
            {
                continue;
            }
            Visited[Neighbor] = true;
            Data.FillParent[Neighbor] = Current.Cell;
            Data.FilledHeight[Neighbor] = FMath::Max(Data.Height[Neighbor], Current.Height + FMath::Max(0.0001, Epsilon));
            Queue.push({Data.FilledHeight[Neighbor], Neighbor});
        }
    }
}

static int32 SteepestReceiver(const FAvenorStripData& Data, int32 X, int32 Y)
{
    const int32 Cell = Data.Index(X, Y);
    double BestSlope = 0.0;
    int32 Best = INDEX_NONE;
    for (int32 Direction = 0; Direction < 8; ++Direction)
    {
        const int32 NX = X + NeighborX[Direction];
        const int32 NY = Y + NeighborY[Direction];
        if (!Data.IsValid(NX, NY))
        {
            continue;
        }
        const int32 Neighbor = Data.Index(NX, NY);
        const double Distance = (NeighborX[Direction] != 0 && NeighborY[Direction] != 0)
            ? Data.CellSize * 1.4142135623730951 : Data.CellSize;
        const double CandidateSlope = (Data.FilledHeight[Cell] - Data.FilledHeight[Neighbor]) / Distance;
        if (CandidateSlope > BestSlope)
        {
            BestSlope = CandidateSlope;
            Best = Neighbor;
        }
    }
    return Best != INDEX_NONE ? Best : Data.FillParent[Cell];
}

static void BuildContinuousFlow(FAvenorStripData& Data)
{
    const int32 CellCount = Data.Height.Num();
    Data.ReceiverA.Init(INDEX_NONE, CellCount);
    Data.ReceiverB.Init(INDEX_NONE, CellCount);
    Data.ReceiverWeightA.Init(1.0, CellCount);
    Data.Slope.Init(0.0, CellCount);
    Data.Accumulation.Init(Data.CellAreaSquareKilometres(), CellCount);

    for (int32 Y = 0; Y < Data.Rows; ++Y)
    {
        for (int32 X = 0; X < Data.Columns; ++X)
        {
            const int32 Cell = Data.Index(X, Y);
            if (Data.IsBoundary(X, Y))
            {
                continue;
            }
            const double DX = (Data.FilledHeight[Data.Index(X + 1, Y)] - Data.FilledHeight[Data.Index(X - 1, Y)]) / (2.0 * Data.CellSize);
            const double DY = (Data.FilledHeight[Data.Index(X, Y + 1)] - Data.FilledHeight[Data.Index(X, Y - 1)]) / (2.0 * Data.CellSize);
            Data.Slope[Cell] = FMath::Sqrt(DX * DX + DY * DY);
            double Angle = FMath::Atan2(-DY, -DX);
            if (Angle < 0.0)
            {
                Angle += 2.0 * PI;
            }
            const double Sector = Angle / (PI * 0.25);
            const int32 LowerDirection = FMath::FloorToInt(Sector) & 7;
            const int32 UpperDirection = (LowerDirection + 1) & 7;
            const double UpperWeight = Sector - static_cast<double>(FMath::FloorToInt(Sector));
            const int32 AX = X + NeighborX[LowerDirection];
            const int32 AY = Y + NeighborY[LowerDirection];
            const int32 BX = X + NeighborX[UpperDirection];
            const int32 BY = Y + NeighborY[UpperDirection];
            int32 A = Data.IsValid(AX, AY) ? Data.Index(AX, AY) : INDEX_NONE;
            int32 B = Data.IsValid(BX, BY) ? Data.Index(BX, BY) : INDEX_NONE;
            if (A != INDEX_NONE && Data.FilledHeight[A] >= Data.FilledHeight[Cell])
            {
                A = INDEX_NONE;
            }
            if (B != INDEX_NONE && Data.FilledHeight[B] >= Data.FilledHeight[Cell])
            {
                B = INDEX_NONE;
            }
            if (A == INDEX_NONE && B == INDEX_NONE)
            {
                A = SteepestReceiver(Data, X, Y);
                B = INDEX_NONE;
            }
            else if (A == INDEX_NONE)
            {
                A = B;
                B = INDEX_NONE;
            }
            Data.ReceiverA[Cell] = A;
            Data.ReceiverB[Cell] = B;
            Data.ReceiverWeightA[Cell] = B == INDEX_NONE ? 1.0 : 1.0 - UpperWeight;
        }
    }

    TArray<int32> Order;
    Order.SetNumUninitialized(CellCount);
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        Order[Cell] = Cell;
    }
    Order.Sort([&](int32 A, int32 B) { return Data.FilledHeight[A] > Data.FilledHeight[B]; });
    for (int32 Cell : Order)
    {
        const int32 A = Data.ReceiverA[Cell];
        const int32 B = Data.ReceiverB[Cell];
        const double WeightA = Data.ReceiverWeightA[Cell];
        if (A != INDEX_NONE)
        {
            Data.Accumulation[A] += Data.Accumulation[Cell] * WeightA;
        }
        if (B != INDEX_NONE)
        {
            Data.Accumulation[B] += Data.Accumulation[Cell] * (1.0 - WeightA);
        }
    }
}

static void ApplyStreamPowerErosion(
    FAvenorStripData& Data,
    int32 Iterations,
    double Strength,
    double StreamStartArea,
    double Epsilon
)
{
    for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
    {
        PriorityFlood(Data, Epsilon);
        BuildContinuousFlow(Data);
        TArray<double> Delta;
        Delta.Init(0.0, Data.Height.Num());
        for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
        {
            if (Data.Accumulation[Cell] < StreamStartArea || Data.ReceiverA[Cell] == INDEX_NONE)
            {
                continue;
            }
            const double AreaFactor = FMath::Pow(Data.Accumulation[Cell] / FMath::Max(0.01, StreamStartArea), 0.42);
            const double SlopeFactor = FMath::Pow(FMath::Max(0.00001, Data.Slope[Cell]), 0.72);
            // Cap is generous (200m/iteration at default cell size) so it
            // rarely binds - the strength/area/slope formula itself, not
            // an artificial ceiling, is what should decide how much a
            // given stream actually carves.
            Delta[Cell] = FMath::Min(Data.CellSize * 2.0, Strength * 760.0 * AreaFactor * SlopeFactor);
        }
        for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
        {
            Data.Height[Cell] -= Delta[Cell];
        }
    }
    PriorityFlood(Data, Epsilon);
    BuildContinuousFlow(Data);
}

static int32 PrimaryReceiver(const FAvenorStripData& Data, int32 Cell)
{
    if (Cell == INDEX_NONE)
    {
        return INDEX_NONE;
    }
    const int32 A = Data.ReceiverA[Cell];
    const int32 B = Data.ReceiverB[Cell];
    if (B == INDEX_NONE || Data.ReceiverWeightA[Cell] >= 0.5)
    {
        return A;
    }
    return B;
}

static double DrainageScaleAlpha(double Area, double MainRiverArea)
{
    if (Area >= MainRiverArea)
    {
        return 1.0;
    }
    return Smooth01(FMath::Loge(1.0 + FMath::Max(0.0, Area)) / FMath::Loge(1.0 + FMath::Max(0.01, MainRiverArea)));
}
} // namespace UE::Avenor::Strip

namespace UE::Avenor::Strip
{
// ---------------------------------------------------------------------
// Lake boundary tracing: raster depression -> marching-squares contour
// placed at the true height-crossing point -> simplify by arc length ->
// Chaikin smooth -> resample. Every stage after the first works in
// world-space distance, never a fixed point count, so it behaves the
// same way on a tiny pond and a kilometre-scale lake.
// ---------------------------------------------------------------------

struct FLakeBoundaryEdge
{
    FIntPoint Start;
    FIntPoint End;
    FVector2D Position = FVector2D::ZeroVector;
    bool bUsed = false;
};

static TArray<FVector> TraceComponentBoundary(
    const FAvenorStripData& Data,
    const TArray<int32>& Cells,
    double SurfaceHeight
)
{
    TSet<int32> Membership;
    Membership.Reserve(Cells.Num());
    for (int32 Cell : Cells)
    {
        Membership.Add(Cell);
    }
    TArray<FLakeBoundaryEdge> Edges;
    auto AddEdge = [&Data, SurfaceHeight, &Edges](
        int32 Cell, int32 OutsideX, int32 OutsideY,
        const FIntPoint& Start, const FIntPoint& End)
    {
        FVector2D Position;
        if (Data.IsValid(OutsideX, OutsideY))
        {
            const int32 Outside = Data.Index(OutsideX, OutsideY);
            const double InsideHeight = Data.Height[Cell];
            const double OutsideHeight = Data.Height[Outside];
            const double Denominator = OutsideHeight - InsideHeight;
            const double Alpha = FMath::Abs(Denominator) > 1.0
                ? FMath::Clamp((SurfaceHeight - InsideHeight) / Denominator, 0.05, 0.95)
                : 0.5;
            Position = FMath::Lerp(Data.CellPosition(Cell), Data.CellPosition(Outside), Alpha);
        }
        else
        {
            Position = FVector2D(
                Data.Bounds.Min.X + (Start.X + End.X) * 0.5 * Data.CellSize,
                Data.Bounds.Min.Y + (Start.Y + End.Y) * 0.5 * Data.CellSize
            );
        }
        Edges.Add({Start, End, Position, false});
    };
    for (int32 Cell : Cells)
    {
        const int32 X = Cell % Data.Columns;
        const int32 Y = Cell / Data.Columns;
        if (Y == 0 || !Membership.Contains(Data.Index(X, Y - 1)))
        {
            AddEdge(Cell, X, Y - 1, FIntPoint(X, Y), FIntPoint(X + 1, Y));
        }
        if (X + 1 >= Data.Columns || !Membership.Contains(Data.Index(X + 1, Y)))
        {
            AddEdge(Cell, X + 1, Y, FIntPoint(X + 1, Y), FIntPoint(X + 1, Y + 1));
        }
        if (Y + 1 >= Data.Rows || !Membership.Contains(Data.Index(X, Y + 1)))
        {
            AddEdge(Cell, X, Y + 1, FIntPoint(X + 1, Y + 1), FIntPoint(X, Y + 1));
        }
        if (X == 0 || !Membership.Contains(Data.Index(X - 1, Y)))
        {
            AddEdge(Cell, X - 1, Y, FIntPoint(X, Y + 1), FIntPoint(X, Y));
        }
    }
    if (Edges.IsEmpty())
    {
        return {};
    }
    TArray<FVector> Boundary;
    for (int32 StartIndex = 0; StartIndex < Edges.Num(); ++StartIndex)
    {
        if (Edges[StartIndex].bUsed)
        {
            continue;
        }
        TArray<FVector> Loop;
        int32 EdgeIndex = StartIndex;
        const FIntPoint First = Edges[StartIndex].Start;
        for (int32 Guard = 0; Guard <= Edges.Num(); ++Guard)
        {
            FLakeBoundaryEdge& Edge = Edges[EdgeIndex];
            if (Edge.bUsed)
            {
                break;
            }
            Edge.bUsed = true;
            Loop.Emplace(Edge.Position.X, Edge.Position.Y, SurfaceHeight);
            if (Edge.End == First)
            {
                break;
            }
            EdgeIndex = INDEX_NONE;
            for (int32 CandidateIndex = 0; CandidateIndex < Edges.Num(); ++CandidateIndex)
            {
                if (!Edges[CandidateIndex].bUsed && Edges[CandidateIndex].Start == Edge.End)
                {
                    EdgeIndex = CandidateIndex;
                    break;
                }
            }
            if (EdgeIndex == INDEX_NONE)
            {
                Loop.Reset();
                break;
            }
        }
        if (Loop.Num() > Boundary.Num())
        {
            Boundary = MoveTemp(Loop);
        }
    }
    if (Boundary.Num() < 4)
    {
        return {};
    }
    TArray<FVector> Reduced = ResamplePolyline(Boundary, FMath::Max(Data.CellSize, 3500.0), true);
    Boundary = ChaikinSmooth(Reduced, true, 2);
    return ResamplePolyline(Boundary, FMath::Max(Data.CellSize * 0.40, 2500.0), true);
}

struct FLakeCandidate
{
    TArray<int32> Cells;
    double SurfaceHeight = -TNumericLimits<double>::Max();
    double MaximumDepth = 0.0;
    double CatchmentArea = 0.0;
    double MinX = TNumericLimits<double>::Max();
    double MaxX = -TNumericLimits<double>::Max();
    double MinY = TNumericLimits<double>::Max();
    double MaxY = -TNumericLimits<double>::Max();
    bool bMandatory = false;
    int32 RimSpillCell = INDEX_NONE;
    double RimSpillHeight = TNumericLimits<double>::Max();
};

// A real, surviving drainage system dead-ending in the interior (not at
// the map boundary, which is a legitimate "flows off the world" case).
// Runs before any lake exists, so Data.LakeIndex is still empty here.
static TArray<int32> FindDrainageTerminusSeeds(
    const FAvenorStripData& Data,
    double MountainStartArea,
    double LowlandStartArea,
    double MinimumSystemLength
)
{
    TArray<bool> Channel;
    Channel.Init(false, Data.Height.Num());
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        const double MountainFraction = FMath::Clamp(Data.Slope[Cell] / 0.18, 0.0, 1.0);
        const double StartArea = FMath::Lerp(LowlandStartArea, MountainStartArea, MountainFraction);
        Channel[Cell] = Data.Accumulation[Cell] >= StartArea && PrimaryReceiver(Data, Cell) != INDEX_NONE;
    }
    TArray<int32> SystemParent;
    SystemParent.Init(INDEX_NONE, Data.Height.Num());
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (Channel[Cell])
        {
            SystemParent[Cell] = Cell;
        }
    }
    auto FindSystemRoot = [&](int32 Cell)
    {
        int32 Root = Cell;
        while (SystemParent[Root] != Root)
        {
            Root = SystemParent[Root];
        }
        while (SystemParent[Cell] != Cell)
        {
            const int32 Next = SystemParent[Cell];
            SystemParent[Cell] = Root;
            Cell = Next;
        }
        return Root;
    };
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (!Channel[Cell])
        {
            continue;
        }
        const int32 Receiver = PrimaryReceiver(Data, Cell);
        if (Receiver == INDEX_NONE || !Channel[Receiver])
        {
            continue;
        }
        const int32 RootA = FindSystemRoot(Cell);
        const int32 RootB = FindSystemRoot(Receiver);
        if (RootA != RootB)
        {
            SystemParent[RootB] = RootA;
        }
    }
    TArray<double> SystemLength;
    SystemLength.Init(0.0, Data.Height.Num());
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (!Channel[Cell])
        {
            continue;
        }
        const int32 Receiver = PrimaryReceiver(Data, Cell);
        if (Receiver != INDEX_NONE && Channel[Receiver])
        {
            SystemLength[FindSystemRoot(Cell)] += FVector2D::Distance(Data.CellPosition(Cell), Data.CellPosition(Receiver));
        }
    }
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (Channel[Cell] && SystemLength[FindSystemRoot(Cell)] < MinimumSystemLength)
        {
            Channel[Cell] = false;
        }
    }
    TArray<int32> Termini;
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (!Channel[Cell])
        {
            continue;
        }
        const int32 X = Cell % Data.Columns;
        const int32 Y = Cell / Data.Columns;
        constexpr int32 BoundaryMargin = 3;
        const bool bNearBoundary = X <= BoundaryMargin || X >= Data.Columns - 1 - BoundaryMargin ||
            Y <= BoundaryMargin || Y >= Data.Rows - 1 - BoundaryMargin;
        if (bNearBoundary)
        {
            continue;
        }
        const int32 Receiver = PrimaryReceiver(Data, Cell);
        if (Receiver == INDEX_NONE || !Channel[Receiver])
        {
            Termini.Add(Cell);
        }
    }
    return Termini;
}

static void ExtractLakes(
    FAvenorStripData& Data,
    const TArray<int32>& MandatoryTerminusSeeds,
    double MinimumDepth,
    double MinimumBedDepth,
    double MaximumBedDepth,
    double MaximumArea,
    int32 MaximumCount,
    double MaximumCoverageFraction,
    double BankBlendWidth,
    double DepthRampWidth,
    double SurfaceInset,
    bool bWantOutflows,
    TArray<int32>& OutOutflowSeeds
)
{
    Data.Lakes.Reset();
    Data.LakeIndex.Init(INDEX_NONE, Data.Height.Num());
    OutOutflowSeeds.Reset();
    if (MaximumCount <= 0 && MandatoryTerminusSeeds.IsEmpty())
    {
        return;
    }
    TSet<int32> MandatorySeedSet(MandatoryTerminusSeeds);
    const double WorldArea = Data.Height.Num() * Data.CellAreaSquareKilometres();
    TArray<bool> Candidate;
    Candidate.Init(false, Data.Height.Num());
    const double ShorelineFillThreshold = FMath::Max(50.0, MinimumDepth * 0.5);
    for (int32 Y = 1; Y + 1 < Data.Rows; ++Y)
    {
        for (int32 X = 1; X + 1 < Data.Columns; ++X)
        {
            const int32 Cell = Data.Index(X, Y);
            Candidate[Cell] = (Data.FilledHeight[Cell] - Data.Height[Cell]) >= ShorelineFillThreshold;
        }
    }
    TArray<bool> Visited;
    Visited.Init(false, Data.Height.Num());
    TArray<FLakeCandidate> Basins;
    constexpr int32 CardinalX[4] = {1, 0, -1, 0};
    constexpr int32 CardinalY[4] = {0, 1, 0, -1};
    for (int32 SeedCell = 0; SeedCell < Data.Height.Num(); ++SeedCell)
    {
        if (!Candidate[SeedCell] || Visited[SeedCell])
        {
            continue;
        }
        FLakeCandidate Basin;
        TArray<int32> Queue;
        Queue.Add(SeedCell);
        Visited[SeedCell] = true;
        for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
        {
            const int32 Cell = Queue[QueueIndex];
            Basin.Cells.Add(Cell);
            Basin.SurfaceHeight = FMath::Max(Basin.SurfaceHeight, Data.FilledHeight[Cell]);
            Basin.MaximumDepth = FMath::Max(Basin.MaximumDepth, Data.FilledHeight[Cell] - Data.Height[Cell]);
            Basin.CatchmentArea = FMath::Max(Basin.CatchmentArea, Data.Accumulation[Cell]);
            const FVector2D CellPos = Data.CellPosition(Cell);
            Basin.MinX = FMath::Min(Basin.MinX, CellPos.X);
            Basin.MaxX = FMath::Max(Basin.MaxX, CellPos.X);
            Basin.MinY = FMath::Min(Basin.MinY, CellPos.Y);
            Basin.MaxY = FMath::Max(Basin.MaxY, CellPos.Y);
            const int32 X = Cell % Data.Columns;
            const int32 Y = Cell / Data.Columns;
            for (int32 Direction = 0; Direction < 4; ++Direction)
            {
                const int32 NX = X + CardinalX[Direction];
                const int32 NY = Y + CardinalY[Direction];
                if (!Data.IsValid(NX, NY))
                {
                    continue;
                }
                const int32 Neighbor = Data.Index(NX, NY);
                if (Candidate[Neighbor])
                {
                    if (!Visited[Neighbor])
                    {
                        Visited[Neighbor] = true;
                        Queue.Add(Neighbor);
                    }
                }
                else if (Data.Height[Neighbor] < Basin.RimSpillHeight)
                {
                    // The lowest non-basin neighbour is the natural rim:
                    // where water overtops once the lake fills.
                    Basin.RimSpillHeight = Data.Height[Neighbor];
                    Basin.RimSpillCell = Neighbor;
                }
            }
        }
        const double Area = Basin.Cells.Num() * Data.CellAreaSquareKilometres();
        for (int32 Cell : Basin.Cells)
        {
            if (MandatorySeedSet.Contains(Cell))
            {
                Basin.bMandatory = true;
                break;
            }
        }
        if (Area >= Data.CellAreaSquareKilometres() * 2.0 && Area <= WorldArea * 0.2)
        {
            Basins.Add(MoveTemp(Basin));
        }
    }
    TArray<FLakeCandidate> MandatoryBasins;
    TArray<FLakeCandidate> OptionalBasins;
    for (FLakeCandidate& Basin : Basins)
    {
        (Basin.bMandatory ? MandatoryBasins : OptionalBasins).Add(MoveTemp(Basin));
    }
    MandatoryBasins.Sort([](const FLakeCandidate& A, const FLakeCandidate& B) { return A.CatchmentArea > B.CatchmentArea; });
    OptionalBasins.Sort([](const FLakeCandidate& A, const FLakeCandidate& B)
    {
        return A.CatchmentArea * FMath::Sqrt(A.MaximumDepth) > B.CatchmentArea * FMath::Sqrt(B.MaximumDepth);
    });
    TArray<FLakeCandidate> OrderedBasins;
    OrderedBasins.Reserve(MandatoryBasins.Num() + OptionalBasins.Num());
    for (FLakeCandidate& Basin : MandatoryBasins) { OrderedBasins.Add(MoveTemp(Basin)); }
    for (FLakeCandidate& Basin : OptionalBasins) { OrderedBasins.Add(MoveTemp(Basin)); }

    const double MaximumTotalLakeArea = WorldArea * FMath::Clamp(MaximumCoverageFraction, 0.0, 0.5);
    double AcceptedLakeArea = 0.0;
    int32 AcceptedOptionalCount = 0;
    for (const FLakeCandidate& CandidateBasin : OrderedBasins)
    {
        const double BasinArea = CandidateBasin.Cells.Num() * Data.CellAreaSquareKilometres();
        if (BasinArea <= 0.0)
        {
            continue;
        }
        if (!CandidateBasin.bMandatory)
        {
            if (AcceptedOptionalCount >= MaximumCount ||
                CandidateBasin.MaximumDepth < MinimumDepth ||
                BasinArea > MaximumArea ||
                AcceptedLakeArea + BasinArea > MaximumTotalLakeArea)
            {
                continue;
            }
            const double BoundingWidth = FMath::Max(1.0, CandidateBasin.MaxX - CandidateBasin.MinX);
            const double BoundingHeight = FMath::Max(1.0, CandidateBasin.MaxY - CandidateBasin.MinY);
            const double FillRatio = BasinArea / FMath::Max(0.0001, (BoundingWidth * BoundingHeight) / 10000000000.0);
            if (FillRatio < 0.12)
            {
                continue;
            }
        }
        FLakeBasin Lake;
        Lake.SurfaceHeight = CandidateBasin.SurfaceHeight;
        Lake.MaximumDepth = FMath::Clamp(
            FMath::Max(CandidateBasin.MaximumDepth, MinimumBedDepth),
            MinimumBedDepth, FMath::Max(MinimumBedDepth, MaximumBedDepth)
        );
        Lake.BankBlendWidth = BankBlendWidth;
        Lake.DepthRampWidth = DepthRampWidth;
        Lake.Shoreline = TraceComponentBoundary(Data, CandidateBasin.Cells, Lake.SurfaceHeight);
        if (Lake.Shoreline.Num() < 4)
        {
            continue;
        }
        double MinimumShorelineHeight = TNumericLimits<double>::Max();
        for (const FVector& Point : Lake.Shoreline)
        {
            MinimumShorelineHeight = FMath::Min(MinimumShorelineHeight, Data.SampleGrid(Data.Height, FVector2D(Point)));
        }
        Lake.ShorelineHeight = MinimumShorelineHeight < TNumericLimits<double>::Max()
            ? FMath::Min(Lake.SurfaceHeight, MinimumShorelineHeight) : Lake.SurfaceHeight;
        Lake.SurfaceHeight = Lake.ShorelineHeight - FMath::Max(0.0, SurfaceInset);
        for (FVector& Point : Lake.Shoreline)
        {
            Point.Z = Lake.SurfaceHeight;
        }
        double BasinInradius = 0.0;
        for (int32 Cell : CandidateBasin.Cells)
        {
            double EdgeDistance = 0.0;
            if (IsInsidePolygon(Data.CellPosition(Cell), Lake.Shoreline, &EdgeDistance))
            {
                BasinInradius = FMath::Max(BasinInradius, EdgeDistance);
            }
        }
        const double MinimumRamp = FMath::Max(500.0, Data.CellSize * 0.15);
        const double BasinRamp = FMath::Max(MinimumRamp, BasinInradius * 0.65);
        Lake.DepthRampWidth = FMath::Clamp(FMath::Max(DepthRampWidth, BasinInradius * 0.35), MinimumRamp, BasinRamp);
        for (const FVector& Point : Lake.Shoreline)
        {
            Lake.Bounds += FVector2D(Point);
        }
        Lake.Bounds = Lake.Bounds.ExpandBy(BankBlendWidth);
        const int32 NewLakeIndex = Data.Lakes.Num();
        for (int32 Cell : CandidateBasin.Cells)
        {
            Data.LakeIndex[Cell] = NewLakeIndex;
        }
        AcceptedLakeArea += BasinArea;
        if (!CandidateBasin.bMandatory)
        {
            ++AcceptedOptionalCount;
        }
        // Fills from its inflow(s) and, once at the rim, overflows here.
        // The downstream neighbour of the rim's lowest point is where a
        // real outflow river would begin.
        if (bWantOutflows && CandidateBasin.RimSpillCell != INDEX_NONE)
        {
            const int32 OutflowStart = PrimaryReceiver(Data, CandidateBasin.RimSpillCell);
            if (OutflowStart != INDEX_NONE &&
                (!Data.LakeIndex.IsValidIndex(OutflowStart) || Data.LakeIndex[OutflowStart] == INDEX_NONE))
            {
                OutOutflowSeeds.Add(OutflowStart);
            }
        }
        Data.Lakes.Add(MoveTemp(Lake));
    }
}
} // namespace UE::Avenor::Strip

namespace UE::Avenor::Strip
{
static void EnforceDownhill(TArray<FVector>& Points)
{
    if (Points.Num() < 2)
    {
        return;
    }
    for (int32 Index = 1; Index < Points.Num(); ++Index)
    {
        const double HorizontalDistance = FVector2D::Distance(FVector2D(Points[Index - 1]), FVector2D(Points[Index]));
        Points[Index].Z = FMath::Min(Points[Index].Z, Points[Index - 1].Z - HorizontalDistance * 0.0006);
    }
}

struct FRiverCandidate
{
    TArray<int32> Cells;
    double Score = 0.0;
};

// Rivers form as a tree: every cell has exactly one downstream neighbour
// (already known from flow accumulation), so the whole network is one
// directed structure. Shared trunk segments are only ever traced once -
// tributaries stop cleanly at confluences, never overlapping.
static void ExtractRivers(
    FAvenorStripData& Data,
    const TArray<int32>& ForcedOutflowSeeds,
    int32 Seed,
    double MountainStartArea,
    double LowlandStartArea,
    double MainRiverArea,
    double MinimumSystemLength,
    double HeadwaterWidth,
    double MainRiverWidth,
    double MaximumDepth,
    double HeadwaterValleyWidth,
    double MainValleyWidth,
    double MaximumValleyDepth,
    double MeanderStrength,
    int32 MaximumReaches,
    bool bCanyons,
    double CanyonStartArea
)
{
    Data.Rivers.Reset();
    if (MaximumReaches <= 0)
    {
        return;
    }
    TArray<bool> Channel;
    Channel.Init(false, Data.Height.Num());
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (Data.LakeIndex.IsValidIndex(Cell) && Data.LakeIndex[Cell] != INDEX_NONE)
        {
            continue;
        }
        const double MountainFraction = FMath::Clamp(Data.Slope[Cell] / 0.18, 0.0, 1.0);
        const double StartArea = FMath::Lerp(LowlandStartArea, MountainStartArea, MountainFraction);
        Channel[Cell] = Data.Accumulation[Cell] >= StartArea && PrimaryReceiver(Data, Cell) != INDEX_NONE;
    }
    // Outflow seeds are a lake's spillway - they get to count as channel
    // regardless of local accumulation, since the water arriving there is
    // whatever filled the lake upstream, not what this one cell drains.
    for (int32 Seed2 : ForcedOutflowSeeds)
    {
        if (Data.Height.IsValidIndex(Seed2) &&
            (!Data.LakeIndex.IsValidIndex(Seed2) || Data.LakeIndex[Seed2] == INDEX_NONE) &&
            PrimaryReceiver(Data, Seed2) != INDEX_NONE)
        {
            Channel[Seed2] = true;
        }
    }

    TArray<int32> SystemParent;
    SystemParent.Init(INDEX_NONE, Data.Height.Num());
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (Channel[Cell])
        {
            SystemParent[Cell] = Cell;
        }
    }
    auto FindSystemRoot = [&](int32 Cell)
    {
        int32 Root = Cell;
        while (SystemParent[Root] != Root)
        {
            Root = SystemParent[Root];
        }
        while (SystemParent[Cell] != Cell)
        {
            const int32 Next = SystemParent[Cell];
            SystemParent[Cell] = Root;
            Cell = Next;
        }
        return Root;
    };
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (!Channel[Cell])
        {
            continue;
        }
        const int32 Receiver = PrimaryReceiver(Data, Cell);
        if (Receiver == INDEX_NONE || !Channel[Receiver])
        {
            continue;
        }
        const int32 RootA = FindSystemRoot(Cell);
        const int32 RootB = FindSystemRoot(Receiver);
        if (RootA != RootB)
        {
            SystemParent[RootB] = RootA;
        }
    }
    TSet<int32> ForcedSeedSet(ForcedOutflowSeeds);
    TArray<double> SystemLength;
    SystemLength.Init(0.0, Data.Height.Num());
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (!Channel[Cell])
        {
            continue;
        }
        const int32 Receiver = PrimaryReceiver(Data, Cell);
        if (Receiver != INDEX_NONE && Channel[Receiver])
        {
            SystemLength[FindSystemRoot(Cell)] += FVector2D::Distance(Data.CellPosition(Cell), Data.CellPosition(Receiver));
        }
    }
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        // A forced outflow seed's system must survive even if short - it
        // is, by construction, a real lake spillway, not incidental noise.
        if (Channel[Cell] && SystemLength[FindSystemRoot(Cell)] < MinimumSystemLength &&
            !ForcedSeedSet.Contains(FindSystemRoot(Cell)))
        {
            bool bContainsForcedSeed = false;
            for (int32 ForcedSeedCell : ForcedOutflowSeeds)
            {
                if (Channel[ForcedSeedCell] && FindSystemRoot(ForcedSeedCell) == FindSystemRoot(Cell))
                {
                    bContainsForcedSeed = true;
                    break;
                }
            }
            if (!bContainsForcedSeed)
            {
                Channel[Cell] = false;
            }
        }
    }
    TArray<int32> UpstreamCount;
    UpstreamCount.Init(0, Data.Height.Num());
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (!Channel[Cell])
        {
            continue;
        }
        const int32 Receiver = PrimaryReceiver(Data, Cell);
        if (Receiver != INDEX_NONE && Channel[Receiver])
        {
            ++UpstreamCount[Receiver];
        }
    }
    TArray<int32> Starts;
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (Channel[Cell] && UpstreamCount[Cell] != 1)
        {
            Starts.Add(Cell);
        }
    }
    Starts.Sort([&](int32 A, int32 B) { return Data.FilledHeight[A] > Data.FilledHeight[B]; });

    TSet<int64> UsedEdges;
    TArray<FRiverCandidate> Candidates;
    for (int32 Start : Starts)
    {
        FRiverCandidate CandidateReach;
        int32 Cell = Start;
        for (int32 Guard = 0; Guard < Data.Height.Num(); ++Guard)
        {
            CandidateReach.Cells.Add(Cell);
            const int32 Receiver = PrimaryReceiver(Data, Cell);
            if (Receiver == INDEX_NONE || !Channel[Receiver])
            {
                break;
            }
            const int64 EdgeKey = (static_cast<int64>(Cell) << 32) | static_cast<uint32>(Receiver);
            if (UsedEdges.Contains(EdgeKey))
            {
                break;
            }
            UsedEdges.Add(EdgeKey);
            Cell = Receiver;
            if (Data.LakeIndex.IsValidIndex(Cell) && Data.LakeIndex[Cell] != INDEX_NONE)
            {
                break;
            }
            if (UpstreamCount[Cell] != 1)
            {
                CandidateReach.Cells.Add(Cell);
                break;
            }
        }
        if (CandidateReach.Cells.Num() >= 2)
        {
            const int32 EndCell = CandidateReach.Cells.Last();
            CandidateReach.Score = CandidateReach.Cells.Num() * Data.CellSize *
                FMath::Sqrt(FMath::Max(0.01, Data.Accumulation[EndCell])) *
                (1.0 + 0.35 * FMath::Clamp(Data.Slope[Start] / 0.12, 0.0, 1.0));
            Candidates.Add(MoveTemp(CandidateReach));
        }
    }
    Candidates.Sort([](const FRiverCandidate& A, const FRiverCandidate& B) { return A.Score > B.Score; });
    const int32 ReachCount = FMath::Min(MaximumReaches, Candidates.Num());
    for (int32 ReachIndex = 0; ReachIndex < ReachCount; ++ReachIndex)
    {
        const FRiverCandidate& CandidateReach = Candidates[ReachIndex];
        TArray<FVector> Points;
        Points.Reserve(CandidateReach.Cells.Num());
        double MeanSlope = 0.0;
        for (int32 Cell : CandidateReach.Cells)
        {
            const FVector2D Position = Data.CellPosition(Cell);
            Points.Emplace(Position.X, Position.Y, Data.FilledHeight[Cell]);
            MeanSlope += Data.Slope[Cell];
        }
        MeanSlope /= CandidateReach.Cells.Num();
        Points = ChaikinSmooth(Points, false, 1);
        Points = ResamplePolyline(Points, Data.CellSize * 1.35, false);
        const double LowlandFraction = 1.0 - FMath::Clamp(MeanSlope / 0.12, 0.0, 1.0);
        AddBroadMeanders(Points, Data.CellSize, MeanderStrength, LowlandFraction, Seed ^ (ReachIndex * 0x45D9F3B), Data.Bounds);
        Points = ChaikinSmooth(Points, false, 2);
        Points = ResamplePolyline(Points, FMath::Max(2500.0, Data.CellSize * 0.42), false);
        for (FVector& Point : Points)
        {
            const FVector2D Position(Point);
            const double LocalArea = Data.SampleGrid(Data.Accumulation, Position);
            const double LocalAlpha = DrainageScaleAlpha(LocalArea, MainRiverArea);
            double ValleyInset = FMath::Lerp(MaximumValleyDepth * 0.12, MaximumValleyDepth, LocalAlpha);
            if (bCanyons && LocalArea >= CanyonStartArea)
            {
                ValleyInset *= 1.2;
            }
            Point.Z = Data.SampleGrid(Data.Height, Position) - FMath::Lerp(150.0, 900.0, LocalAlpha) - ValleyInset;
        }
        EnforceDownhill(Points);

        const int32 EndCell = CandidateReach.Cells.Last();
        const double Area = Data.Accumulation[EndCell];
        const double RiverAlpha = DrainageScaleAlpha(Area, MainRiverArea);
        FRiverReach River;
        River.Points = MoveTemp(Points);
        River.DrainageArea = Area;
        River.Width = FMath::Lerp(HeadwaterWidth, MainRiverWidth, RiverAlpha);
        River.Depth = FMath::Lerp(FMath::Max(120.0, MaximumDepth * 0.12), MaximumDepth, RiverAlpha);
        River.ValleyHalfWidth = FMath::Lerp(HeadwaterValleyWidth, MainValleyWidth, RiverAlpha);
        River.ValleyDepth = FMath::Lerp(River.Depth * 1.4, MaximumValleyDepth, RiverAlpha);
        if (bCanyons && Area >= CanyonStartArea && MeanSlope > 0.055)
        {
            River.ValleyHalfWidth *= 0.48;
            River.ValleyDepth = FMath::Max(River.ValleyDepth, MaximumValleyDepth * 1.35);
            River.CrossSectionExponent = 0.62;
        }
        else
        {
            River.CrossSectionExponent = FMath::Lerp(1.65, 0.82, FMath::Clamp(MeanSlope / 0.16, 0.0, 1.0));
        }
        for (const FVector& Point : River.Points)
        {
            River.Bounds += FVector2D(Point);
        }
        River.Bounds = River.Bounds.ExpandBy(River.ValleyHalfWidth);
        Data.Rivers.Add(MoveTemp(River));
    }
}
} // namespace UE::Avenor::Strip

double FAvenorStripData::SampleHeight(const FVector2D& Position) const
{
    using namespace UE::Avenor::Strip;
    const double Underlying = SampleGrid(Height, Position);
    double Result = Underlying;
    bool bInsideLake = false;
    bool bNearLakeBank = false;
    double BestBankAlpha = TNumericLimits<double>::Max();
    for (const FLakeBasin& Lake : Lakes)
    {
        if (!Lake.Bounds.IsInside(Position) || Lake.Shoreline.Num() < 3)
        {
            continue;
        }
        double EdgeDistance = 0.0;
        const bool bInside = IsInsidePolygon(Position, Lake.Shoreline, &EdgeDistance);
        const double Radius = FMath::Max(CellSize * 0.35, Lake.DepthRampWidth);
        if (bInside)
        {
            const double DepthAlpha = Smooth01(FMath::Clamp(EdgeDistance / Radius, 0.0, 1.0));
            const double BedNoise = Fbm(Position, FMath::Max(Radius * 2.0, CellSize * 2.0), FVector2D(4231.0, -8877.0), 3);
            const double BedDepth = Lake.MaximumDepth * FMath::Clamp(0.86 + 0.14 * BedNoise, 0.72, 1.0);
            Result = FMath::Lerp(Lake.SurfaceHeight, Lake.SurfaceHeight - BedDepth, DepthAlpha);
            bInsideLake = true;
            break;
        }
        else if (EdgeDistance < Lake.BankBlendWidth)
        {
            const double BankAlpha = Smooth01(FMath::Clamp(EdgeDistance / FMath::Max(1.0, Lake.BankBlendWidth), 0.0, 1.0));
            if (BankAlpha < BestBankAlpha)
            {
                BestBankAlpha = BankAlpha;
                Result = FMath::Lerp(Lake.SurfaceHeight, Underlying, BankAlpha);
                bNearLakeBank = true;
            }
        }
    }
    if (bInsideLake || bNearLakeBank)
    {
        return Result;
    }
    bool bHasRiverProfile = false;
    double BestRiverInfluence = 0.0;
    double BestRiverTarget = Result;
    for (const FRiverReach& River : Rivers)
    {
        if (!River.Bounds.IsInside(Position) || River.Points.Num() < 2)
        {
            continue;
        }
        double ClosestDistance = TNumericLimits<double>::Max();
        double SurfaceHeight = 0.0;
        for (int32 Index = 0; Index + 1 < River.Points.Num(); ++Index)
        {
            const FVector& A = River.Points[Index];
            const FVector& B = River.Points[Index + 1];
            double Alpha = 0.0;
            const double Distance = SegmentDistance(Position, FVector2D(A), FVector2D(B), &Alpha);
            if (Distance < ClosestDistance)
            {
                ClosestDistance = Distance;
                SurfaceHeight = FMath::Lerp(A.Z, B.Z, Alpha);
            }
        }
        if (ClosestDistance >= River.ValleyHalfWidth)
        {
            continue;
        }
        const double WaterHalfWidth = FMath::Max(100.0, River.Width * 0.5);
        double Influence = 0.0;
        double CarveTarget = Underlying;
        if (ClosestDistance <= WaterHalfWidth)
        {
            const double ChannelAlpha = Smooth01(FMath::Clamp(ClosestDistance / WaterHalfWidth, 0.0, 1.0));
            CarveTarget = FMath::Lerp(SurfaceHeight - River.Depth, SurfaceHeight, ChannelAlpha);
            Influence = 2.0 - ChannelAlpha;
        }
        else
        {
            const double ValleyAlpha = FMath::Clamp(
                (ClosestDistance - WaterHalfWidth) / FMath::Max(1.0, River.ValleyHalfWidth - WaterHalfWidth), 0.0, 1.0
            );
            const double Shaped = FMath::Pow(ValleyAlpha, FMath::Max(0.2, River.CrossSectionExponent));
            const double BankAlpha = Smooth01(Shaped);
            CarveTarget = FMath::Lerp(SurfaceHeight, Underlying, BankAlpha);
            Influence = 1.0 - BankAlpha;
        }
        if (!bHasRiverProfile || Influence > BestRiverInfluence)
        {
            bHasRiverProfile = true;
            BestRiverInfluence = Influence;
            BestRiverTarget = CarveTarget;
        }
    }
    if (bHasRiverProfile)
    {
        Result = BestRiverTarget;
    }
    return Result;
}

float FAvenorStripData::SampleChannel(FName Channel, const FVector2D& Position) const
{
    using namespace UE::Avenor::Strip;
    if (Channel == ElevationChannel)
    {
        return static_cast<float>(FMath::Clamp(
            (SampleHeight(Position) - Bounds.Min.Z) / FMath::Max(1.0, Bounds.GetSize().Z), 0.0, 1.0
        ));
    }
    if (Channel == SlopeChannel)
    {
        return static_cast<float>(FMath::Clamp(SampleGrid(Slope, Position) / 0.35, 0.0, 1.0));
    }
    if (Channel == WetnessChannel)
    {
        return static_cast<float>(FMath::Clamp(FMath::Loge(1.0 + SampleGrid(Accumulation, Position)) / 6.0, 0.0, 1.0));
    }
    if (Channel == LakeChannel)
    {
        for (const FLakeBasin& Lake : Lakes)
        {
            if (Lake.Bounds.IsInside(Position) && IsInsidePolygon(Position, Lake.Shoreline))
            {
                return 1.0f;
            }
        }
        return 0.0f;
    }
    if (Channel == RiverChannel)
    {
        for (const FRiverReach& River : Rivers)
        {
            if (!River.Bounds.IsInside(Position))
            {
                continue;
            }
            for (int32 Index = 0; Index + 1 < River.Points.Num(); ++Index)
            {
                const double Distance = SegmentDistance(Position, FVector2D(River.Points[Index]), FVector2D(River.Points[Index + 1]));
                if (Distance <= River.ValleyHalfWidth)
                {
                    return static_cast<float>(1.0 - Distance / River.ValleyHalfWidth);
                }
            }
        }
        return 0.0f;
    }
    return 0.0f;
}

namespace UE::Avenor::Strip
{
// ---------------------------------------------------------------------
// Landform: a shallow spine valley runs the world's length. Four biome
// affinities (mountainous / hilly / desert-canyon / plains) blend
// smoothly along that same length via low-frequency noise, each weighted
// by its configured prevalence. Mountain and desert relief additionally
// scale up toward the two long edges, so the strip reads as "valley in
// the middle, big features on the sides" without ever hard-cutting
// between zones or hard-cutting between spine and edge.
// ---------------------------------------------------------------------

static double EvaluateLandform(
    const FVector2D& Position,
    const FBox& Bounds,
    int32 Seed,
    EAvenorStripLongAxis LongAxis,
    bool bMountains,
    bool bHills,
    bool bDesert,
    double SpineValleyDepth,
    double SpineWidthFraction,
    double ZoneLength,
    double MountainWeight,
    double HillWeight,
    double DesertWeight,
    double PlainsWeight,
    double MountainRelief,
    double MountainFeatureScale,
    double HillsRelief,
    double HillsScale,
    double MesaRelief,
    double MesaScale,
    int32 MesaTerraces,
    bool bOcean,
    double SeaLevel,
    double MinimumOceanDepth,
    double MaximumOceanDepth,
    double CoastWidth,
    bool bOceanWidthEdges,
    bool bOceanLengthEnds
)
{
    const FVector2D SeedOffset(
        static_cast<double>((Seed * 92821) & 0x7ffff),
        static_cast<double>((Seed * 68917) & 0x7ffff)
    );
    const FVector2D Warp(
        Fbm(Position, 950000.0, SeedOffset + FVector2D(137.0, 911.0), 3),
        Fbm(Position, 950000.0, SeedOffset + FVector2D(733.0, 271.0), 3)
    );
    const FVector2D Warped = Position + Warp * 180000.0;

    const FVector2D Centre(Bounds.GetCenter());
    const double AcrossExtent = LongAxis == EAvenorStripLongAxis::X
        ? Bounds.GetSize().Y * 0.5 : Bounds.GetSize().X * 0.5;
    const double AcrossOffset = LongAxis == EAvenorStripLongAxis::X
        ? (Position.Y - Centre.Y) : (Position.X - Centre.X);
    const double AcrossFraction = FMath::Clamp(FMath::Abs(AcrossOffset) / FMath::Max(1.0, AcrossExtent), 0.0, 1.0);
    const double SpineWidth = FMath::Clamp(SpineWidthFraction, 0.01, 1.0);
    const double SpineBlend = Smooth01(AcrossFraction / SpineWidth);
    const double BaseValley = FMath::Lerp(-SpineValleyDepth, 0.0, SpineBlend);
    const double EdgeEmphasis = FMath::Lerp(0.3, 1.0, AcrossFraction);

    const double ZoneCoordinate = LongAxis == EAvenorStripLongAxis::X ? Position.X : Position.Y;
    const double ZoneT = ZoneCoordinate / FMath::Max(1.0, ZoneLength);
    auto ZoneAffinity = [&](double PhaseOffset)
    {
        return 0.5 + 0.5 * Fbm(FVector2D(ZoneT, PhaseOffset), 1.0, SeedOffset, 3, 0.55, 2.0);
    };
    double MountainWeighted = ZoneAffinity(0.0) * FMath::Max(0.0, MountainWeight);
    double HillWeighted = ZoneAffinity(131.0) * FMath::Max(0.0, HillWeight);
    double DesertWeighted = ZoneAffinity(277.0) * FMath::Max(0.0, DesertWeight);
    double PlainsWeighted = ZoneAffinity(419.0) * FMath::Max(0.0, PlainsWeight);
    const double WeightSum = FMath::Max(
        0.0001, MountainWeighted + HillWeighted + DesertWeighted + PlainsWeighted
    );
    const double MountainAffinity = MountainWeighted / WeightSum;
    const double HillAffinity = HillWeighted / WeightSum;
    const double DesertAffinity = DesertWeighted / WeightSum;
    const double PlainsAffinity = PlainsWeighted / WeightSum;

    double Height = BaseValley;

    if (bMountains)
    {
        const double MountainNoise = RidgedFbm(Warped, MountainFeatureScale, SeedOffset + FVector2D(811.0, 157.0), 5);
        Height += MountainNoise * MountainRelief * MountainAffinity * EdgeEmphasis;
    }
    if (bHills)
    {
        const double HillNoise = Fbm(Warped, HillsScale, SeedOffset + FVector2D(887.0, 157.0), 5, 0.54, 2.03);
        Height += HillNoise * HillsRelief * HillAffinity * FMath::Lerp(0.6, 1.0, AcrossFraction);
    }
    if (bDesert)
    {
        const double DesertNoise = 0.5 + 0.5 * Fbm(Warped, MesaScale, SeedOffset + FVector2D(555.0, 222.0), 4);
        double Terraced = DesertNoise;
        if (MesaTerraces > 1)
        {
            const double Stepped = FMath::Floor(DesertNoise * MesaTerraces + 0.35) / MesaTerraces;
            Terraced = FMath::Lerp(DesertNoise, Stepped, 0.72);
        }
        Height += Terraced * MesaRelief * DesertAffinity * EdgeEmphasis;
    }
    {
        const double PlainsNoise = Fbm(Warped, HillsScale * 3.0, SeedOffset + FVector2D(101.0, 43.0), 4);
        Height += PlainsNoise * (HillsRelief * 0.12) * PlainsAffinity;
    }
    return Height;
}
} // namespace UE::Avenor::Strip

using namespace UE::Avenor::Strip;

namespace UE::Avenor::Strip
{
static TSharedPtr<FAvenorStripData> GenerateData(const AAvenorStripTerrainGenerator& Generator)
{
    const FBox Bounds = Generator.GetGenerationBounds();
    if (!Bounds.IsValid)
    {
        return nullptr;
    }
    TSharedPtr<FAvenorStripData> Data = MakeShared<FAvenorStripData>();
    Data->Bounds = Bounds;
    const FVector Size = Bounds.GetSize();
    double CellSize = FMath::Max(2500.0, Generator.AnalysisCellSize);
    int64 Columns = FMath::Max<int64>(2, FMath::CeilToInt(Size.X / CellSize));
    int64 Rows = FMath::Max<int64>(2, FMath::CeilToInt(Size.Y / CellSize));
    const int64 MaximumCells = FMath::Clamp<int64>(Generator.MaximumAnalysisCells, 10000, 2000000);
    if (Columns * Rows > MaximumCells)
    {
        CellSize *= FMath::Sqrt(static_cast<double>(Columns * Rows) / static_cast<double>(MaximumCells));
        Columns = FMath::Max<int64>(2, FMath::CeilToInt(Size.X / CellSize));
        Rows = FMath::Max<int64>(2, FMath::CeilToInt(Size.Y / CellSize));
    }
    Data->Columns = static_cast<int32>(Columns);
    Data->Rows = static_cast<int32>(Rows);
    Data->CellSize = CellSize;
    const int32 CellCount = Data->Columns * Data->Rows;
    Data->Height.SetNumUninitialized(CellCount);

    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        Data->Height[Cell] = EvaluateLandform(
            Data->CellPosition(Cell), Bounds, Generator.Seed, Generator.LongAxis,
            Generator.bGenerateMountains, Generator.bGenerateHills, Generator.bGenerateMesasAndCanyons,
            Generator.SpineValleyDepth, Generator.SpineWidthFraction, Generator.ZoneLength,
            Generator.MountainZoneWeight, Generator.HillZoneWeight, Generator.DesertZoneWeight, Generator.PlainsZoneWeight,
            Generator.MountainRelief, Generator.MountainFeatureScale,
            Generator.HillsRelief, Generator.HillsScale,
            Generator.MesaRelief, Generator.MesaScale, Generator.MesaTerraces,
            Generator.bGenerateOcean, Generator.SeaLevel,
            Generator.MinimumOceanDepth, Generator.MaximumOceanDepth,
            Generator.CoastTransitionWidth, Generator.bOceanWidthEdges,
            Generator.bOceanLengthEnds
        );
    }

    ApplyThermalErosion(*Data, Generator.ThermalErosionIterations, Generator.ThermalErosionStrength, Generator.TalusAngleDegrees);
    ApplyStreamPowerErosion(
        *Data, Generator.StreamPowerIterations, Generator.StreamPowerStrength,
        FMath::Min(Generator.MountainStreamStartArea, Generator.LowlandStreamStartArea), Generator.DrainageEpsilon
    );

    TArray<int32> OutflowSeeds;
    if (Generator.bGenerateLakes)
    {
        const TArray<int32> MandatoryTerminusSeeds = Generator.bGenerateRivers
            ? FindDrainageTerminusSeeds(*Data, Generator.MountainStreamStartArea, Generator.LowlandStreamStartArea, Generator.MinimumRiverSystemLength)
            : TArray<int32>();
        ExtractLakes(
            *Data, MandatoryTerminusSeeds, Generator.MinimumLakeDepth, Generator.MinimumLakeBedDepth,
            Generator.MaximumLakeBedDepth, Generator.MaximumLakeArea, Generator.MaximumLakeCount,
            Generator.MaximumLakeCoverageFraction, Generator.LakeBankBlendWidth, Generator.LakeDepthRampWidth,
            Generator.LakeSurfaceInset, Generator.bGenerateLakeOutflows, OutflowSeeds
        );
    }
    else
    {
        Data->LakeIndex.Init(INDEX_NONE, CellCount);
    }
    if (Generator.bGenerateRivers)
    {
        ExtractRivers(
            *Data, OutflowSeeds, Generator.Seed, Generator.MountainStreamStartArea, Generator.LowlandStreamStartArea,
            Generator.MainRiverArea, Generator.MinimumRiverSystemLength, Generator.HeadwaterWidth, Generator.MainRiverWidth,
            Generator.MaximumRiverDepth, Generator.HeadwaterValleyHalfWidth, Generator.MainValleyHalfWidth,
            Generator.MaximumValleyDepth, Generator.LowlandMeanderStrength, Generator.MaximumRiverReaches,
            Generator.bGenerateMesasAndCanyons, Generator.CanyonStartArea
        );
    }
    if (Generator.bGenerateOcean)
    {
        const double Inset = FMath::Clamp(Generator.CoastTransitionWidth, CellSize, FMath::Min(Size.X, Size.Y) * 0.42);
        TArray<FVector> Boundary = {
            FVector(Bounds.Min.X + Inset, Bounds.Min.Y + Inset, Generator.SeaLevel),
            FVector(Bounds.Max.X - Inset, Bounds.Min.Y + Inset, Generator.SeaLevel),
            FVector(Bounds.Max.X - Inset, Bounds.Max.Y - Inset, Generator.SeaLevel),
            FVector(Bounds.Min.X + Inset, Bounds.Max.Y - Inset, Generator.SeaLevel)
        };
        Boundary = ChaikinSmooth(Boundary, true, 2);
        Data->OceanBoundary = ResamplePolyline(Boundary, FMath::Max(CellSize, 5000.0), true);
    }
    return Data;
}

class FStripTerrainOp final : public UE::MeshPartition::IModifierBackgroundOp
{
public:
    explicit FStripTerrainOp(FName Name) : IModifierBackgroundOp(Name) {}

    virtual void GetInstancesInBounds(const FBox& InBounds, TArray<FInstanceInfo>& OutInstances) const override
    {
        if (!WorldBounds.Intersect(InBounds))
        {
            return;
        }
        FInstanceInfo& Instance = OutInstances.AddDefaulted_GetRef();
        Instance.InstanceID = 0;
        Instance.Bounds = WorldBounds;
        Instance.ReadViewComponents = UE::MeshPartition::EMeshViewComponents::VertexPos;
        Instance.WriteViewComponents = static_cast<UE::MeshPartition::EMeshViewComponents>(
            UE::MeshPartition::EMeshViewComponents::VertexPos |
            UE::MeshPartition::EMeshViewComponents::VertexAttributeWeight
        );
        Instance.UsedChannels = { ElevationChannel, SlopeChannel, WetnessChannel, RiverChannel, LakeChannel };
    }

    virtual void ApplyModifications(
        UE::MeshPartition::FMeshView& MeshView,
        const FTransform3d& MeshTransform,
        const FInstanceInfo& InstanceInfo
    ) const override
    {
        (void)InstanceInfo;
        if (!Data)
        {
            return;
        }
        for (int32 Vertex = 0; Vertex < MeshView.VertexCount(); ++Vertex)
        {
            FVector3d WorldPosition = MeshTransform.TransformPosition(MeshView.GetVertexPos(Vertex));
            const FVector2D XY(WorldPosition.X, WorldPosition.Y);
            const double SampledHeight = Data->SampleHeight(XY);
            WorldPosition.Z = BaseWorldZ + SampledHeight;
            MeshView.SetVertexPos(Vertex, MeshTransform.InverseTransformPosition(WorldPosition));
            MeshView.SetVertexAttributeWeight(
                ElevationChannel,
                Vertex,
                static_cast<float>(FMath::Clamp(
                    (SampledHeight - Data->Bounds.Min.Z) /
                        FMath::Max(1.0, Data->Bounds.GetSize().Z),
                    0.0,
                    1.0
                ))
            );
            MeshView.SetVertexAttributeWeight(SlopeChannel, Vertex, Data->SampleChannel(SlopeChannel, XY));
            MeshView.SetVertexAttributeWeight(WetnessChannel, Vertex, Data->SampleChannel(WetnessChannel, XY));
            MeshView.SetVertexAttributeWeight(RiverChannel, Vertex, Data->SampleChannel(RiverChannel, XY));
            MeshView.SetVertexAttributeWeight(LakeChannel, Vertex, Data->SampleChannel(LakeChannel, XY));
        }
    }

    virtual bool DisableDDCWrite() const override { return false; }
    static FGuid Version() { return FGuid(TEXT("5f3b61a2-8d44-4f73-a92d-81b6c2e7d409")); }

    FBox WorldBounds = FBox(ForceInit);
    double BaseWorldZ = 0.0;
    TSharedPtr<const FAvenorStripData> Data;
};

template<typename TWaterActor>
static TWaterActor* SpawnWaterActor(
    UWorld& World,
    const FString& Label,
    FName OwnerTag
)
{
    UActorFactory* Factory = GEditor ? GEditor->FindActorFactoryForActorClass(TWaterActor::StaticClass()) : nullptr;
    if (!Factory)
    {
        return nullptr;
    }
    TWaterActor* Actor = Cast<TWaterActor>(Factory->CreateActor(TWaterActor::StaticClass(), World.GetCurrentLevel(), FTransform::Identity));
    if (Actor)
    {
        Actor->SetActorLabel(Label);
        Actor->SetFolderPath(TEXT("Avenor/Generated/StripWater"));
        Actor->Tags.AddUnique(GeneratedWaterTag);
        Actor->Tags.AddUnique(OwnerTag);
    }
    return Actor;
}

static void ConfigureExactSpline(UWaterSplineComponent& Spline, const TArray<FVector>& Points, bool bClosed)
{
    Spline.SetSplinePoints(Points, ESplineCoordinateSpace::World, false);
    Spline.SetClosedLoop(bClosed, false);
    for (int32 Index = 0; Index < Spline.GetNumberOfSplinePoints(); ++Index)
    {
        Spline.SetSplinePointType(Index, ESplinePointType::Linear, false);
    }
    Spline.UpdateSpline();
}

static void ConfigureRiverSpline(UWaterSplineComponent& Spline, const TArray<FVector>& Points, double FullWidth, double Depth)
{
    ConfigureExactSpline(Spline, Points, false);
    UWaterSplineMetadata* Metadata = Cast<UWaterSplineMetadata>(Spline.GetSplinePointsMetadata());
    if (Metadata)
    {
        Metadata->Fixup(Spline.GetNumberOfSplinePoints(), &Spline);
        const float MetadataWidth = static_cast<float>(FMath::Max(100.0, FullWidth));
        const float WaterDepth = static_cast<float>(FMath::Max(1.0, Depth));
        for (int32 Index = 0; Index < Spline.GetNumberOfSplinePoints(); ++Index)
        {
            if (Metadata->RiverWidth.Points.IsValidIndex(Index))
            {
                Metadata->RiverWidth.Points[Index].OutVal = MetadataWidth;
            }
            if (Metadata->Depth.Points.IsValidIndex(Index))
            {
                Metadata->Depth.Points[Index].OutVal = WaterDepth;
            }
        }
    }
    Spline.K2_SynchronizeAndBroadcastDataChange();
}

static void DisableSecondaryTerrainCarving(AActor& WaterActor)
{
    TInlineComponentArray<UE::MeshPartition::UModifierComponent*> Modifiers;
    WaterActor.GetComponents(Modifiers);
    for (UE::MeshPartition::UModifierComponent* Modifier : Modifiers)
    {
        Modifier->SetAffectedMeshPartition(nullptr);
    }
}
} // namespace UE::Avenor::Strip

TArray<FBox> UAvenorStripTerrainModifier::ComputeBounds() const
{
    const AAvenorStripTerrainGenerator* Generator = Cast<AAvenorStripTerrainGenerator>(GetOwner());
    if (!Generator)
    {
        return {};
    }
    const FBox GenerationBounds = Generator->GetGenerationBounds();
    return GenerationBounds.IsValid ? TArray<FBox>{GenerationBounds} : TArray<FBox>{};
}

TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
UAvenorStripTerrainModifier::CreateBackgroundOp(UE::MeshPartition::EBuildType BuildType) const
{
    (void)BuildType;
    TSharedPtr<FStripTerrainOp> Op = MakeShared<FStripTerrainOp>(GetFName());
    const AAvenorStripTerrainGenerator* Generator = Cast<AAvenorStripTerrainGenerator>(GetOwner());
    if (Generator)
    {
        Op->WorldBounds = Generator->GetGenerationBounds();
        Op->BaseWorldZ = Generator->GetActorLocation().Z;
        Op->Data = Generator->GetOrCreateData();
    }
    return Op;
}

FGuid UAvenorStripTerrainModifier::GetCodeVersionKey() const
{
    return FStripTerrainOp::Version();
}

AAvenorStripTerrainGenerator::AAvenorStripTerrainGenerator()
{
    PrimaryActorTick.bCanEverTick = false;
    SetIsSpatiallyLoaded(false);
    TerrainModifier = CreateDefaultSubobject<UAvenorStripTerrainModifier>(TEXT("StripTerrain"));
    SetRootComponent(TerrainModifier);
    LastBuildStamp = TEXT("Never generated");
}

FBox AAvenorStripTerrainGenerator::GetGenerationBounds() const
{
    const FVector Centre = GetActorLocation();
    const FVector Extent(
        FMath::Max(10000.0, WorldSize.X) * 0.5,
        FMath::Max(10000.0, WorldSize.Y) * 0.5,
        500000.0
    );
    return FBox(Centre - Extent, Centre + Extent);
}

TSharedPtr<const FAvenorStripData> AAvenorStripTerrainGenerator::GetOrCreateData() const
{
    FScopeLock Lock(&DataMutex);
    if (!CachedData)
    {
        CachedData = GenerateData(*this);
    }
    return CachedData;
}

void AAvenorStripTerrainGenerator::InvalidateData()
{
    FScopeLock Lock(&DataMutex);
    CachedData.Reset();
    bTerrainPlanReadyForWater = false;
}

void AAvenorStripTerrainGenerator::ClearGeneratedWater()
{
#if WITH_EDITOR
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    const FName OwnerTag = MakeWaterOwnerTag(*this);
    TArray<AWaterBody*> ToDelete;
    for (TActorIterator<AWaterBody> It(World); It; ++It)
    {
        if (!It->Tags.Contains(GeneratedWaterTag))
        {
            continue;
        }

        // Owner-less actors were produced by the original generator. Delete
        // them once as a migration path; thereafter each generator only
        // replaces its own water.
        bool bHasAnyOwnerTag = false;
        for (const FName& Tag : It->Tags)
        {
            bHasAnyOwnerTag |= IsWaterOwnerTag(Tag);
        }
        if (It->Tags.Contains(OwnerTag) || !bHasAnyOwnerTag)
        {
            ToDelete.Add(*It);
        }
    }
    for (AWaterBody* Water : ToDelete)
    {
        DisableSecondaryTerrainCarving(*Water);
        World->EditorDestroyActor(Water, true);
    }
#endif
}

void AAvenorStripTerrainGenerator::CreateWaterActors(const TSharedPtr<const FAvenorStripData>& Data)
{
#if WITH_EDITOR
    UWorld* World = GetWorld();
    if (!World || !Data)
    {
        return;
    }
    const FName OwnerTag = MakeWaterOwnerTag(*this);
    auto ToWorldPoints = [&](const TArray<FVector>& Source)
    {
        TArray<FVector> Result = Source;
        for (FVector& Point : Result)
        {
            Point.Z += GetActorLocation().Z;
        }
        return Result;
    };
    if (Data->OceanBoundary.Num() >= 4)
    {
        if (AWaterBodyOcean* Ocean = SpawnWaterActor<AWaterBodyOcean>(
            *World, TEXT("Avenor_Strip_Ocean"), OwnerTag))
        {
            DisableSecondaryTerrainCarving(*Ocean);
            ConfigureExactSpline(*Ocean->GetWaterBodyComponent()->GetWaterSpline(), ToWorldPoints(Data->OceanBoundary), true);
            Ocean->PostEditChange();
            DisableSecondaryTerrainCarving(*Ocean);
        }
    }
    for (int32 Index = 0; Index < Data->Lakes.Num(); ++Index)
    {
        TArray<FVector> LakePoints = ToWorldPoints(Data->Lakes[Index].Shoreline);
        if (AWaterBodyLake* Lake = SpawnWaterActor<AWaterBodyLake>(
            *World,
            FString::Printf(TEXT("Avenor_Strip_Lake_%02d"), Index + 1),
            OwnerTag))
        {
            DisableSecondaryTerrainCarving(*Lake);
            ConfigureExactSpline(*Lake->GetWaterBodyComponent()->GetWaterSpline(), LakePoints, true);
            Lake->PostEditChange();
            ConfigureExactSpline(*Lake->GetWaterBodyComponent()->GetWaterSpline(), LakePoints, true);
            Lake->GetWaterBodyComponent()->GetWaterSpline()->K2_SynchronizeAndBroadcastDataChange();
            DisableSecondaryTerrainCarving(*Lake);
        }
    }
    for (int32 Index = 0; Index < Data->Rivers.Num(); ++Index)
    {
        TArray<FVector> RiverPoints = ToWorldPoints(Data->Rivers[Index].Points);
        for (FVector& Point : RiverPoints)
        {
            Point.Z -= 100.0;
        }
        if (AWaterBodyRiver* River = SpawnWaterActor<AWaterBodyRiver>(
            *World,
            FString::Printf(TEXT("Avenor_Strip_River_%03d"), Index + 1),
            OwnerTag))
        {
            DisableSecondaryTerrainCarving(*River);
            ConfigureRiverSpline(*River->GetWaterBodyComponent()->GetWaterSpline(), RiverPoints, Data->Rivers[Index].Width, Data->Rivers[Index].Depth);
            River->PostEditChange();
            ConfigureRiverSpline(*River->GetWaterBodyComponent()->GetWaterSpline(), RiverPoints, Data->Rivers[Index].Width, Data->Rivers[Index].Depth);
            DisableSecondaryTerrainCarving(*River);
        }
    }
#endif
}

void AAvenorStripTerrainGenerator::GenerateTerrain()
{
#if WITH_EDITOR
    UE::MeshPartition::AMeshPartition* TargetMeshPartition =
        Cast<UE::MeshPartition::AMeshPartition>(MeshPartitionActor);
    if (!TargetMeshPartition)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(TEXT(
                "Assign a valid Mesh Partition actor to MeshPartitionActor "
                "before generating terrain."
            ))
        );
        return;
    }

    FScopedSlowTask Progress(3.0f, FText::FromString(TEXT("Generating strip terrain plan...")));
    Progress.MakeDialog();
    Progress.EnterProgressFrame(1.0f, FText::FromString(TEXT("Building landforms and erosion")));
    bTerrainPlanReadyForWater = false;
    ClearGeneratedWater();
    InvalidateData();
    const TSharedPtr<const FAvenorStripData> Data = GetOrCreateData();
    if (!Data)
    {
        return;
    }
    Progress.EnterProgressFrame(1.0f, FText::FromString(TEXT("Registering Mesh Partition modifier")));
    if (TerrainModifier)
    {
        TerrainModifier->SetAffectedMeshPartition(nullptr);
        TerrainModifier->BP_SetAffectedMegaMesh(TargetMeshPartition);
    }
    Progress.EnterProgressFrame(1.0f, FText::FromString(TEXT("Submitting terrain modifier")));

    bTerrainPlanReadyForWater = true;

    LastBuildStamp = FString::Printf(
        TEXT("Terrain submitted %s | seed %d | %d cells @ %.0fcm | %d rivers | %d lakes | WAIT FOR MESH BUILD, THEN GENERATE WATER"),
        *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
        Seed, Data->Height.Num(), Data->CellSize, Data->Rivers.Num(), Data->Lakes.Num()
    );
    UE_LOG(LogTemp, Display, TEXT("Avenor strip terrain submitted: %s"), *LastBuildStamp);
#endif
}

void AAvenorStripTerrainGenerator::GenerateWater()
{
#if WITH_EDITOR
    if (!bTerrainPlanReadyForWater || !CachedData)
    {
        FMessageDialog::Open(
            EAppMsgType::Ok,
            FText::FromString(TEXT(
                "Generate Terrain must succeed first. Then wait until the "
                "Mesh Partition build has visibly completed before pressing "
                "Generate Water."
            ))
        );
        return;
    }

    const EAppReturnType::Type Confirmation = FMessageDialog::Open(
        EAppMsgType::YesNo,
        FText::FromString(TEXT(
            "Has the Mesh Partition terrain build completely finished?\n\n"
            "Choose No if it is still building. Water actors must not inspect "
            "the old mesh."
        ))
    );
    if (Confirmation != EAppReturnType::Yes)
    {
        return;
    }

    FScopedSlowTask Progress(2.0f, FText::FromString(TEXT("Generating strip water...")));
    Progress.MakeDialog();
    Progress.EnterProgressFrame(1.0f, FText::FromString(TEXT("Replacing this generator's water")));
    ClearGeneratedWater();
    Progress.EnterProgressFrame(1.0f, FText::FromString(TEXT("Creating lakes and rivers")));
    CreateWaterActors(CachedData);

    LastBuildStamp = FString::Printf(
        TEXT("Water created %s | seed %d | %d rivers | %d lakes"),
        *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")),
        Seed, CachedData->Rivers.Num(), CachedData->Lakes.Num()
    );
    UE_LOG(LogTemp, Display, TEXT("Avenor strip water generated: %s"), *LastBuildStamp);
#endif
}

#if WITH_EDITOR
void AAvenorStripTerrainGenerator::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    InvalidateData();
    Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif
