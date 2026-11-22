using LuminaBuildTool.Configuration;

[TestTarget]
public class RHITestsTarget : LuminaTargetRules
{
    public RHITestsTarget(TargetInfo Target)
        : base(Target)
    {
        Type = Target.Type;
        LaunchModuleName = "RHITests";

        PreBuildTargetNames.Add("Reflector");

        // Never monolithic, so the tests exercise the same Runtime DLL the editor loads.
        bMonolithic = false;

        // Built on demand, not as part of every solution build.
        bBuildByDefault = false;
    }
}
