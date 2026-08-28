using LuminaBuildTool.Configuration;

public class PhysicsBenchTarget : LuminaTargetRules
{
    public PhysicsBenchTarget(TargetInfo Target)
        : base(Target)
    {
        Type = Target.Type;
        LaunchModuleName = "PhysicsBench";

        PreBuildTargetNames.Add("Reflector");

        // Measures the same Runtime DLL the editor loads, so never monolithic.
        bMonolithic = false;

        // Built on demand, not as part of every solution build.
        bBuildByDefault = false;
    }
}
