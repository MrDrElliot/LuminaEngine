using LuminaBuildTool.Configuration;

public class Miniz : LuminaThirdPartyModuleRules
{
    public Miniz(TargetInfo Target)
        : base(Target)
    {
        PublicIncludePaths.Add(".");

        bUseExplicitSourceList = true;
        ExtraSourceFiles.Add("miniz.c");
    }
}
