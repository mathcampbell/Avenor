using UnrealBuildTool;

public class Avenor : ModuleRules
{
	public Avenor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"MeshPartition",
				"PCG",
				"Water"
			}
		);

		// The Spine's Mesh Terrain corridor is an editor-only authoring
		// component. MeshPartitionModifierComponent lives in this editor
		// module in UE 5.8, so do not make packaged Avenor builds depend on it.
		if (Target.bBuildEditor)
		{
			PublicDependencyModuleNames.Add("MeshPartitionEditor");
		}

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
