using LuminaBuildTool.Configuration;

/// <summary>
/// Headless RHI exerciser. Walks the RHI entry points one at a time, submitting and waiting after each,
/// so a validation error or a device loss names the single call that caused it instead of arriving in
/// the middle of a frame with forty other things in flight.
/// </summary>
public class RHITests : LuminaModuleRules
{
    public RHITests(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.ConsoleApplication;
        HostType = ModuleHostType.Developer;

        // A harness, not an engine module: no reflected types of its own.
        bEnableReflection = false;

        PrivateIncludePaths.Add("Source");

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Runtime",
            "RPMalloc",
            "EA",
        });
    }
}
