using UnrealBuildTool;
using System.IO;

public class AvenorEditor : ModuleRules
{
    public AvenorEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PrivateIncludePaths.Add(
            Path.Combine(ModuleDirectory, "../Avenor")
        );

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "AssetRegistry",
                "Avenor",
                "UnrealEd",
                "Water",
                "MeshPartition",
                "MeshPartitionEditor",
                "MeshPartitionWater",
                "ProceduralMeshComponent",
                "MeshDescription",
                "StaticMeshDescription"
            }
        );
    }
}
