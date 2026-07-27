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
				"ProceduralMeshComponent"
			}
		);

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
