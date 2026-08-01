#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MeshPartitionModifierComponent.h"

#include "AvenorGeomorphTerrainGenerator.generated.h"

class AWaterBody;
struct FAvenorGeomorphData;

UENUM(BlueprintType)
enum class EAvenorWorldLongAxis : uint8
{
    X,
    Y
};

UCLASS(ClassGroup=(Avenor), meta=(BlueprintSpawnableComponent))
class AVENOREDITOR_API UAvenorGeomorphTerrainModifier final
    : public UE::MeshPartition::UModifierComponent
{
    GENERATED_BODY()

public:
    virtual TArray<FBox> ComputeBounds() const override;

    virtual TSharedPtr<const UE::MeshPartition::IModifierBackgroundOp>
    CreateBackgroundOp(
        UE::MeshPartition::EBuildType BuildType
    ) const override;

    virtual FGuid GetCodeVersionKey() const override;
};

UCLASS(BlueprintType)
class AVENOREDITOR_API AAvenorGeomorphTerrainGenerator final
    : public AActor
{
    GENERATED_BODY()

public:
    AAvenorGeomorphTerrainGenerator();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Terrain")
    TObjectPtr<UAvenorGeomorphTerrainModifier> TerrainModifier;

    UPROPERTY(
        EditAnywhere,
        BlueprintReadWrite,
        Category="Terrain",
        meta=(AllowedClasses="/Script/MeshPartition.MeshPartition")
    )
    TObjectPtr<AActor> MeshPartitionActor;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World")
    FVector2D WorldSize = FVector2D(2000000.0, 10000000.0);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World")
    EAvenorWorldLongAxis LongAxis = EAvenorWorldLongAxis::Y;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="World")
    int32 Seed = 1847;

    UPROPERTY(
        EditAnywhere,
        BlueprintReadWrite,
        Category="World",
        meta=(ClampMin="2500.0", UIMin="5000.0", Units="cm")
    )
    double AnalysisCellSize = 10000.0;

    UPROPERTY(
        EditAnywhere,
        BlueprintReadWrite,
        Category="World",
        meta=(ClampMin="10000", ClampMax="2000000")
    )
    int32 MaximumAnalysisCells = 500000;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Features")
    bool bGeneratePlains = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Features")
    bool bGenerateRollingHills = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Features")
    bool bGenerateMountains = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Features")
    bool bGenerateMesas = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Features")
    bool bGenerateValleys = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Features")
    bool bGenerateCanyons = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Features")
    bool bGenerateRivers = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Features")
    bool bGenerateLakes = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Features")
    bool bGenerateOcean = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Plains")
    double PlainsCoverage = 0.42;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Plains", meta=(Units="cm"))
    double PlainsRelief = 2200.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Hills")
    double HillsCoverage = 0.38;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Hills", meta=(Units="cm"))
    double HillsScale = 350000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Hills", meta=(Units="cm"))
    double HillsRelief = 22000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mountains")
    double MountainRangesPer100Km = 2.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mountains", meta=(Units="cm"))
    double MountainRangeLength = 2400000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mountains", meta=(Units="cm"))
    double MountainRangeWidth = 500000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mountains", meta=(Units="cm"))
    double MountainPeakSpacing = 260000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mountains", meta=(Units="cm"))
    double MountainRelief = 170000.0;

    UPROPERTY(
        EditAnywhere,
        BlueprintReadWrite,
        Category="Mountains",
        meta=(Units="cm", ToolTip="Only excludes major mountains from the central strip; it does not flatten or otherwise create a spine corridor.")
    )
    double CentralMountainExclusionHalfWidth = 180000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mountains")
    double MountainEdgeBias = 0.58;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mesas")
    double MesaCoverage = 0.08;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mesas", meta=(Units="cm"))
    double MesaScale = 320000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mesas", meta=(Units="cm"))
    double MesaRelief = 42000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Mesas", meta=(ClampMin="1", ClampMax="8"))
    int32 MesaTerraces = 4;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Erosion", meta=(ClampMin="0", ClampMax="32"))
    int32 ThermalErosionIterations = 8;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Erosion", meta=(ClampMin="0.0", ClampMax="1.0"))
    double ThermalErosionStrength = 0.32;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Erosion", meta=(ClampMin="1.0", ClampMax="60.0", Units="deg"))
    double TalusAngleDegrees = 31.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Erosion", meta=(ClampMin="0", ClampMax="32"))
    int32 StreamPowerIterations = 8;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Erosion", meta=(ClampMin="0.0", ClampMax="1.0"))
    double StreamPowerStrength = 0.16;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Hydrology", meta=(ClampMin="0.0001", Units="cm"))
    double DrainageEpsilon = 1.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rivers", meta=(ClampMin="0.01"))
    double MountainStreamStartArea = 0.45;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rivers", meta=(ClampMin="0.01"))
    double LowlandStreamStartArea = 2.5;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rivers", meta=(ClampMin="0.1"))
    double MainRiverArea = 40.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rivers", meta=(ClampMin="0.0", Units="cm"))
    double MinimumRiverSystemLength = 200000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rivers", meta=(ClampMin="0", ClampMax="8"))
    int32 JunctionOverlapCells = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rivers", meta=(Units="cm"))
    double HeadwaterWidth = 450.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rivers", meta=(Units="cm"))
    double MainRiverWidth = 6500.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rivers", meta=(Units="cm"))
    double MaximumRiverDepth = 2800.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rivers", meta=(Units="cm"))
    double HeadwaterValleyHalfWidth = 14000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rivers", meta=(Units="cm"))
    double MainValleyHalfWidth = 95000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rivers", meta=(Units="cm"))
    double MaximumValleyDepth = 11000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rivers", meta=(ClampMin="0.0", ClampMax="1.5"))
    double LowlandMeanderStrength = 0.72;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Rivers", meta=(ClampMin="1", ClampMax="512"))
    int32 MaximumRiverReaches = 128;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Canyons", meta=(ClampMin="0.1"))
    double CanyonStartArea = 18.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Lakes", meta=(ClampMin="0.01"))
    double MinimumLakeCatchmentArea = 1.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Lakes", meta=(ClampMin="1.0", Units="cm"))
    double MinimumLakeDepth = 180.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Lakes", meta=(ClampMin="0.01"))
    double MaximumLakeArea = 25.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Lakes", meta=(ClampMin="0", ClampMax="64"))
    int32 MaximumLakeCount = 12;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Lakes", meta=(Units="cm"))
    double LakeBankBlendWidth = 24000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ocean", meta=(Units="cm"))
    double SeaLevel = -5000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ocean", meta=(Units="cm"))
    double OceanDepth = 30000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ocean", meta=(Units="cm"))
    double CoastTransitionWidth = 220000.0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ocean")
    bool bOceanAlongWidthEdges = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ocean")
    bool bOceanAtLengthEnds = false;

    UFUNCTION(CallInEditor, Category="Avenor Terrain")
    void RegenerateTerrainAndWater();

    UFUNCTION(CallInEditor, Category="Avenor Terrain")
    void ClearGeneratedWater();

    FBox GetGenerationBounds() const;
    TSharedPtr<const FAvenorGeomorphData> GetOrCreateData() const;
    void InvalidateData();

#if WITH_EDITOR
    virtual void PostEditChangeProperty(
        FPropertyChangedEvent& PropertyChangedEvent
    ) override;
#endif

private:
    void CreateWaterActors(
        const TSharedPtr<const FAvenorGeomorphData>& Data
    );

    mutable FCriticalSection DataMutex;
    mutable TSharedPtr<const FAvenorGeomorphData> CachedData;
};
