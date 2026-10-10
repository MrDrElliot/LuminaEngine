using LuminaBuildTool.Configuration;

public class DLSSRuntime : LuminaModuleRules
{
    public DLSSRuntime(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;
        HostType = ModuleHostType.Runtime;

        // No reflected types, and skipping the generator keeps it out of the NGX headers.
        bEnableReflection = false;

        PublicIncludePaths.Add(".");

        PublicDependencyModuleNames.Add("Runtime");

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "RPMalloc",
            "Volk",
            "DLSS",
        });

        // Force-included because this module has no engine PCH to prime the ECS headers.
        ForceIncludeFiles.Add("World/ECS/Registry.h");
    }
}
