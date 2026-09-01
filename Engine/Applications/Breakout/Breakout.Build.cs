using LuminaBuildTool.Configuration;

/// <summary>
/// A complete game on the bare engine: window, RHI and ECS registry, with every shader embedded in the
/// binary so nothing is compiled from the shader tree at startup.
/// </summary>
public class Breakout : LuminaModuleRules
{
    public Breakout(TargetInfo Target)
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
