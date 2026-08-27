using LuminaBuildTool.Configuration;

public class MCPEditor : LuminaModuleRules
{
    public MCPEditor(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;
        HostType = ModuleHostType.Editor;

        PublicIncludePaths.Add(".");

        PublicDependencyModuleNames.AddRange(new[] { "Runtime" });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Editor",
            "NlohmannJson",
            "RPMalloc",
        });
    }
}
