using LuminaBuildTool.Configuration;

/// <summary>
/// The reference host. Every engine entry point a library user needs is called here, so a missing
/// RUNTIME_API breaks this target rather than being discovered by whoever tries to use it next.
/// </summary>
public class MinimalHost : LuminaModuleRules
{
    public MinimalHost(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.ConsoleApplication;
        HostType = ModuleHostType.Developer;

        // A sample, not an engine module: no reflected types of its own.
        bEnableReflection = false;

        PrivateIncludePaths.Add("Source");

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Runtime",
            "RPMalloc",
        });
    }
}
