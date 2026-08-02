using LuminaBuildTool.Configuration;

public class FastGLTF : LuminaThirdPartyModuleRules
{
    public FastGLTF(TargetInfo Target)
        : base(Target)
    {
        PublicIncludePaths.Add("include");
        PrivateIncludePaths.Add(".");
    }
}
