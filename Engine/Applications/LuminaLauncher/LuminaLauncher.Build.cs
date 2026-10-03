using LuminaBuildTool.Configuration;

// The executable at a packaged engine's root, which starts the editor under Binaries.
public class LuminaLauncher : ModuleRules
{
    public LuminaLauncher(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.WindowedApplication;
        HostType = ModuleHostType.Program;

        // A tool rather than an engine module, so it has no ModuleAPI.h or generated reflection.
        bEnableReflection = false;

        PrivateIncludePaths.Add("Source");
    }
}
