using LuminaBuildTool.Configuration;

public class TestsTarget : LuminaTargetRules
{
    public TestsTarget(TargetInfo Target)
        : base(Target)
    {
        Type = Target.Type;
        LaunchModuleName = "Tests";

        PreBuildTargetNames.Add("Reflector");

        // Never monolithic: the tests link the same module DLLs the editor loads.
        bMonolithic = false;

        // Built on demand, not as part of every solution build.
        bBuildByDefault = false;
    }
}
