using LuminaBuildTool.Configuration;

// Loaded in both the editor and a packaged game. Gameplay lives here: components, systems and
// reflected types.
public class $RUNTIMEMODULE : LuminaModuleRules
{
    public $RUNTIMEMODULE(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;
        HostType = ModuleHostType.Runtime;

        PublicIncludePaths.Add(".");

        PublicDependencyModuleNames.Add("Runtime");

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "ImGui",
            "RPMalloc",
            "EA",
            "Entt",
            "Tracy",
        });
    }
}
