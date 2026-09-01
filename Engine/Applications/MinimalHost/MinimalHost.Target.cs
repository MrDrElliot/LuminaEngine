using LuminaBuildTool.Configuration;

public class MinimalHostTarget : LuminaTargetRules
{
    public MinimalHostTarget(TargetInfo Target)
        : base(Target)
    {
        Type = Target.Type;
        LaunchModuleName = "MinimalHost";

        PreBuildTargetNames.Add("Reflector");

        // Links the same Runtime DLL the editor loads, which is the point of the exercise.
        bMonolithic = false;

        // Built on demand, not as part of every solution build.
        bBuildByDefault = false;
    }
}
