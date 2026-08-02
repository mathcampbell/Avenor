#include "AvenorGeomorphTerrainGenerator.h"

#include "ActorFactories/ActorFactory.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "MeshPartition.h"
#include "MeshPartitionMeshView.h"
#include "Misc/ScopedSlowTask.h"
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#include "WaterBodyLakeActor.h"
#include "WaterBodyOceanActor.h"
#include "WaterBodyRiverActor.h"
#include "WaterSplineComponent.h"
#include "WaterSplineMetadata.h"

#include <queue>
#include <vector>

namespace UE::Avenor::Geomorph
{
static const FName GeneratedWaterTag(TEXT("AvenorGeomorphWater"));
static const FName LegacyGeneratedWaterTag(TEXT("AvenorGeneratedWater"));
static const FName ElevationChannel(TEXT("Elevation"));
static const FName SlopeChannel(TEXT("Slope"));
static const FName WetnessChannel(TEXT("Wetness"));
static const FName RiverChannel(TEXT("River"));
static const FName LakeChannel(TEXT("Lake"));
static const FName MountainChannel(TEXT("Mountain"));
static const FName MesaChannel(TEXT("Mesa"));
static const FName PlainChannel(TEXT("Plain"));

struct FMountainRange
{
    FVector2D Centre = FVector2D::ZeroVector;
    FVector2D Along = FVector2D(1.0, 0.0);
    FVector2D Across = FVector2D(0.0, 1.0);
    double HalfLength = 1.0;
    double HalfWidth = 1.0;
    double PeakSpacing = 1.0;
    double Relief = 0.0;
    double Phase = 0.0;
};

struct FMesaRegion
{
    FVector2D Centre = FVector2D::ZeroVector;
    FVector2D Axis = FVector2D(1.0, 0.0);
    double RadiusX = 1.0;
    double RadiusY = 1.0;
    double Relief = 0.0;
    double Phase = 0.0;
};

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
    double CatchmentArea = 0.0;
    FBox2D Bounds = FBox2D(ForceInit);
};

struct FPriorityCell
{
    double Height = 0.0;
    int32 Cell = INDEX_NONE;

    bool operator>(const FPriorityCell& Other) const
    {
        return Height > Other.Height;
    }
};

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
        double Ridge = 1.0 - FMath::Abs(
            Noise(Position, FrequencyScale, Offset)
        );
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
        EdgeDistance = FMath::Min(
            EdgeDistance,
            SegmentDistance(Point, A, B)
        );
        if ((A.Y > Point.Y) != (B.Y > Point.Y))
        {
            const double CrossingX = A.X +
                (Point.Y - A.Y) * (B.X - A.X) /
                    FMath::Max(
                        UE_DOUBLE_KINDA_SMALL_NUMBER,
                        FMath::Abs(B.Y - A.Y)
                    ) * FMath::Sign(B.Y - A.Y);
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
        const int32 SegmentCount = bClosed
            ? Points.Num()
            : Points.Num() - 1;
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
    const int32 SegmentCount = bClosed
        ? Input.Num()
        : Input.Num() - 1;
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
        FMath::CeilToInt(TotalLength / FMath::Max(1.0, Spacing)) +
            (bClosed ? 0 : 1)
    );
    TArray<FVector> Result;
    Result.Reserve(SampleCount);
    int32 SegmentIndex = 0;
    double SegmentStart = 0.0;
    for (int32 Sample = 0; Sample < SampleCount; ++Sample)
    {
        const double Distance = bClosed
            ? TotalLength * static_cast<double>(Sample) / SampleCount
            : TotalLength * static_cast<double>(Sample) /
                FMath::Max(1, SampleCount - 1);
        while (SegmentIndex + 1 < SegmentCount &&
               SegmentStart + SegmentLengths[SegmentIndex] < Distance)
        {
            SegmentStart += SegmentLengths[SegmentIndex];
            ++SegmentIndex;
        }
        const double Alpha = FMath::Clamp(
            (Distance - SegmentStart) /
                FMath::Max(1.0, SegmentLengths[SegmentIndex]),
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
    const double Amplitude = FMath::Min(
        TotalLength * 0.075,
        CellSize * 3.8
    ) * Strength * FMath::Lerp(0.12, 1.0, LowlandFraction);
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
            0.28 * FMath::Sin(
                2.0 * PI * Distance / (Wavelength * 2.37) + PhaseB
            );
        const FVector2D Offset = Normal * Amplitude * Fade * Wave;
        Points[Index].X = FMath::Clamp(
            Points[Index].X + Offset.X,
            Bounds.Min.X + CellSize,
            Bounds.Max.X - CellSize
        );
        Points[Index].Y = FMath::Clamp(
            Points[Index].Y + Offset.Y,
            Bounds.Min.Y + CellSize,
            Bounds.Max.Y - CellSize
        );
    }
}
} // namespace UE::Avenor::Geomorph

struct FAvenorGeomorphData
{
    FBox Bounds = FBox(ForceInit);
    int32 Columns = 0;
    int32 Rows = 0;
    double CellSize = 10000.0;
    TArray<double> Height;
    TArray<double> FilledHeight;
    TArray<double> Accumulation;
    TArray<double> Slope;
    TArray<double> MountainMask;
    TArray<double> MesaMask;
    TArray<double> PlainMask;
    TArray<int32> ReceiverA;
    TArray<int32> ReceiverB;
    TArray<double> ReceiverWeightA;
    TArray<int32> FillParent;
    TArray<int32> LakeIndex;
    TArray<UE::Avenor::Geomorph::FRiverReach> Rivers;
    TArray<UE::Avenor::Geomorph::FLakeBasin> Lakes;
    TArray<FVector> OceanBoundary;

    int32 Index(int32 X, int32 Y) const
    {
        return Y * Columns + X;
    }

    bool IsValid(int32 X, int32 Y) const
    {
        return X >= 0 && X < Columns && Y >= 0 && Y < Rows;
    }

    bool IsBoundary(int32 X, int32 Y) const
    {
        return X == 0 || Y == 0 || X == Columns - 1 || Y == Rows - 1;
    }

    FVector2D CellPosition(int32 Cell) const
    {
        const int32 X = Cell % Columns;
        const int32 Y = Cell / Columns;
        return FVector2D(
            Bounds.Min.X + (static_cast<double>(X) + 0.5) * CellSize,
            Bounds.Min.Y + (static_cast<double>(Y) + 0.5) * CellSize
        );
    }

    double CellAreaSquareKilometres() const
    {
        return CellSize * CellSize / 10000000000.0;
    }

    double SampleGrid(
        const TArray<double>& Values,
        const FVector2D& Position
    ) const
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

namespace UE::Avenor::Geomorph
{
static constexpr int32 NeighborX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
static constexpr int32 NeighborY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
static void ApplyThermalErosion(
    FAvenorGeomorphData& Data,
    int32 Iterations,
    double Strength,
    double TalusAngleDegrees
)
{
    if (Iterations <= 0 || Strength <= 0.0)
    {
        return;
    }
    const double TalusDrop = FMath::Tan(
        FMath::DegreesToRadians(TalusAngleDegrees)
    ) * Data.CellSize;
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
                    const int32 Neighbor = Data.Index(
                        X + NeighborX[Direction],
                        Y + NeighborY[Direction]
                    );
                    const double Distance =
                        (NeighborX[Direction] != 0 && NeighborY[Direction] != 0)
                        ? Data.CellSize * 1.4142135623730951
                        : Data.CellSize;
                    const double AllowedDrop = TalusDrop * Distance / Data.CellSize;
                    Excess[Direction] = FMath::Max(
                        0.0,
                        Data.Height[Cell] - Data.Height[Neighbor] - AllowedDrop
                    );
                    TotalExcess += Excess[Direction];
                }
                if (TotalExcess <= 0.0)
                {
                    continue;
                }
                const double Transfer = FMath::Min(
                    TotalExcess * Strength * 0.18,
                    Data.CellSize * 0.25
                );
                Delta[Cell] -= Transfer;
                for (int32 Direction = 0; Direction < 8; ++Direction)
                {
                    if (Excess[Direction] <= 0.0)
                    {
                        continue;
                    }
                    const int32 Neighbor = Data.Index(
                        X + NeighborX[Direction],
                        Y + NeighborY[Direction]
                    );
                    Delta[Neighbor] += Transfer *
                        Excess[Direction] / TotalExcess;
                }
            }
        }
        for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
        {
            Data.Height[Cell] += Delta[Cell];
        }
    }
}

static void PriorityFlood(
    FAvenorGeomorphData& Data,
    double Epsilon
)
{
    const int32 CellCount = Data.Height.Num();
    Data.FilledHeight = Data.Height;
    Data.FillParent.Init(INDEX_NONE, CellCount);
    TArray<bool> Visited;
    Visited.Init(false, CellCount);
    std::priority_queue<
        FPriorityCell,
        std::vector<FPriorityCell>,
        std::greater<FPriorityCell>
    > Queue;
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
            Data.FilledHeight[Neighbor] = FMath::Max(
                Data.Height[Neighbor],
                Current.Height + FMath::Max(0.0001, Epsilon)
            );
            Queue.push({Data.FilledHeight[Neighbor], Neighbor});
        }
    }
}

static int32 SteepestReceiver(
    const FAvenorGeomorphData& Data,
    int32 X,
    int32 Y
)
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
        const double Distance =
            (NeighborX[Direction] != 0 && NeighborY[Direction] != 0)
            ? Data.CellSize * 1.4142135623730951
            : Data.CellSize;
        const double CandidateSlope =
            (Data.FilledHeight[Cell] - Data.FilledHeight[Neighbor]) /
            Distance;
        if (CandidateSlope > BestSlope)
        {
            BestSlope = CandidateSlope;
            Best = Neighbor;
        }
    }
    return Best != INDEX_NONE ? Best : Data.FillParent[Cell];
}

static void BuildContinuousFlow(FAvenorGeomorphData& Data)
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
            const double DX = (
                Data.FilledHeight[Data.Index(X + 1, Y)] -
                Data.FilledHeight[Data.Index(X - 1, Y)]
            ) / (2.0 * Data.CellSize);
            const double DY = (
                Data.FilledHeight[Data.Index(X, Y + 1)] -
                Data.FilledHeight[Data.Index(X, Y - 1)]
            ) / (2.0 * Data.CellSize);
            Data.Slope[Cell] = FMath::Sqrt(DX * DX + DY * DY);
            double Angle = FMath::Atan2(-DY, -DX);
            if (Angle < 0.0)
            {
                Angle += 2.0 * PI;
            }
            const double Sector = Angle / (PI * 0.25);
            const int32 LowerDirection = FMath::FloorToInt(Sector) & 7;
            const int32 UpperDirection = (LowerDirection + 1) & 7;
            const double UpperWeight = Sector -
                static_cast<double>(FMath::FloorToInt(Sector));
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
            Data.ReceiverWeightA[Cell] = B == INDEX_NONE
                ? 1.0
                : 1.0 - UpperWeight;
        }
    }

    TArray<int32> Order;
    Order.SetNumUninitialized(CellCount);
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        Order[Cell] = Cell;
    }
    Order.Sort([&](int32 A, int32 B)
    {
        return Data.FilledHeight[A] > Data.FilledHeight[B];
    });
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
    FAvenorGeomorphData& Data,
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
            if (Data.Accumulation[Cell] < StreamStartArea ||
                Data.ReceiverA[Cell] == INDEX_NONE)
            {
                continue;
            }
            const double AreaFactor = FMath::Pow(
                Data.Accumulation[Cell] /
                    FMath::Max(0.01, StreamStartArea),
                0.42
            );
            const double SlopeFactor = FMath::Pow(
                FMath::Max(0.00001, Data.Slope[Cell]),
                0.72
            );
            Delta[Cell] = FMath::Min(
                Data.CellSize * 0.035,
                Strength * 760.0 * AreaFactor * SlopeFactor
            );
        }
        for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
        {
            Data.Height[Cell] -= Delta[Cell];
        }
    }
    PriorityFlood(Data, Epsilon);
    BuildContinuousFlow(Data);
}

static int32 PrimaryReceiver(
    const FAvenorGeomorphData& Data,
    int32 Cell
)
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
    return Smooth01(
        FMath::Loge(1.0 + FMath::Max(0.0, Area)) /
        FMath::Loge(1.0 + FMath::Max(0.01, MainRiverArea))
    );
}

struct FLakeCandidate
{
    TArray<int32> Cells;
    double SurfaceHeight = -TNumericLimits<double>::Max();
    double MaximumDepth = 0.0;
    double CatchmentArea = 0.0;
};

struct FLakeBoundaryEdge
{
    FIntPoint Start;
    FIntPoint End;
    FVector2D Position = FVector2D::ZeroVector;
    bool bUsed = false;
};

static TArray<FVector> TraceComponentBoundary(
    const FAvenorGeomorphData& Data,
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
        int32 Cell,
        int32 OutsideX,
        int32 OutsideY,
        const FIntPoint& Start,
        const FIntPoint& End)
    {
        FVector2D Position;
        if (Data.IsValid(OutsideX, OutsideY))
        {
            const int32 Outside = Data.Index(OutsideX, OutsideY);
            const double InsideHeight = Data.Height[Cell];
            const double OutsideHeight = Data.Height[Outside];
            const double Denominator = OutsideHeight - InsideHeight;
            const double Alpha = FMath::Abs(Denominator) > 1.0
                ? FMath::Clamp(
                    (SurfaceHeight - InsideHeight) / Denominator,
                    0.05,
                    0.95
                )
                : 0.5;
            Position = FMath::Lerp(
                Data.CellPosition(Cell),
                Data.CellPosition(Outside),
                Alpha
            );
        }
        else
        {
            Position = FVector2D(
                Data.Bounds.Min.X +
                    (Start.X + End.X) * 0.5 * Data.CellSize,
                Data.Bounds.Min.Y +
                    (Start.Y + End.Y) * 0.5 * Data.CellSize
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
            AddEdge(
                Cell,
                X,
                Y - 1,
                FIntPoint(X, Y),
                FIntPoint(X + 1, Y)
            );
        }
        if (X + 1 >= Data.Columns ||
            !Membership.Contains(Data.Index(X + 1, Y)))
        {
            AddEdge(
                Cell,
                X + 1,
                Y,
                FIntPoint(X + 1, Y),
                FIntPoint(X + 1, Y + 1)
            );
        }
        if (Y + 1 >= Data.Rows ||
            !Membership.Contains(Data.Index(X, Y + 1)))
        {
            AddEdge(
                Cell,
                X,
                Y + 1,
                FIntPoint(X + 1, Y + 1),
                FIntPoint(X, Y + 1)
            );
        }
        if (X == 0 || !Membership.Contains(Data.Index(X - 1, Y)))
        {
            AddEdge(
                Cell,
                X - 1,
                Y,
                FIntPoint(X, Y + 1),
                FIntPoint(X, Y)
            );
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
            for (int32 CandidateIndex = 0;
                 CandidateIndex < Edges.Num();
                 ++CandidateIndex)
            {
                if (!Edges[CandidateIndex].bUsed &&
                    Edges[CandidateIndex].Start == Edge.End)
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

    // Restore the original shoreline construction: the raster selects the
    // filled basin, but its contour is sampled where the continuous terrain
    // crosses the lake surface. Simplification must use world-space spacing,
    // not an arbitrary maximum point count: fixed-count reduction displaced
    // large shorelines much more severely than small ones.
    TArray<FVector> Reduced = ResamplePolyline(
        Boundary,
        FMath::Max(Data.CellSize, 3500.0),
        true
    );
    Boundary = ChaikinSmooth(Reduced, true, 2);
    return ResamplePolyline(
        Boundary,
        FMath::Max(Data.CellSize * 0.40, 2500.0),
        true
    );
}

static void ExtractLakes(
    FAvenorGeomorphData& Data,
    double MinimumCatchmentArea,
    double MinimumDepth,
    double MinimumBedDepth,
    double MaximumBedDepth,
    double MaximumArea,
    int32 MaximumCount,
    double MaximumCoverageFraction,
    double BankBlendWidth,
    double DepthRampWidth,
    double SurfaceInset
)
{
    Data.Lakes.Reset();
    Data.LakeIndex.Init(INDEX_NONE, Data.Height.Num());
    if (MaximumCount <= 0)
    {
        return;
    }
    TArray<bool> Candidate;
    Candidate.Init(false, Data.Height.Num());
    // Use the stable, meaningfully flooded part of a Priority-Flood
    // depression to decide lake membership. Including its 1-5% fill fringe
    // turns broad lowlands into lakes, removes those cells from river
    // extraction, and lets lake carving overwrite otherwise valid valleys.
    // TraceComponentBoundary independently interpolates the continuous
    // water-level crossing, so an organic shoreline does not require that
    // shallow raster fringe to be classified as lake interior.
    const double ShorelineFillThreshold = FMath::Max(
        50.0,
        MinimumDepth * 0.5
    );
    for (int32 Y = 1; Y + 1 < Data.Rows; ++Y)
    {
        for (int32 X = 1; X + 1 < Data.Columns; ++X)
        {
            const int32 Cell = Data.Index(X, Y);
            const double FillDepth =
                Data.FilledHeight[Cell] - Data.Height[Cell];
            Candidate[Cell] = FillDepth >= ShorelineFillThreshold;
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
            Basin.SurfaceHeight = FMath::Max(
                Basin.SurfaceHeight,
                Data.FilledHeight[Cell]
            );
            Basin.MaximumDepth = FMath::Max(
                Basin.MaximumDepth,
                Data.FilledHeight[Cell] - Data.Height[Cell]
            );
            Basin.CatchmentArea = FMath::Max(
                Basin.CatchmentArea,
                Data.Accumulation[Cell]
            );
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
                if (Candidate[Neighbor] && !Visited[Neighbor])
                {
                    Visited[Neighbor] = true;
                    Queue.Add(Neighbor);
                }
            }
        }
        const double Area = Basin.Cells.Num() *
            Data.CellAreaSquareKilometres();
        if (Basin.CatchmentArea >= MinimumCatchmentArea &&
            Basin.MaximumDepth >= MinimumDepth &&
            Area >= Data.CellAreaSquareKilometres() * 2.0 &&
            Area <= MaximumArea)
        {
            Basins.Add(MoveTemp(Basin));
        }
    }
    Basins.Sort([](const FLakeCandidate& A, const FLakeCandidate& B)
    {
        const double ScoreA = A.CatchmentArea * FMath::Sqrt(A.MaximumDepth);
        const double ScoreB = B.CatchmentArea * FMath::Sqrt(B.MaximumDepth);
        return ScoreA > ScoreB;
    });
    const double WorldArea = Data.Height.Num() *
        Data.CellAreaSquareKilometres();
    const double MaximumTotalLakeArea = WorldArea * FMath::Clamp(
        MaximumCoverageFraction,
        0.0,
        0.5
    );
    double AcceptedLakeArea = 0.0;
    for (int32 BasinIndex = 0;
         BasinIndex < Basins.Num() && Data.Lakes.Num() < MaximumCount;
         ++BasinIndex)
    {
        const FLakeCandidate& CandidateBasin = Basins[BasinIndex];
        const double BasinArea = CandidateBasin.Cells.Num() *
            Data.CellAreaSquareKilometres();
        if (BasinArea <= 0.0 ||
            AcceptedLakeArea + BasinArea > MaximumTotalLakeArea)
        {
            continue;
        }
        FLakeBasin Lake;
        Lake.SurfaceHeight = CandidateBasin.SurfaceHeight;
        Lake.MaximumDepth = FMath::Clamp(
            FMath::Max(CandidateBasin.MaximumDepth, MinimumBedDepth),
            MinimumBedDepth,
            FMath::Max(MinimumBedDepth, MaximumBedDepth)
        );
        Lake.BankBlendWidth = BankBlendWidth;
        Lake.DepthRampWidth = DepthRampWidth;
        Lake.CatchmentArea = CandidateBasin.CatchmentArea;
        Lake.Shoreline = TraceComponentBoundary(
            Data,
            CandidateBasin.Cells,
            Lake.SurfaceHeight
        );
        if (Lake.Shoreline.Num() < 4)
        {
            continue;
        }

        // Chaikin smoothing moves the final outline away from the original
        // flood-cell corners. Recalculate the rim from the raw terrain beneath
        // that finished outline instead of reusing the pre-smoothing fill
        // height.
        double MinimumShorelineHeight =
            TNumericLimits<double>::Max();
        for (const FVector& Point : Lake.Shoreline)
        {
            MinimumShorelineHeight = FMath::Min(
                MinimumShorelineHeight,
                Data.SampleGrid(Data.Height, FVector2D(Point))
            );
        }
        if (MinimumShorelineHeight < TNumericLimits<double>::Max())
        {
            Lake.ShorelineHeight = FMath::Min(
                Lake.SurfaceHeight,
                MinimumShorelineHeight
            );
        }
        else
        {
            Lake.ShorelineHeight = Lake.SurfaceHeight;
        }

        // The bowl exists in the generated terrain first. The visible lake is
        // then a single level plane lowered just below that terrain rim.
        Lake.SurfaceHeight = Lake.ShorelineHeight -
            FMath::Max(0.0, SurfaceInset);
        for (FVector& Point : Lake.Shoreline)
        {
            Point.Z = Lake.SurfaceHeight;
        }

        // A fixed ramp makes small lakes permanently shallow. Measure the
        // actual basin inradius and shorten only the inward depth ramp when
        // necessary, so every accepted lake reaches its configured bed depth
        // while the exterior shoreline blend remains broad and gentle.
        double BasinInradius = 0.0;
        for (int32 Cell : CandidateBasin.Cells)
        {
            double EdgeDistance = 0.0;
            if (IsInsidePolygon(
                    Data.CellPosition(Cell),
                    Lake.Shoreline,
                    &EdgeDistance
                ))
            {
                BasinInradius = FMath::Max(
                    BasinInradius,
                    EdgeDistance
                );
            }
        }
        const double MinimumRamp = FMath::Max(
            500.0,
            Data.CellSize * 0.15
        );
        const double BasinRamp = FMath::Max(
            MinimumRamp,
            BasinInradius * 0.65
        );
        Lake.DepthRampWidth = FMath::Clamp(
            FMath::Max(DepthRampWidth, BasinInradius * 0.35),
            MinimumRamp,
            BasinRamp
        );
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
        Data.Lakes.Add(MoveTemp(Lake));
    }
}

static void EnforceDownhill(TArray<FVector>& Points)
{
    if (Points.Num() < 2)
    {
        return;
    }
    for (int32 Index = 1; Index < Points.Num(); ++Index)
    {
        const double HorizontalDistance = FVector2D::Distance(
            FVector2D(Points[Index - 1]), FVector2D(Points[Index])
        );
        Points[Index].Z = FMath::Min(
            Points[Index].Z,
            Points[Index - 1].Z - HorizontalDistance * 0.0006
        );
    }
}

struct FRiverCandidate
{
    TArray<int32> Cells;
    double Score = 0.0;
};

static void ExtractRivers(
    FAvenorGeomorphData& Data,
    int32 Seed,
    double MountainStartArea,
    double LowlandStartArea,
    double MainRiverArea,
    double MinimumSystemLength,
    int32 JunctionOverlapCells,
    double HeadwaterWidth,
    double MainRiverWidth,
    double MaximumDepth,
    double HeadwaterSurfaceInset,
    double MainRiverSurfaceInset,
    double HeadwaterValleyWidth,
    double MainValleyWidth,
    double MaximumValleyDepth,
    double MeanderStrength,
    int32 MaximumReaches,
    bool bValleys,
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
        if (Data.LakeIndex.IsValidIndex(Cell) &&
            Data.LakeIndex[Cell] != INDEX_NONE)
        {
            continue;
        }
        const double MountainFraction = FMath::Clamp(
            Data.Slope[Cell] / 0.18,
            0.0,
            1.0
        );
        const double StartArea = FMath::Lerp(
            LowlandStartArea,
            MountainStartArea,
            MountainFraction
        );
        Channel[Cell] =
            Data.Accumulation[Cell] >= StartArea &&
            PrimaryReceiver(Data, Cell) != INDEX_NONE;
    }

    // Keep or reject an entire connected drainage system. Short segments
    // between nearby confluences must not disappear merely because that
    // individual reach is shorter than the overall river.
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
            SystemLength[FindSystemRoot(Cell)] += FVector2D::Distance(
                Data.CellPosition(Cell),
                Data.CellPosition(Receiver)
            );
        }
    }
    for (int32 Cell = 0; Cell < Data.Height.Num(); ++Cell)
    {
        if (Channel[Cell] &&
            SystemLength[FindSystemRoot(Cell)] < MinimumSystemLength)
        {
            Channel[Cell] = false;
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
    Starts.Sort([&](int32 A, int32 B)
    {
        return Data.FilledHeight[A] > Data.FilledHeight[B];
    });

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
            const int64 EdgeKey =
                (static_cast<int64>(Cell) << 32) |
                static_cast<uint32>(Receiver);
            if (UsedEdges.Contains(EdgeKey))
            {
                break;
            }
            UsedEdges.Add(EdgeKey);
            Cell = Receiver;
            if (Data.LakeIndex.IsValidIndex(Cell) &&
                Data.LakeIndex[Cell] != INDEX_NONE)
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
            // Adjacent WaterBodyRiver actors are capped independently by
            // Unreal. Extend an upstream reach a little way into the next
            // downstream reach so confluences cannot expose a visible gap.
            int32 OverlapCell = CandidateReach.Cells.Last();
            for (int32 OverlapIndex = 0;
                 OverlapIndex < JunctionOverlapCells;
                 ++OverlapIndex)
            {
                const int32 Receiver = PrimaryReceiver(Data, OverlapCell);
                if (Receiver == INDEX_NONE || !Channel[Receiver] ||
                    CandidateReach.Cells.Contains(Receiver))
                {
                    break;
                }
                CandidateReach.Cells.Add(Receiver);
                OverlapCell = Receiver;
            }
            const int32 EndCell = CandidateReach.Cells.Last();
            CandidateReach.Score =
                CandidateReach.Cells.Num() * Data.CellSize *
                FMath::Sqrt(FMath::Max(0.01, Data.Accumulation[EndCell])) *
                (1.0 + 2.0 * FMath::Clamp(
                    Data.Slope[Start] / 0.12,
                    0.0,
                    1.0
                ));
            Candidates.Add(MoveTemp(CandidateReach));
        }
    }
    Candidates.Sort([](const FRiverCandidate& A, const FRiverCandidate& B)
    {
        return A.Score > B.Score;
    });
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
            Points.Emplace(
                Position.X,
                Position.Y,
                Data.FilledHeight[Cell]
            );
            MeanSlope += Data.Slope[Cell];
        }
        MeanSlope /= CandidateReach.Cells.Num();
        Points = ChaikinSmooth(Points, false, 1);
        Points = ResamplePolyline(Points, Data.CellSize * 1.35, false);
        const double LowlandFraction = 1.0 - FMath::Clamp(
            MeanSlope / 0.12,
            0.0,
            1.0
        );
        AddBroadMeanders(
            Points,
            Data.CellSize,
            MeanderStrength,
            LowlandFraction,
            Seed ^ (ReachIndex * 0x45D9F3B),
            Data.Bounds
        );
        Points = ChaikinSmooth(Points, false, 2);
        Points = ResamplePolyline(
            Points,
            FMath::Max(2500.0, Data.CellSize * 0.42),
            false
        );
        for (FVector& Point : Points)
        {
            // Route on FilledHeight, but place the visible water below the
            // actual eroded terrain by an amount that grows smoothly with
            // drainage area. This is a local inset, never a world datum.
            const FVector2D Position(Point);
            const double LocalArea = Data.SampleGrid(
                Data.Accumulation,
                Position
            );
            const double LocalAlpha = DrainageScaleAlpha(
                LocalArea,
                MainRiverArea
            );
            const double SurfaceInset = FMath::Lerp(
                HeadwaterSurfaceInset,
                MainRiverSurfaceInset,
                LocalAlpha
            );
            // This is the actual geomorphic valley incision. Previously the
            // computed ValleyDepth was stored later but never affected the
            // centreline elevation, leaving a nearly level board with only a
            // shallow water-width notch in it.
            double ValleyInset = bValleys
                ? FMath::Lerp(
                    MaximumValleyDepth * 0.12,
                    MaximumValleyDepth,
                    LocalAlpha
                )
                : 0.0;
            if (bCanyons && LocalArea >= CanyonStartArea)
            {
                ValleyInset *= 1.2;
            }
            Point.Z = Data.SampleGrid(Data.Height, Position) -
                SurfaceInset - ValleyInset;
        }
        EnforceDownhill(Points);

        const int32 EndCell = CandidateReach.Cells.Last();
        const double Area = Data.Accumulation[EndCell];
        const double RiverAlpha = DrainageScaleAlpha(Area, MainRiverArea);
        FRiverReach River;
        River.Points = MoveTemp(Points);
        River.DrainageArea = Area;
        River.Width = FMath::Lerp(
            HeadwaterWidth,
            MainRiverWidth,
            RiverAlpha
        );
        River.Depth = FMath::Lerp(
            FMath::Max(120.0, MaximumDepth * 0.12),
            MaximumDepth,
            RiverAlpha
        );
        River.ValleyHalfWidth = bValleys
            ? FMath::Lerp(
                HeadwaterValleyWidth,
                MainValleyWidth,
                RiverAlpha
            )
            : River.Width * 2.2;
        River.ValleyDepth = bValleys
            ? FMath::Lerp(
                River.Depth * 1.4,
                MaximumValleyDepth,
                RiverAlpha
            )
            : River.Depth;
        if (bCanyons && Area >= CanyonStartArea && MeanSlope > 0.055)
        {
            River.ValleyHalfWidth *= 0.48;
            River.ValleyDepth = FMath::Max(
                River.ValleyDepth,
                MaximumValleyDepth * 1.35
            );
            River.CrossSectionExponent = 0.62;
        }
        else
        {
            River.CrossSectionExponent = FMath::Lerp(
                1.65,
                0.82,
                FMath::Clamp(MeanSlope / 0.16, 0.0, 1.0)
            );
        }
        for (const FVector& Point : River.Points)
        {
            River.Bounds += FVector2D(Point);
        }
        River.Bounds = River.Bounds.ExpandBy(River.ValleyHalfWidth);
        Data.Rivers.Add(MoveTemp(River));
    }
}
} // namespace UE::Avenor::Geomorph

double FAvenorGeomorphData::SampleHeight(const FVector2D& Position) const
{
    using namespace UE::Avenor::Geomorph;
    const double Underlying = SampleGrid(Height, Position);
    double Result = Underlying;
    bool bInsideLake = false;
    for (const FLakeBasin& Lake : Lakes)
    {
        if (!Lake.Bounds.IsInside(Position) || Lake.Shoreline.Num() < 3)
        {
            continue;
        }
        double EdgeDistance = 0.0;
        const bool bInside = IsInsidePolygon(
            Position,
            Lake.Shoreline,
            &EdgeDistance
        );
        const double Radius = FMath::Max(
            CellSize * 0.35,
            Lake.DepthRampWidth
        );
        if (bInside)
        {
            // The selected basin is a terrain bowl, not a water-shaped
            // puddle. At zero edge distance the terrain is exactly the water
            // surface; inward it falls to the bed and outward the bank rises
            // back toward the untouched land.
            const double DepthAlpha = Smooth01(
                FMath::Clamp(EdgeDistance / Radius, 0.0, 1.0)
            );
            // Retain a level water surface but give the submerged terrain a
            // broad, bounded variation. The radial depth ramp still controls
            // the shore, so this cannot create steps or lift the bed through
            // the water plane at the lake edge.
            const double BedNoise = Fbm(
                Position,
                FMath::Max(Radius * 2.0, CellSize * 2.0),
                FVector2D(4231.0, -8877.0),
                3
            );
            const double BedDepth = Lake.MaximumDepth * FMath::Clamp(
                0.86 + 0.14 * BedNoise,
                0.72,
                1.0
            );
            Result = FMath::Lerp(
                Lake.SurfaceHeight,
                Lake.SurfaceHeight - BedDepth,
                DepthAlpha
            );
            bInsideLake = true;
            break;
        }
        else if (EdgeDistance < Lake.BankBlendWidth)
        {
            // Blend the outside bank back to the untouched terrain. This
            // makes the first terrain ring outside the polygon meet the same
            // elevation as the water rather than exposing the Water mesh's
            // vertical skirt.
            const double BankAlpha = Smooth01(
                FMath::Clamp(
                    EdgeDistance / FMath::Max(1.0, Lake.BankBlendWidth),
                    0.0,
                    1.0
                )
            );
            Result = FMath::Lerp(
                Lake.SurfaceHeight,
                Underlying,
                BankAlpha
            );
        }
    }
    // A lake owns its enclosed basin. River reaches may terminate at or flow
    // into the shoreline, but must not overwrite the lake bowl after it has
    // been carved.
    if (bInsideLake)
    {
        return Result;
    }
    // Evaluate every reach first, then apply exactly one continuous river
    // cross-section. Selecting the strongest local influence prevents one
    // overlapping reach from carving through another reach's bank profile at
    // tributary junctions.
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
            const double Distance = SegmentDistance(
                Position,
                FVector2D(A),
                FVector2D(B),
                &Alpha
            );
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
        const double WaterHalfWidth = FMath::Max(
            100.0,
            River.Width * 0.5
        );
        double Influence = 0.0;
        double CarveTarget = Underlying;
        if (ClosestDistance <= WaterHalfWidth)
        {
            // Wet channel: deepest at the centreline and exactly equal to the
            // water surface at the visible mesh edge. This buries the Water
            // mesh's vertical skirt instead of exposing it above the land.
            const double ChannelAlpha = Smooth01(FMath::Clamp(
                ClosestDistance / WaterHalfWidth,
                0.0,
                1.0
            ));
            CarveTarget = FMath::Lerp(
                SurfaceHeight - River.Depth,
                SurfaceHeight,
                ChannelAlpha
            );
            Influence = 2.0 - ChannelAlpha;
        }
        else
        {
            // Dry valley/bank: begin at the exact same surface elevation as
            // the water edge, then return smoothly to the original landform.
            // The exponent retains broad lowland valleys and tighter upland
            // channels without introducing a discontinuity at either edge.
            const double ValleyAlpha = FMath::Clamp(
                (ClosestDistance - WaterHalfWidth) /
                    FMath::Max(
                        1.0,
                        River.ValleyHalfWidth - WaterHalfWidth
                    ),
                0.0,
                1.0
            );
            const double Shaped = FMath::Pow(
                ValleyAlpha,
                FMath::Max(0.2, River.CrossSectionExponent)
            );
            const double BankAlpha = Smooth01(Shaped);
            CarveTarget = FMath::Lerp(
                SurfaceHeight,
                Underlying,
                BankAlpha
            );
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
        // Assignment is required: Min(Underlying, Target) cannot make a low
        // terrain sample rise to meet the exact water-edge elevation and was
        // the remaining source of floating water skirts.
        Result = BestRiverTarget;
    }
    return Result;
}

float FAvenorGeomorphData::SampleChannel(
    FName Channel,
    const FVector2D& Position
) const
{
    using namespace UE::Avenor::Geomorph;
    if (Channel == ElevationChannel)
    {
        const double HeightValue = SampleHeight(Position);
        return static_cast<float>(FMath::Clamp(
            (HeightValue - Bounds.Min.Z) /
                FMath::Max(1.0, Bounds.GetSize().Z),
            0.0,
            1.0
        ));
    }
    if (Channel == SlopeChannel)
    {
        return static_cast<float>(FMath::Clamp(
            SampleGrid(Slope, Position) / 0.35,
            0.0,
            1.0
        ));
    }
    if (Channel == WetnessChannel)
    {
        return static_cast<float>(FMath::Clamp(
            FMath::Loge(1.0 + SampleGrid(Accumulation, Position)) / 6.0,
            0.0,
            1.0
        ));
    }
    if (Channel == MountainChannel)
    {
        return static_cast<float>(FMath::Clamp(
            SampleGrid(MountainMask, Position), 0.0, 1.0
        ));
    }
    if (Channel == MesaChannel)
    {
        return static_cast<float>(FMath::Clamp(
            SampleGrid(MesaMask, Position), 0.0, 1.0
        ));
    }
    if (Channel == PlainChannel)
    {
        return static_cast<float>(FMath::Clamp(
            SampleGrid(PlainMask, Position), 0.0, 1.0
        ));
    }
    if (Channel == LakeChannel)
    {
        for (const FLakeBasin& Lake : Lakes)
        {
            if (Lake.Bounds.IsInside(Position) &&
                IsInsidePolygon(Position, Lake.Shoreline))
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
                const double Distance = SegmentDistance(
                    Position,
                    FVector2D(River.Points[Index]),
                    FVector2D(River.Points[Index + 1])
                );
                if (Distance <= River.ValleyHalfWidth)
                {
                    return static_cast<float>(
                        1.0 - Distance / River.ValleyHalfWidth
                    );
                }
            }
        }
        return 0.0f;
    }
    return 0.0f;
}

namespace UE::Avenor::Geomorph
{
static TArray<FMountainRange> BuildMountainRanges(
    const FBox& Bounds,
    EAvenorWorldLongAxis LongAxis,
    int32 Seed,
    double DensityPer100Km,
    double RangeLength,
    double RangeWidth,
    double PeakSpacing,
    double Relief,
    double CentralExclusion,
    double EdgeBias
);

static TArray<FMesaRegion> BuildMesas(
    const FBox& Bounds,
    int32 Seed,
    double Coverage,
    double Scale,
    double Relief
);

static double EvaluateLandform(
    const FVector2D& Position,
    const FBox& Bounds,
    int32 Seed,
    const TArray<FMountainRange>& Mountains,
    const TArray<FMesaRegion>& Mesas,
    bool bPlains,
    bool bHills,
    bool bMountains,
    bool bMesas,
    bool bValleys,
    bool bOcean,
    double PlainsCoverage,
    double PlainsRelief,
    double HillsCoverage,
    double HillsScale,
    double HillsRelief,
    double ValleyCoverage,
    double ValleyScale,
    double ValleyDepth,
    int32 MesaTerraces,
    double SeaLevel,
    double OceanDepth,
    double CoastWidth,
    bool bOceanWidthEdges,
    bool bOceanLengthEnds,
    EAvenorWorldLongAxis LongAxis,
    double& OutMountain,
    double& OutMesa,
    double& OutPlain
);

static TSharedPtr<FAvenorGeomorphData> GenerateData(
    const AAvenorGeomorphTerrainGenerator& Generator
)
{
    const FBox Bounds = Generator.GetGenerationBounds();
    if (!Bounds.IsValid)
    {
        return nullptr;
    }
    TSharedPtr<FAvenorGeomorphData> Data =
        MakeShared<FAvenorGeomorphData>();
    Data->Bounds = Bounds;
    const FVector Size = Bounds.GetSize();
    double CellSize = FMath::Max(2500.0, Generator.AnalysisCellSize);
    int64 Columns = FMath::Max<int64>(
        2,
        FMath::CeilToInt(Size.X / CellSize)
    );
    int64 Rows = FMath::Max<int64>(
        2,
        FMath::CeilToInt(Size.Y / CellSize)
    );
    const int64 MaximumCells = FMath::Clamp<int64>(
        Generator.MaximumAnalysisCells,
        10000,
        2000000
    );
    if (Columns * Rows > MaximumCells)
    {
        CellSize *= FMath::Sqrt(
            static_cast<double>(Columns * Rows) /
            static_cast<double>(MaximumCells)
        );
        Columns = FMath::Max<int64>(
            2,
            FMath::CeilToInt(Size.X / CellSize)
        );
        Rows = FMath::Max<int64>(
            2,
            FMath::CeilToInt(Size.Y / CellSize)
        );
    }
    Data->Columns = static_cast<int32>(Columns);
    Data->Rows = static_cast<int32>(Rows);
    Data->CellSize = CellSize;
    const int32 CellCount = Data->Columns * Data->Rows;
    Data->Height.SetNumUninitialized(CellCount);
    Data->MountainMask.SetNumUninitialized(CellCount);
    Data->MesaMask.SetNumUninitialized(CellCount);
    Data->PlainMask.SetNumUninitialized(CellCount);

    const TArray<FMountainRange> Mountains = BuildMountainRanges(
        Bounds,
        Generator.LongAxis,
        Generator.Seed,
        Generator.bGenerateMountains
            ? Generator.MountainRangesPer100Km
            : 0.0,
        Generator.MountainRangeLength,
        Generator.MountainRangeWidth,
        Generator.MountainPeakSpacing,
        Generator.MountainRelief,
        Generator.CentralMountainExclusionHalfWidth,
        Generator.MountainEdgeBias
    );
    const TArray<FMesaRegion> Mesas = BuildMesas(
        Bounds,
        Generator.Seed,
        Generator.bGenerateMesas ? Generator.MesaCoverage : 0.0,
        Generator.MesaScale,
        Generator.MesaRelief
    );
    for (int32 Cell = 0; Cell < CellCount; ++Cell)
    {
        double Mountain = 0.0;
        double Mesa = 0.0;
        double Plain = 0.0;
        Data->Height[Cell] = EvaluateLandform(
            Data->CellPosition(Cell),
            Bounds,
            Generator.Seed,
            Mountains,
            Mesas,
            Generator.bGeneratePlains,
            Generator.bGenerateRollingHills,
            Generator.bGenerateMountains,
            Generator.bGenerateMesas,
            Generator.bGenerateValleys,
            Generator.bGenerateOcean,
            Generator.PlainsCoverage,
            Generator.PlainsRelief,
            Generator.HillsCoverage,
            Generator.HillsScale,
            Generator.HillsRelief,
            Generator.RegionalValleyCoverage,
            Generator.RegionalValleyScale,
            Generator.RegionalValleyDepth,
            Generator.MesaTerraces,
            Generator.SeaLevel,
            Generator.OceanDepth,
            Generator.CoastTransitionWidth,
            Generator.bOceanAlongWidthEdges,
            Generator.bOceanAtLengthEnds,
            Generator.LongAxis,
            Mountain,
            Mesa,
            Plain
        );
        Data->MountainMask[Cell] = Mountain;
        Data->MesaMask[Cell] = Mesa;
        Data->PlainMask[Cell] = Plain;
    }

    ApplyThermalErosion(
        *Data,
        Generator.ThermalErosionIterations,
        Generator.ThermalErosionStrength,
        Generator.TalusAngleDegrees
    );
    ApplyStreamPowerErosion(
        *Data,
        Generator.StreamPowerIterations,
        Generator.StreamPowerStrength,
        FMath::Min(
            Generator.MountainStreamStartArea,
            Generator.LowlandStreamStartArea
        ),
        Generator.DrainageEpsilon
    );
    if (Generator.bGenerateLakes)
    {
        ExtractLakes(
            *Data,
            Generator.MinimumLakeCatchmentArea,
            Generator.MinimumLakeDepth,
            Generator.MinimumLakeBedDepth,
            Generator.MaximumLakeBedDepth,
            Generator.MaximumLakeArea,
            Generator.MaximumLakeCount,
            Generator.MaximumLakeCoverageFraction,
            Generator.LakeBankBlendWidth,
            Generator.LakeDepthRampWidth,
            Generator.LakeSurfaceInset
        );
    }
    else
    {
        Data->LakeIndex.Init(INDEX_NONE, CellCount);
    }
    if (Generator.bGenerateRivers)
    {
        ExtractRivers(
            *Data,
            Generator.Seed,
            Generator.MountainStreamStartArea,
            Generator.LowlandStreamStartArea,
            Generator.MainRiverArea,
            Generator.MinimumRiverSystemLength,
            Generator.JunctionOverlapCells,
            Generator.HeadwaterWidth,
            Generator.MainRiverWidth,
            Generator.MaximumRiverDepth,
            Generator.HeadwaterSurfaceInset,
            Generator.MainRiverSurfaceInset,
            Generator.HeadwaterValleyHalfWidth,
            Generator.MainValleyHalfWidth,
            Generator.MaximumValleyDepth,
            Generator.LowlandMeanderStrength,
            Generator.MaximumRiverReaches,
            Generator.bGenerateValleys,
            Generator.bGenerateCanyons,
            Generator.CanyonStartArea
        );
    }
    if (Generator.bGenerateOcean)
    {
        const double Inset = FMath::Clamp(
            Generator.CoastTransitionWidth,
            CellSize,
            FMath::Min(Size.X, Size.Y) * 0.42
        );
        TArray<FVector> Boundary = {
            FVector(Bounds.Min.X + Inset, Bounds.Min.Y + Inset, Generator.SeaLevel),
            FVector(Bounds.Max.X - Inset, Bounds.Min.Y + Inset, Generator.SeaLevel),
            FVector(Bounds.Max.X - Inset, Bounds.Max.Y - Inset, Generator.SeaLevel),
            FVector(Bounds.Min.X + Inset, Bounds.Max.Y - Inset, Generator.SeaLevel)
        };
        Boundary = ChaikinSmooth(Boundary, true, 2);
        Data->OceanBoundary = ResamplePolyline(
            Boundary,
            FMath::Max(CellSize, 5000.0),
            true
        );
    }
    return Data;
}

class FGeomorphTerrainOp final
    : public UE::MeshPartition::IModifierBackgroundOp
{
public:
    explicit FGeomorphTerrainOp(FName Name)
        : IModifierBackgroundOp(Name)
    {
    }

    virtual void GetInstancesInBounds(
        const FBox& InBounds,
        TArray<FInstanceInfo>& OutInstances
    ) const override
    {
        if (!WorldBounds.Intersect(InBounds))
        {
            return;
        }

        // AddDefaultInstanceIfIntersects declares only a vertex-position
        // write. This modifier also writes the generated classification
        // weights, so describe both operations before MeshPartition creates
        // the restricted FMeshView used on the background worker.
        FInstanceInfo& Instance = OutInstances.AddDefaulted_GetRef();
        Instance.InstanceID = 0;
        Instance.Bounds = WorldBounds;
        Instance.ReadViewComponents =
            UE::MeshPartition::EMeshViewComponents::VertexPos;
        Instance.WriteViewComponents = static_cast<
            UE::MeshPartition::EMeshViewComponents
        >(
            UE::MeshPartition::EMeshViewComponents::VertexPos |
            UE::MeshPartition::EMeshViewComponents::VertexAttributeWeight
        );
        Instance.UsedChannels = {
            ElevationChannel,
            SlopeChannel,
            WetnessChannel,
            RiverChannel,
            LakeChannel,
            MountainChannel,
            MesaChannel,
            PlainChannel
        };
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
            FVector3d WorldPosition = MeshTransform.TransformPosition(
                MeshView.GetVertexPos(Vertex)
            );
            const FVector2D XY(WorldPosition.X, WorldPosition.Y);
            WorldPosition.Z = BaseWorldZ + Data->SampleHeight(XY);
            MeshView.SetVertexPos(
                Vertex,
                MeshTransform.InverseTransformPosition(WorldPosition)
            );
            MeshView.SetVertexAttributeWeight(
                ElevationChannel,
                Vertex,
                Data->SampleChannel(ElevationChannel, XY)
            );
            MeshView.SetVertexAttributeWeight(
                SlopeChannel,
                Vertex,
                Data->SampleChannel(SlopeChannel, XY)
            );
            MeshView.SetVertexAttributeWeight(
                WetnessChannel,
                Vertex,
                Data->SampleChannel(WetnessChannel, XY)
            );
            MeshView.SetVertexAttributeWeight(
                RiverChannel,
                Vertex,
                Data->SampleChannel(RiverChannel, XY)
            );
            MeshView.SetVertexAttributeWeight(
                LakeChannel,
                Vertex,
                Data->SampleChannel(LakeChannel, XY)
            );
            MeshView.SetVertexAttributeWeight(
                MountainChannel,
                Vertex,
                Data->SampleChannel(MountainChannel, XY)
            );
            MeshView.SetVertexAttributeWeight(
                MesaChannel,
                Vertex,
                Data->SampleChannel(MesaChannel, XY)
            );
            MeshView.SetVertexAttributeWeight(
                PlainChannel,
                Vertex,
                Data->SampleChannel(PlainChannel, XY)
            );
        }
    }

    virtual bool DisableDDCWrite() const override
    {
        return false;
    }

    static FGuid Version()
    {
        return FGuid(TEXT("39d91872-8c57-4fbb-9dc0-78cc2e0a5d11"));
    }

    FBox WorldBounds = FBox(ForceInit);
    double BaseWorldZ = 0.0;
    TSharedPtr<const FAvenorGeomorphData> Data;
};

template<typename TWaterActor>
static TWaterActor* SpawnWaterActor(UWorld& World, const FString& Label)
{
    UActorFactory* Factory = GEditor
        ? GEditor->FindActorFactoryForActorClass(TWaterActor::StaticClass())
        : nullptr;
    if (!Factory)
    {
        return nullptr;
    }
    TWaterActor* Actor = Cast<TWaterActor>(Factory->CreateActor(
        TWaterActor::StaticClass(),
        World.GetCurrentLevel(),
        FTransform::Identity
    ));
    if (Actor)
    {
        Actor->SetActorLabel(Label);
        Actor->SetFolderPath(TEXT("Avenor/Generated/GeomorphWater"));
        Actor->Tags.AddUnique(GeneratedWaterTag);
    }
    return Actor;
}

static void ConfigureExactSpline(
    UWaterSplineComponent& Spline,
    const TArray<FVector>& Points,
    bool bClosed
)
{
    Spline.SetSplinePoints(
        Points,
        ESplineCoordinateSpace::World,
        false
    );
    Spline.SetClosedLoop(bClosed, false);
    for (int32 Index = 0;
         Index < Spline.GetNumberOfSplinePoints();
         ++Index)
    {
        Spline.SetSplinePointType(
            Index,
            ESplinePointType::Linear,
            false
        );
    }
    Spline.UpdateSpline();
}

static void ConfigureRiverSpline(
    UWaterSplineComponent& Spline,
    const TArray<FVector>& Points,
    double FullWidth,
    double Depth
)
{
    ConfigureExactSpline(Spline, Points, false);
    UWaterSplineMetadata* Metadata = Cast<UWaterSplineMetadata>(
        Spline.GetSplinePointsMetadata()
    );
    if (Metadata)
    {
        Metadata->Fixup(Spline.GetNumberOfSplinePoints(), &Spline);
        // RiverWidth metadata is the complete bank-to-bank width. The
        // terrain sampler converts this same value to a half-width only for
        // its centreline-distance calculation. Supplying half here made the
        // rendered water narrower than the channel carved for it.
        const float MetadataWidth = static_cast<float>(
            FMath::Max(100.0, FullWidth)
        );
        const float WaterDepth = static_cast<float>(FMath::Max(1.0, Depth));
        for (int32 Index = 0;
             Index < Spline.GetNumberOfSplinePoints();
             ++Index)
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
} // namespace UE::Avenor::Geomorph

using namespace UE::Avenor::Geomorph;

TArray<FBox> UAvenorGeomorphTerrainModifier::ComputeBounds() const
{
    const AAvenorGeomorphTerrainGenerator* Generator =
        Cast<AAvenorGeomorphTerrainGenerator>(GetOwner());
    if (!Generator)
    {
        return {};
    }
    const FBox GenerationBounds = Generator->GetGenerationBounds();
    return GenerationBounds.IsValid
        ? TArray<FBox>{GenerationBounds}
        : TArray<FBox>{};
}

TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
UAvenorGeomorphTerrainModifier::CreateBackgroundOp(
    UE::MeshPartition::EBuildType BuildType
) const
{
    (void)BuildType;
    TSharedPtr<FGeomorphTerrainOp> Op =
        MakeShared<FGeomorphTerrainOp>(GetFName());
    const AAvenorGeomorphTerrainGenerator* Generator =
        Cast<AAvenorGeomorphTerrainGenerator>(GetOwner());
    if (Generator)
    {
        Op->WorldBounds = Generator->GetGenerationBounds();
        Op->BaseWorldZ = Generator->GetActorLocation().Z;
        Op->Data = Generator->GetOrCreateData();
    }
    return Op;
}

FGuid UAvenorGeomorphTerrainModifier::GetCodeVersionKey() const
{
    return FGeomorphTerrainOp::Version();
}

AAvenorGeomorphTerrainGenerator::AAvenorGeomorphTerrainGenerator()
{
    PrimaryActorTick.bCanEverTick = false;
    SetIsSpatiallyLoaded(false);
    TerrainModifier = CreateDefaultSubobject<UAvenorGeomorphTerrainModifier>(
        TEXT("GeomorphTerrain")
    );
    SetRootComponent(TerrainModifier);
}

FBox AAvenorGeomorphTerrainGenerator::GetGenerationBounds() const
{
    const FVector Centre = GetActorLocation();
    const FVector Extent(
        FMath::Max(10000.0, WorldSize.X) * 0.5,
        FMath::Max(10000.0, WorldSize.Y) * 0.5,
        500000.0
    );
    return FBox(Centre - Extent, Centre + Extent);
}

TSharedPtr<const FAvenorGeomorphData>
AAvenorGeomorphTerrainGenerator::GetOrCreateData() const
{
    FScopeLock Lock(&DataMutex);
    if (!CachedData)
    {
        CachedData = GenerateData(*this);
    }
    return CachedData;
}

void AAvenorGeomorphTerrainGenerator::InvalidateData()
{
    FScopeLock Lock(&DataMutex);
    CachedData.Reset();
}

void AAvenorGeomorphTerrainGenerator::ClearGeneratedWater()
{
#if WITH_EDITOR
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    TArray<AWaterBody*> ToDelete;
    for (TActorIterator<AWaterBody> It(World); It; ++It)
    {
        if (It->Tags.Contains(GeneratedWaterTag) ||
            It->Tags.Contains(LegacyGeneratedWaterTag))
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

void AAvenorGeomorphTerrainGenerator::CreateWaterActors(
    const TSharedPtr<const FAvenorGeomorphData>& Data
)
{
#if WITH_EDITOR
    UWorld* World = GetWorld();
    if (!World || !Data)
    {
        return;
    }
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
            *World,
            TEXT("Avenor_Geomorph_Ocean")
        ))
        {
            DisableSecondaryTerrainCarving(*Ocean);
            ConfigureExactSpline(
                *Ocean->GetWaterBodyComponent()->GetWaterSpline(),
                ToWorldPoints(Data->OceanBoundary),
                true
            );
            Ocean->PostEditChange();
            DisableSecondaryTerrainCarving(*Ocean);
        }
    }
    for (int32 Index = 0; Index < Data->Lakes.Num(); ++Index)
    {
        // ExtractLakes establishes one level shoreline from the natural rim
        // and SampleHeight carves the terrain against that same value. Do not
        // derive a second actor-only height here: that permits the visible
        // water and the generated shore to disagree after interpolation.
        TArray<FVector> LakePoints = ToWorldPoints(
            Data->Lakes[Index].Shoreline
        );
        if (AWaterBodyLake* Lake = SpawnWaterActor<AWaterBodyLake>(
            *World,
            FString::Printf(TEXT("Avenor_Geomorph_Lake_%02d"), Index + 1)
        ))
        {
            DisableSecondaryTerrainCarving(*Lake);
            ConfigureExactSpline(
                *Lake->GetWaterBodyComponent()->GetWaterSpline(),
                LakePoints,
                true
            );
            Lake->PostEditChange();
            // PostEditChange can recreate/synchronise the Water spline.
            // Reapply the exact generated shoreline afterwards so rendering
            // and terrain carving continue to consume identical geometry.
            ConfigureExactSpline(
                *Lake->GetWaterBodyComponent()->GetWaterSpline(),
                LakePoints,
                true
            );
            Lake->GetWaterBodyComponent()->GetWaterSpline()
                ->K2_SynchronizeAndBroadcastDataChange();
            DisableSecondaryTerrainCarving(*Lake);
        }
    }
    for (int32 Index = 0; Index < Data->Rivers.Num(); ++Index)
    {
        TArray<FVector> RiverPoints = ToWorldPoints(
            Data->Rivers[Index].Points
        );
        for (FVector& Point : RiverPoints)
        {
            Point.Z -= FMath::Max(0.0, RiverWaterMeshInset);
        }
        if (AWaterBodyRiver* River = SpawnWaterActor<AWaterBodyRiver>(
            *World,
            FString::Printf(TEXT("Avenor_Geomorph_River_%03d"), Index + 1)
        ))
        {
            DisableSecondaryTerrainCarving(*River);
            ConfigureRiverSpline(
                *River->GetWaterBodyComponent()->GetWaterSpline(),
                RiverPoints,
                Data->Rivers[Index].Width,
                Data->Rivers[Index].Depth
            );
            River->PostEditChange();
            // WaterBody synchronisation may restore its default width/depth
            // curves. Apply the generated values after that synchronisation,
            // otherwise the visible sheet can be much wider than its carve.
            ConfigureRiverSpline(
                *River->GetWaterBodyComponent()->GetWaterSpline(),
                RiverPoints,
                Data->Rivers[Index].Width,
                Data->Rivers[Index].Depth
            );
            DisableSecondaryTerrainCarving(*River);
        }
    }
#endif
}

void AAvenorGeomorphTerrainGenerator::RegenerateTerrainAndWater()
{
#if WITH_EDITOR
    FScopedSlowTask Progress(
        4.0f,
        FText::FromString(TEXT("Generating geomorphic terrain..."))
    );
    Progress.MakeDialog();
    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Building landforms and erosion"))
    );
    InvalidateData();
    const TSharedPtr<const FAvenorGeomorphData> Data = GetOrCreateData();
    if (!Data)
    {
        return;
    }
    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Replacing generated water"))
    );
    ClearGeneratedWater();
    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Creating exact water geometry"))
    );
    CreateWaterActors(Data);
    Progress.EnterProgressFrame(
        1.0f,
        FText::FromString(TEXT("Registering Mesh Partition modifier"))
    );
    if (TerrainModifier)
    {
        TerrainModifier->SetAffectedMeshPartition(nullptr);
        TerrainModifier->BP_SetAffectedMegaMesh(
            Cast<UE::MeshPartition::AMeshPartition>(MeshPartitionActor)
        );
    }
    UE_LOG(
        LogTemp,
        Display,
        TEXT(
            "Avenor geomorph generated %d cells at %.0f cm, "
            "%d river reaches and %d lake basins."
        ),
        Data->Height.Num(),
        Data->CellSize,
        Data->Rivers.Num(),
        Data->Lakes.Num()
    );
#endif
}

#if WITH_EDITOR
void AAvenorGeomorphTerrainGenerator::PostEditChangeProperty(
    FPropertyChangedEvent& PropertyChangedEvent
)
{
    InvalidateData();
    Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

namespace UE::Avenor::Geomorph
{
static TArray<FMountainRange> BuildMountainRanges(
    const FBox& Bounds,
    EAvenorWorldLongAxis LongAxis,
    int32 Seed,
    double DensityPer100Km,
    double RangeLength,
    double RangeWidth,
    double PeakSpacing,
    double Relief,
    double CentralExclusion,
    double EdgeBias
)
{
    const FVector Size = Bounds.GetSize();
    const double LongLength = LongAxis == EAvenorWorldLongAxis::X
        ? Size.X
        : Size.Y;
    const int32 Count = FMath::Clamp(
        FMath::RoundToInt(
            FMath::Max(0.0, DensityPer100Km) * LongLength / 10000000.0
        ),
        DensityPer100Km > 0.0 ? 1 : 0,
        64
    );
    const FVector2D Centre(Bounds.GetCenter());
    const double AcrossExtent = LongAxis == EAvenorWorldLongAxis::X
        ? Size.Y * 0.5
        : Size.X * 0.5;
    FRandomStream Random(Seed ^ 0x51A7F00D);
    TArray<FMountainRange> Ranges;
    Ranges.Reserve(Count);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const double Side = Random.FRand() < 0.5 ? -1.0 : 1.0;
        const double MinimumAcross = FMath::Min(
            AcrossExtent * 0.9,
            FMath::Max(CentralExclusion, RangeWidth * 0.55)
        );
        const double UniformAcross = Random.FRandRange(
            MinimumAcross,
            FMath::Max(MinimumAcross, AcrossExtent * 0.88)
        );
        const double BiasedAcross = FMath::Lerp(
            UniformAcross,
            FMath::Lerp(MinimumAcross, AcrossExtent * 0.88, FMath::Sqrt(Random.FRand())),
            FMath::Clamp(EdgeBias, 0.0, 1.0)
        ) * Side;
        const double AlongValue = Random.FRandRange(
            -LongLength * 0.43,
            LongLength * 0.43
        );
        const double Angle = Random.FRandRange(-0.42, 0.42);
        FVector2D LongDirection = LongAxis == EAvenorWorldLongAxis::X
            ? FVector2D(1.0, 0.0)
            : FVector2D(0.0, 1.0);
        FVector2D AcrossDirection = Rotate90(LongDirection);
        FVector2D Along =
            LongDirection * FMath::Cos(Angle) +
            AcrossDirection * FMath::Sin(Angle);
        Along.Normalize();
        FMountainRange Range;
        Range.Centre = Centre + LongDirection * AlongValue +
            AcrossDirection * BiasedAcross;
        Range.Along = Along;
        Range.Across = Rotate90(Along);
        Range.HalfLength = RangeLength * Random.FRandRange(0.72, 1.28) * 0.5;
        Range.HalfWidth = RangeWidth * Random.FRandRange(0.76, 1.24) * 0.5;
        Range.PeakSpacing = PeakSpacing * Random.FRandRange(0.78, 1.22);
        Range.Relief = Relief * Random.FRandRange(0.76, 1.18);
        Range.Phase = Random.FRandRange(-PI, PI);
        Ranges.Add(Range);
    }
    return Ranges;
}

static TArray<FMesaRegion> BuildMesas(
    const FBox& Bounds,
    int32 Seed,
    double Coverage,
    double Scale,
    double Relief
)
{
    const FVector Size = Bounds.GetSize();
    const double Area = Size.X * Size.Y;
    const int32 Count = FMath::Clamp(
        FMath::RoundToInt(
            FMath::Max(0.0, Coverage) * Area /
                FMath::Max(1.0, PI * Scale * Scale)
        ),
        Coverage > 0.0 ? 1 : 0,
        128
    );
    FRandomStream Random(Seed ^ 0x4D35A123);
    TArray<FMesaRegion> Mesas;
    Mesas.Reserve(Count);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        FMesaRegion Mesa;
        Mesa.Centre = FVector2D(
            Random.FRandRange(Bounds.Min.X, Bounds.Max.X),
            Random.FRandRange(Bounds.Min.Y, Bounds.Max.Y)
        );
        const double Angle = Random.FRandRange(-PI, PI);
        Mesa.Axis = FVector2D(FMath::Cos(Angle), FMath::Sin(Angle));
        Mesa.RadiusX = Scale * Random.FRandRange(0.65, 1.45);
        Mesa.RadiusY = Scale * Random.FRandRange(0.45, 1.05);
        Mesa.Relief = Relief * Random.FRandRange(0.7, 1.25);
        Mesa.Phase = Random.FRandRange(-PI, PI);
        Mesas.Add(Mesa);
    }
    return Mesas;
}

static double EvaluateLandform(
    const FVector2D& Position,
    const FBox& Bounds,
    int32 Seed,
    const TArray<FMountainRange>& Mountains,
    const TArray<FMesaRegion>& Mesas,
    bool bPlains,
    bool bHills,
    bool bMountains,
    bool bMesas,
    bool bValleys,
    bool bOcean,
    double PlainsCoverage,
    double PlainsRelief,
    double HillsCoverage,
    double HillsScale,
    double HillsRelief,
    double ValleyCoverage,
    double ValleyScale,
    double ValleyDepth,
    int32 MesaTerraces,
    double SeaLevel,
    double OceanDepth,
    double CoastWidth,
    bool bOceanWidthEdges,
    bool bOceanLengthEnds,
    EAvenorWorldLongAxis LongAxis,
    double& OutMountain,
    double& OutMesa,
    double& OutPlain
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
    const double Regional = 0.5 + 0.5 * Fbm(
        Warped, 1300000.0, SeedOffset + FVector2D(319.0, 557.0), 4
    );
    OutPlain = bPlains
        ? 1.0 - Smooth01(
            (Regional - FMath::Clamp(PlainsCoverage, 0.0, 0.95)) / 0.22
        )
        : 0.0;
    const double Base = Fbm(
        Warped, 2200000.0, SeedOffset + FVector2D(101.0, 43.0), 4
    ) * (bPlains ? PlainsRelief : 0.0);
    // Hills need their own regional selector. Reusing Regional made a seed
    // capable of classifying nearly the entire short test world as plains,
    // which suppressed the requested rolling relief everywhere at once.
    const double HillRegion = 0.5 + 0.5 * Fbm(
        Warped,
        FMath::Max(HillsScale * 2.8, 650000.0),
        SeedOffset + FVector2D(811.0, 463.0),
        3,
        0.55,
        2.01
    );
    const double HillThreshold =
        1.0 - FMath::Clamp(HillsCoverage, 0.0, 1.0);
    const double HillMask = bHills
        ? Smooth01((HillRegion - (HillThreshold - 0.16)) / 0.32) *
            (1.0 - OutPlain * 0.18)
        : 0.0;
    const double HillMacro = Fbm(
        Warped,
        FMath::Max(HillsScale, 1.0),
        SeedOffset + FVector2D(887.0, 157.0),
        5,
        0.54,
        2.03
    );
    const double HillDetail = Fbm(
        Warped,
        FMath::Max(HillsScale * 0.43, 1.0),
        SeedOffset + FVector2D(421.0, 997.0),
        3,
        0.5,
        2.07
    );
    const double ShapedHillMacro = FMath::Sign(HillMacro) * FMath::Pow(
        FMath::Abs(HillMacro),
        0.72
    );
    const double Hills = bHills
        ? (ShapedHillMacro * 0.78 + HillDetail * 0.22) *
            HillsRelief * HillMask
        : 0.0;

    double MountainHeight = 0.0;
    OutMountain = 0.0;
    if (bMountains)
    {
        for (const FMountainRange& Range : Mountains)
        {
            const FVector2D Delta = Warped - Range.Centre;
            const double Along = FVector2D::DotProduct(Delta, Range.Along);
            const double Across = FVector2D::DotProduct(Delta, Range.Across);
            const double AlongNorm = FMath::Abs(Along) /
                FMath::Max(1.0, Range.HalfLength);
            const double AcrossNorm = FMath::Abs(Across) /
                FMath::Max(1.0, Range.HalfWidth);
            const double CoreEnvelope =
                Smooth01(1.0 - AlongNorm) * Smooth01(1.0 - AcrossNorm);
            const double SkirtEnvelope =
                Smooth01(1.0 - AlongNorm / 1.35) *
                Smooth01(1.0 - AcrossNorm / 1.8);
            if (SkirtEnvelope <= 0.0)
            {
                continue;
            }
            const double Peaks = 0.38 + 0.62 * FMath::Pow(
                FMath::Max(
                    0.0,
                    0.5 + 0.5 * FMath::Cos(
                        Along / FMath::Max(1.0, Range.PeakSpacing) *
                            2.0 * PI + Range.Phase
                    )
                ),
                1.7
            );
            const double Ridge = RidgedFbm(
                Warped,
                Range.PeakSpacing * 1.15,
                SeedOffset + FVector2D(Range.Phase * 997.0, 313.0),
                5
            );
            const double Core = Range.Relief * CoreEnvelope *
                Peaks * FMath::Lerp(0.46, 1.0, Ridge);
            const double Foothills = Range.Relief * 0.22 *
                FMath::Max(0.0, SkirtEnvelope - CoreEnvelope * 0.65) *
                (0.35 + 0.65 * Ridge);
            MountainHeight = FMath::Max(MountainHeight, Core + Foothills);
            OutMountain = FMath::Max(OutMountain, SkirtEnvelope);
        }
    }

    double MesaHeight = 0.0;
    OutMesa = 0.0;
    if (bMesas)
    {
        for (const FMesaRegion& Mesa : Mesas)
        {
            const FVector2D Delta = Warped - Mesa.Centre;
            const FVector2D Side = Rotate90(Mesa.Axis);
            const double X = FVector2D::DotProduct(Delta, Mesa.Axis) /
                FMath::Max(1.0, Mesa.RadiusX);
            const double Y = FVector2D::DotProduct(Delta, Side) /
                FMath::Max(1.0, Mesa.RadiusY);
            const double Radius = FMath::Sqrt(X * X + Y * Y);
            if (Radius >= 1.35)
            {
                continue;
            }
            const double Irregularity = Fbm(
                Position,
                FMath::Max(Mesa.RadiusX, Mesa.RadiusY) * 0.42,
                SeedOffset + FVector2D(Mesa.Phase * 431.0, 211.0),
                3
            ) * 0.13;
            const double Edge = Radius + Irregularity;
            const double Envelope = Smooth01((1.24 - Edge) / 0.28);
            double Terrace = Envelope;
            if (MesaTerraces > 1)
            {
                Terrace = FMath::Floor(
                    Envelope * MesaTerraces + 0.35
                ) / MesaTerraces;
                Terrace = FMath::Lerp(Envelope, Terrace, 0.72);
            }
            MesaHeight = FMath::Max(
                MesaHeight,
                Mesa.Relief * Terrace
            );
            OutMesa = FMath::Max(OutMesa, Envelope);
        }
    }

    // These broad valleys exist in the initial landform, before erosion and
    // flow accumulation. Hydrology therefore follows them instead of valleys
    // appearing only as a cosmetic widening around an extracted river.
    double RegionalValleyHeight = 0.0;
    if (bValleys && ValleyCoverage > 0.0 && ValleyDepth > 0.0)
    {
        const double ValleyRegion = 0.5 + 0.5 * Fbm(
            Warped,
            FMath::Max(ValleyScale * 2.35, 1.0),
            SeedOffset + FVector2D(613.0, 109.0),
            3,
            0.56,
            2.0
        );
        const double ValleyThreshold =
            1.0 - FMath::Clamp(ValleyCoverage, 0.0, 1.0);
        const double RegionMask = Smooth01(
            (ValleyRegion - (ValleyThreshold - 0.18)) / 0.36
        );
        const double ValleyNetwork = RidgedFbm(
            Warped,
            FMath::Max(ValleyScale, 1.0),
            SeedOffset + FVector2D(229.0, 853.0),
            4
        );
        const double ValleyShape = FMath::Pow(
            Smooth01((ValleyNetwork - 0.52) / 0.36),
            1.2
        );
        const double MountainProtection = 1.0 - OutMountain * 0.35;
        RegionalValleyHeight = -ValleyDepth * RegionMask *
            ValleyShape * MountainProtection;
    }

    double Height = Base + Hills + RegionalValleyHeight +
        MountainHeight + MesaHeight;
    if (bOcean)
    {
        const double WidthDistance = LongAxis == EAvenorWorldLongAxis::X
            ? FMath::Min(Position.Y - Bounds.Min.Y, Bounds.Max.Y - Position.Y)
            : FMath::Min(Position.X - Bounds.Min.X, Bounds.Max.X - Position.X);
        const double EndDistance = LongAxis == EAvenorWorldLongAxis::X
            ? FMath::Min(Position.X - Bounds.Min.X, Bounds.Max.X - Position.X)
            : FMath::Min(Position.Y - Bounds.Min.Y, Bounds.Max.Y - Position.Y);
        double CoastDistance = TNumericLimits<double>::Max();
        if (bOceanWidthEdges)
        {
            CoastDistance = FMath::Min(CoastDistance, WidthDistance);
        }
        if (bOceanLengthEnds)
        {
            CoastDistance = FMath::Min(CoastDistance, EndDistance);
        }
        if (CoastDistance < CoastWidth)
        {
            const double LandAlpha = Quintic(CoastDistance / FMath::Max(1.0, CoastWidth));
            Height = FMath::Lerp(SeaLevel - OceanDepth, Height, LandAlpha);
        }
    }
    return Height;
}
} // namespace UE::Avenor::Geomorph
