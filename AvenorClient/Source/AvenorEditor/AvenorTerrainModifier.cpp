#include "AvenorTerrainModifier.h"

#include "SpineGenerator.h"

namespace UE::Avenor::TerrainModifier
{
struct FMountain
{
    FVector2D Centre = FVector2D::ZeroVector;
    double Radius = 1.0;
    double Height = 0.0;
};

struct FLake
{
    FVector2D Centre = FVector2D::ZeroVector;
    double Radius = 1.0;
    double Depth = 0.0;
    double SurfaceHeight = 0.0;
};

struct FRiver
{
    TArray<FVector2D> Points;
    TArray<double> BedHeights;
    double Width = 1.0;
    double Depth = 0.0;
};

struct FSettings
{
    int32 Seed = 0;
    double GentleCorridorHalfWidth = 0.0;
    double FullRoughnessDistance = 1.0;
    double CorridorRoughnessFraction = 0.0;
    double RegionalScale = 1.0;
    double RegionalRelief = 0.0;
    double HillScale = 1.0;
    double HillRelief = 0.0;
    double MountainMinimumSpineDistance = 0.0;
};

static double Smooth01(double Value)
{
    const double T = FMath::Clamp(Value, 0.0, 1.0);
    return T * T * (3.0 - 2.0 * T);
}

static double Quintic01(double Value)
{
    const double T = FMath::Clamp(Value, 0.0, 1.0);
    return T * T * T * (T * (T * 6.0 - 15.0) + 10.0);
}

static double DistanceToSegment(
    const FVector2D& Point,
    const FVector2D& A,
    const FVector2D& B,
    double& OutAlpha
)
{
    const FVector2D Segment = B - A;
    const double LengthSquared = Segment.SizeSquared();
    OutAlpha = LengthSquared > UE_DOUBLE_KINDA_SMALL_NUMBER
        ? FMath::Clamp(
            FVector2D::DotProduct(Point - A, Segment) / LengthSquared,
            0.0,
            1.0
        )
        : 0.0;
    return FVector2D::Distance(Point, A + Segment * OutAlpha);
}

static double DistanceToPolyline(
    const FVector2D& Point,
    const TArray<FVector2D>& Polyline,
    FVector2D* OutClosestPoint = nullptr
)
{
    if (Polyline.IsEmpty())
    {
        if (OutClosestPoint)
        {
            *OutClosestPoint = Point;
        }
        return TNumericLimits<double>::Max();
    }
    if (Polyline.Num() == 1)
    {
        if (OutClosestPoint)
        {
            *OutClosestPoint = Polyline[0];
        }
        return FVector2D::Distance(Point, Polyline[0]);
    }

    double Best = TNumericLimits<double>::Max();
    for (int32 Index = 0; Index + 1 < Polyline.Num(); ++Index)
    {
        double Alpha = 0.0;
        const double Distance = DistanceToSegment(
            Point,
            Polyline[Index],
            Polyline[Index + 1],
            Alpha
        );
        if (Distance < Best)
        {
            Best = Distance;
            if (OutClosestPoint)
            {
                *OutClosestPoint = FMath::Lerp(
                    Polyline[Index],
                    Polyline[Index + 1],
                    Alpha
                );
            }
        }
    }
    return Best;
}

static FVector2D SeedOffset(int32 Seed)
{
    return FVector2D(
        static_cast<double>(Seed) * 13.17,
        static_cast<double>(Seed) * -7.91
    );
}

static double Noise(const FVector2D& Point, double Scale)
{
    return FMath::PerlinNoise2D(Point / FMath::Max(1.0, Scale));
}

static double EvaluateRegionalHeight(
    const FVector2D& Point,
    const FVector2D& Offset,
    const FSettings& Settings
)
{
    const FVector2D Warp(
        Noise(Point + Offset * 0.37, Settings.RegionalScale * 1.4),
        Noise(Point + Offset * 0.61, Settings.RegionalScale * 1.4)
    );
    const FVector2D Warped = Point + Warp * Settings.HillScale * 0.55;
    return Noise(Warped + Offset, Settings.RegionalScale) *
        Settings.RegionalRelief;
}

static double EvaluateLand(
    const FVector2D& Point,
    const TArray<FVector2D>& SpinePoints,
    const TArray<FMountain>& Mountains,
    const FSettings& Settings
)
{
    const FVector2D Offset = SeedOffset(Settings.Seed);
    FVector2D ClosestSpinePoint = Point;
    const double DistanceFromSpine = DistanceToPolyline(
        Point,
        SpinePoints,
        &ClosestSpinePoint
    );

    const FVector2D Warp(
        Noise(Point + Offset * 0.37, Settings.RegionalScale * 1.4),
        Noise(Point + Offset * 0.61, Settings.RegionalScale * 1.4)
    );
    const FVector2D Warped = Point + Warp * Settings.HillScale * 0.55;

    const double RoughnessTransition = Smooth01(
        (DistanceFromSpine - Settings.GentleCorridorHalfWidth) /
        FMath::Max(
            1.0,
            Settings.FullRoughnessDistance -
                Settings.GentleCorridorHalfWidth
        )
    );

    // Preserve broad elevation along the Spine, but near it use the elevation
    // sampled at the nearest Spine point. This removes lateral hills and
    // valleys across the development corridor without pinning the entire
    // route to one absolute world height.
    const double RegionalAtPoint = EvaluateRegionalHeight(
        Point,
        Offset,
        Settings
    );
    const double RegionalAtSpine = EvaluateRegionalHeight(
        ClosestSpinePoint,
        Offset,
        Settings
    );
    const double Regional = FMath::Lerp(
        RegionalAtSpine,
        RegionalAtPoint,
        RoughnessTransition
    );

    const double RoughnessWeight = FMath::Lerp(
        Settings.CorridorRoughnessFraction,
        1.0,
        RoughnessTransition
    );

    const double BroadHills =
        Noise(Warped + Offset * 1.73, Settings.HillScale) * 0.58;
    const double MediumHills =
        Noise(Warped + Offset * 2.41, Settings.HillScale * 0.42) * 0.27;
    const double RidgeNoise =
        Noise(Warped + Offset * 3.19, Settings.HillScale * 0.75);
    const double Ridges =
        ((1.0 - FMath::Abs(RidgeNoise)) * 2.0 - 1.0) * 0.15;

    double Height = Regional + (
        BroadHills + MediumHills + Ridges
    ) * Settings.HillRelief * RoughnessWeight;

    for (const FMountain& Mountain : Mountains)
    {
        const double NormalisedDistance =
            FVector2D::Distance(Point, Mountain.Centre) /
            FMath::Max(1.0, Mountain.Radius);
        if (NormalisedDistance >= 1.0)
        {
            continue;
        }

        const double Envelope = Quintic01(1.0 - NormalisedDistance);
        const double LocalVariation = 0.78 + 0.22 * (
            0.5 + 0.5 * Noise(
                Point + Offset * 4.17,
                Settings.HillScale
            )
        );

        // Suppression is based on roughness permission, not added elevation.
        Height += Mountain.Height * Envelope * LocalVariation *
            RoughnessTransition;
    }
    return Height;
}

class FBackgroundOp final
    : public UE::MeshPartition::IModifierBackgroundOp
{
public:
    explicit FBackgroundOp(const FName& OperationName)
        : IModifierBackgroundOp(OperationName)
    {
    }

    virtual void GetInstancesInBounds(
        const FBox& InBounds,
        TArray<FInstanceInfo>& OutInstances
    ) const override
    {
        AddDefaultInstanceIfIntersects(
            GlobalBounds,
            InBounds,
            OutInstances
        );
    }

    virtual void ApplyModifications(
        UE::MeshPartition::FMeshView& MeshView,
        const FTransform3d& MeshTransform,
        const FInstanceInfo& InstanceInfo
    ) const override
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(
            UAvenorTerrainModifier::ApplyModifications
        );

        for (int32 VertexIndex = 0;
             VertexIndex < MeshView.VertexCount();
             ++VertexIndex)
        {
            FVector3d WorldPosition = MeshTransform.TransformPosition(
                MeshView.GetVertexPos(VertexIndex)
            );
            const FVector2D Point(WorldPosition.X, WorldPosition.Y);

            double Height = EvaluateLand(
                Point,
                SpinePoints,
                Mountains,
                Settings
            );

            WorldPosition.Z = BaseWorldZ + Height;
            MeshView.SetVertexPos(
                VertexIndex,
                MeshTransform.InverseTransformPosition(WorldPosition)
            );
        }
    }

    static FGuid CodeVersion()
    {
        static const FGuid Version(
            TEXT("9cb06824-0dd5-45dc-8f89-bb3d9b49f488")
        );
        return Version;
    }

    virtual bool DisableDDCWrite() const override
    {
        return false;
    }

    FBox GlobalBounds;
    double BaseWorldZ = 0.0;
    FSettings Settings;
    TArray<FVector2D> SpinePoints;
    TArray<FMountain> Mountains;
    TArray<FLake> Lakes;
    TArray<FRiver> Rivers;
};
} // namespace UE::Avenor::TerrainModifier

using namespace UE::Avenor::TerrainModifier;

UAvenorTerrainModifier::UAvenorTerrainModifier()
{
}

double UAvenorTerrainModifier::EvaluateBaseHeightAtWorldPosition(
    const FVector2D& WorldPosition
) const
{
    FSettings LocalSettings;
    LocalSettings.Seed = WorldSeed;
    LocalSettings.GentleCorridorHalfWidth = GentleCorridorHalfWidth;
    LocalSettings.FullRoughnessDistance = FMath::Max(
        GentleCorridorHalfWidth + 1.0,
        FullRoughnessDistance
    );
    LocalSettings.CorridorRoughnessFraction =
        CorridorRoughnessFraction;
    LocalSettings.RegionalScale = RegionalScale;
    LocalSettings.RegionalRelief = RegionalRelief;
    LocalSettings.HillScale = HillScale;
    LocalSettings.HillRelief = HillRelief;
    LocalSettings.MountainMinimumSpineDistance =
        MountainMinimumSpineDistance;

    TArray<FVector2D> LocalSpinePoints;
    if (Spine)
    {
        const double Start = -UnscaledCoverage.X * 0.5;
        const double End = UnscaledCoverage.X * 0.5;
        const double Step = FMath::Max(1000.0, SpineSampleSpacing);
        for (double Chainage = Start; Chainage < End; Chainage += Step)
        {
            const FVector Position =
                Spine->GetSpineLocationAtChainage(Chainage);
            LocalSpinePoints.Emplace(Position.X, Position.Y);
        }
        const FVector EndPosition =
            Spine->GetSpineLocationAtChainage(End);
        LocalSpinePoints.Emplace(EndPosition.X, EndPosition.Y);
    }
    else
    {
        const FTransform Transform = GetComponentTransform();
        const FVector Start = Transform.TransformPosition(
            FVector(-UnscaledCoverage.X * 0.5, 0.0, 0.0)
        );
        const FVector End = Transform.TransformPosition(
            FVector(UnscaledCoverage.X * 0.5, 0.0, 0.0)
        );
        LocalSpinePoints.Emplace(Start.X, Start.Y);
        LocalSpinePoints.Emplace(End.X, End.Y);
    }

    TArray<FMountain> LocalMountains;
    FRandomStream Random(WorldSeed);
    const FTransform ComponentTransform = GetComponentTransform();
    auto RandomWorldPoint = [&]()
    {
        const FVector Local(
            Random.FRandRange(
                -UnscaledCoverage.X * 0.5,
                UnscaledCoverage.X * 0.5
            ),
            Random.FRandRange(
                -UnscaledCoverage.Y * 0.5,
                UnscaledCoverage.Y * 0.5
            ),
            0.0
        );
        const FVector World =
            ComponentTransform.TransformPosition(Local);
        return FVector2D(World.X, World.Y);
    };

    for (int32 Index = 0; Index < MountainRegionCount; ++Index)
    {
        FVector2D Centre;
        for (int32 Attempt = 0; Attempt < 32; ++Attempt)
        {
            Centre = RandomWorldPoint();
            if (DistanceToPolyline(Centre, LocalSpinePoints) >=
                MountainMinimumSpineDistance)
            {
                break;
            }
        }

        FMountain Mountain;
        Mountain.Centre = Centre;
        Mountain.Radius =
            MountainRadius * Random.FRandRange(0.75, 1.35);
        Mountain.Height =
            MountainRelief * Random.FRandRange(0.65, 1.25);
        LocalMountains.Add(Mountain);
    }

    return EvaluateLand(
        WorldPosition,
        LocalSpinePoints,
        LocalMountains,
        LocalSettings
    );
}

FBox UAvenorTerrainModifier::GetTerrainWorldBounds() const
{
    const TArray<FBox> Bounds = ComputeBounds();
    return Bounds.IsEmpty() ? FBox(ForceInit) : Bounds[0];
}

TArray<FBox> UAvenorTerrainModifier::ComputeBounds() const
{
    const UE::Geometry::FAxisAlignedBox3d LocalBounds(
        -UnscaledCoverage * 0.5,
        UnscaledCoverage * 0.5
    );
    return {
        FBox(UE::Geometry::FAxisAlignedBox3d(
            LocalBounds,
            GetComponentTransform()
        ))
    };
}

TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
UAvenorTerrainModifier::CreateBackgroundOp(
    UE::MeshPartition::EBuildType InBuildType
) const
{
    (void)InBuildType;

    TSharedPtr<FBackgroundOp> Op =
        MakeShared<FBackgroundOp>(GetFName());
    Op->GlobalBounds = ComputeCombinedBounds();
    Op->BaseWorldZ = GetComponentLocation().Z;
    Op->Settings.Seed = WorldSeed;
    Op->Settings.GentleCorridorHalfWidth = GentleCorridorHalfWidth;
    Op->Settings.FullRoughnessDistance = FMath::Max(
        GentleCorridorHalfWidth + 1.0,
        FullRoughnessDistance
    );
    Op->Settings.CorridorRoughnessFraction =
        CorridorRoughnessFraction;
    Op->Settings.RegionalScale = RegionalScale;
    Op->Settings.RegionalRelief = RegionalRelief;
    Op->Settings.HillScale = HillScale;
    Op->Settings.HillRelief = HillRelief;
    Op->Settings.MountainMinimumSpineDistance =
        MountainMinimumSpineDistance;

    if (Spine)
    {
        // Terrain coverage is much longer than the initial authored road
        // blocks. SpineGenerator deliberately extends its end tangents for
        // out-of-range chainage, so sample the modifier's complete length.
        const double Start = -UnscaledCoverage.X * 0.5;
        const double End = UnscaledCoverage.X * 0.5;
        const double Step = FMath::Max(1000.0, SpineSampleSpacing);
        for (double Chainage = Start; Chainage < End; Chainage += Step)
        {
            const FVector Position =
                Spine->GetSpineLocationAtChainage(Chainage);
            Op->SpinePoints.Emplace(Position.X, Position.Y);
        }
        const FVector EndPosition =
            Spine->GetSpineLocationAtChainage(End);
        Op->SpinePoints.Emplace(EndPosition.X, EndPosition.Y);
    }
    else
    {
        const FTransform Transform = GetComponentTransform();
        const FVector Start = Transform.TransformPosition(
            FVector(-UnscaledCoverage.X * 0.5, 0.0, 0.0)
        );
        const FVector End = Transform.TransformPosition(
            FVector(UnscaledCoverage.X * 0.5, 0.0, 0.0)
        );
        Op->SpinePoints.Emplace(Start.X, Start.Y);
        Op->SpinePoints.Emplace(End.X, End.Y);
    }

    FRandomStream Random(WorldSeed);
    const FTransform ComponentTransform = GetComponentTransform();
    auto RandomWorldPoint = [&]()
    {
        const FVector Local(
            Random.FRandRange(
                -UnscaledCoverage.X * 0.5,
                UnscaledCoverage.X * 0.5
            ),
            Random.FRandRange(
                -UnscaledCoverage.Y * 0.5,
                UnscaledCoverage.Y * 0.5
            ),
            0.0
        );
        const FVector World =
            ComponentTransform.TransformPosition(Local);
        return FVector2D(World.X, World.Y);
    };

    for (int32 Index = 0; Index < MountainRegionCount; ++Index)
    {
        FVector2D Centre;
        for (int32 Attempt = 0; Attempt < 32; ++Attempt)
        {
            Centre = RandomWorldPoint();
            if (DistanceToPolyline(Centre, Op->SpinePoints) >=
                MountainMinimumSpineDistance)
            {
                break;
            }
        }

        FMountain Mountain;
        Mountain.Centre = Centre;
        Mountain.Radius =
            MountainRadius * Random.FRandRange(0.75, 1.35);
        Mountain.Height =
            MountainRelief * Random.FRandRange(0.65, 1.25);
        Op->Mountains.Add(Mountain);
    }

    for (int32 Index = 0; Index < LakeCount; ++Index)
    {
        FLake Lake;
        Lake.Centre = RandomWorldPoint();
        Lake.Radius = LakeRadius * Random.FRandRange(0.65, 1.35);
        Lake.Depth = LakeDepth * Random.FRandRange(0.75, 1.25);
        Lake.SurfaceHeight = EvaluateLand(
            Lake.Centre,
            Op->SpinePoints,
            Op->Mountains,
            Op->Settings
        ) - Lake.Depth * 0.25;
        Op->Lakes.Add(Lake);

        FRiver River;
        River.Width = RiverWidth * Random.FRandRange(0.8, 1.25);
        River.Depth = RiverDepth;

        const FVector LakeLocal =
            ComponentTransform.InverseTransformPosition(
                FVector(Lake.Centre.X, Lake.Centre.Y, 0.0)
            );
        const bool bCrossSpine =
            SpineCrossingEvery > 0 &&
            (Index % SpineCrossingEvery) == 0;
        const double SourceSide =
            LakeLocal.Y < 0.0 ? -1.0 : 1.0;
        const double OutletSide =
            bCrossSpine ? -SourceSide : SourceSide;
        const FVector OutletLocal(
            Random.FRandRange(
                -UnscaledCoverage.X * 0.48,
                UnscaledCoverage.X * 0.48
            ),
            OutletSide * UnscaledCoverage.Y * 0.5,
            0.0
        );
        const FVector OutletWorld =
            ComponentTransform.TransformPosition(OutletLocal);
        const FVector2D Outlet(OutletWorld.X, OutletWorld.Y);

        constexpr int32 PointCount = 24;
        double AccumulatedDistance = 0.0;
        const double Phase = Random.FRandRange(-PI, PI);
        for (int32 PointIndex = 0;
             PointIndex < PointCount;
             ++PointIndex)
        {
            const double Alpha =
                static_cast<double>(PointIndex) /
                static_cast<double>(PointCount - 1);
            FVector2D Point = FMath::Lerp(
                Lake.Centre,
                Outlet,
                Alpha
            );

            const FVector2D Direction =
                (Outlet - Lake.Centre).GetSafeNormal();
            const FVector2D Perpendicular(-Direction.Y, Direction.X);
            Point += Perpendicular *
                FMath::Sin(Alpha * PI * 4.0 + Phase) *
                RiverMeander * FMath::Sin(Alpha * PI);

            if (!River.Points.IsEmpty())
            {
                AccumulatedDistance += FVector2D::Distance(
                    Point,
                    River.Points.Last()
                );
            }
            River.Points.Add(Point);
            River.BedHeights.Add(
                Lake.SurfaceHeight -
                RiverFallPerKilometre *
                    (AccumulatedDistance / 100000.0)
            );
        }
        Op->Rivers.Add(MoveTemp(River));
    }

    return Op;
}

FGuid UAvenorTerrainModifier::GetCodeVersionKey() const
{
    return FBackgroundOp::CodeVersion();
}
