using LuminaBuildTool.Configuration;

public class LuminaLauncherTarget : LuminaTargetRules
{
    public LuminaLauncherTarget(TargetInfo Target)
        : base(Target)
    {
        Type = TargetType.Program;
        LaunchModuleName = "LuminaLauncher";

        OutputSuffix = string.Empty;

        // It sits at the package root on its own, beside none of the runtime DLLs the editor carries.
        bUseDynamicCrt = false;

        // Built by LuminaBuild Package, not as part of every solution build.
        bBuildByDefault = false;
    }
}
