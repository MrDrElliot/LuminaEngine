using LuminaBuildTool.Configuration;

public class JoltPhysics : LuminaThirdPartyModuleRules
{
    public JoltPhysics(TargetInfo Target)
        : base(Target)
    {
        PublicIncludePaths.Add(".");

        // AVX, not AVX2: this changes Jolt's struct layout and alignment, so it must match the
        // engine baseline in Lumina.BuildRules.cs. Do not bump one without the other.
        PublicDefinitions.Add("__AVX__");
    }
}
