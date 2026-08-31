using LuminaBuildTool.Configuration;

/// <summary>
/// The smallest host that stands the engine's window and RHI up on their own and draws through them.
/// No engine loop, no world, no asset system.
/// </summary>
public class HelloTriangle : LuminaModuleRules
{
    public HelloTriangle(TargetInfo Target)
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
