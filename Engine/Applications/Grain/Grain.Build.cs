using LuminaBuildTool.Configuration;

// A micro-voxel world held as a sparse brickmap in GPU memory, with every shader embedded in the binary.
public class Grain : LuminaModuleRules
{
    public Grain(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.ConsoleApplication;
        HostType = ModuleHostType.Developer;

        // A sample rather than an engine module, so it declares no reflected types.
        bEnableReflection = false;

        PrivateIncludePaths.Add("Source");

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Runtime",
            "RPMalloc",
        });
    }
}
