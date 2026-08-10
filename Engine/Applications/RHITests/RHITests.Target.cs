using LuminaBuildTool.Configuration;

public class RHITestsTarget : LuminaTargetRules
{
    public RHITestsTarget(TargetInfo Target)
        : base(Target)
    {
        Type = Target.Type;
        LaunchModuleName = "RHITests";

        PreBuildTargetNames.Add("Reflector");

        // Never monolithic: the point is to exercise the same Runtime DLL the editor loads, not a
        // separately linked copy of it.
        bMonolithic = false;

        // Built on demand, not as part of every solution build.
        bBuildByDefault = false;
    }
}
