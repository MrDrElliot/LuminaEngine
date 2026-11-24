using LuminaBuildTool.Configuration;

public class RenderDocEditor : LuminaModuleRules
{
    public RenderDocEditor(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;
        HostType = ModuleHostType.Editor;

        bEnableReflection = false;

        PublicIncludePaths.Add(".");

        // RenderDoc is public because the module header holds the API table type.
        PublicDependencyModuleNames.AddRange(new[] { "Runtime", "RenderDoc" });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Editor",
            "RPMalloc",
        });

        // Force-included because this module has no engine PCH to prime the ECS headers.
        ForceIncludeFiles.Add("World/ECS/Registry.h");
    }
}
