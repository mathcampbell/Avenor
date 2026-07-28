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
				"Landscape",
				"PCG",
				"Water"
			}
		);

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
