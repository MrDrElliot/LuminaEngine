using LuminaBuildTool.Configuration;

/// <summary>
/// A horde survival game on the bare engine. Shaders and sounds are generated in the binary, and the
/// swarm runs as a structure of arrays so the agent count can reach the hundreds of thousands.
/// </summary>
public class Umbral : LuminaModuleRules
{
    public Umbral(TargetInfo Target)
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
