using LuminaBuildTool.Configuration;

// Measures Box3D solver throughput against the engine's task bridge, so a body-count target has a number.
public class PhysicsBench : LuminaModuleRules
{
    public PhysicsBench(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.ConsoleApplication;
        HostType = ModuleHostType.Developer;

        bEnableReflection = false;

        PrivateIncludePaths.Add("Source");

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Runtime",
            "Box3D",
            "RPMalloc",
        });
    }
}
