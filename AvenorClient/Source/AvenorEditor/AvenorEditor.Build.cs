using UnrealBuildTool;

public class AvenorEditor : ModuleRules
{
    public AvenorEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "Avenor",
                "MeshPartition",
                "MeshPartitionEditor"
            }
        );
    }
}
