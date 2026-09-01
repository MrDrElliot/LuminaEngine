using LuminaBuildTool.Configuration;

public class BreakoutTarget : LuminaTargetRules
{
    public BreakoutTarget(TargetInfo Target)
        : base(Target)
    {
        Type = Target.Type;
        LaunchModuleName = "Breakout";

        PreBuildTargetNames.Add("Reflector");

        // Draws through the same Runtime DLL the editor loads, not a separately linked copy of it.
        bMonolithic = false;

        // Built on demand, not as part of every solution build.
        bBuildByDefault = false;
    }
}
