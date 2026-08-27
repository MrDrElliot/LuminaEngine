using LuminaBuildTool.Configuration;

// Self-check and storage-layout tuning harness for the Lumina sparse-set ECS.
public class ECSBench : LuminaModuleRules
{
    public ECSBench(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.ConsoleApplication;
        HostType = ModuleHostType.Developer;

        // A harness, not an engine module, so it declares no reflected types of its own.
        bEnableReflection = false;

        PrivateIncludePaths.Add("Source");

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Runtime",
            "RPMalloc",
        });
    }
}
