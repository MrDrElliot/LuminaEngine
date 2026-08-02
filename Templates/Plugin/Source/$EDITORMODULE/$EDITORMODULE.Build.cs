using LuminaBuildTool.Configuration;

// Editor only, stripped from packaged builds.
public class $EDITORMODULE : LuminaModuleRules
{
    public $EDITORMODULE(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;
        HostType = ModuleHostType.Editor;

        PublicIncludePaths.Add(".");

        PublicDependencyModuleNames.Add("Runtime");
        PublicDependencyModuleNames.Add("Editor");
        PublicDependencyModuleNames.Add("$RUNTIMEMODULE");

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
