using LuminaBuildTool.Configuration;

public class Zstd : LuminaThirdPartyModuleRules
{
    public Zstd(TargetInfo Target)
        : base(Target)
    {
        PublicIncludePaths.Add(".");

        bUseExplicitSourceList = true;
        ExtraSourceFiles.Add("zstd.c");
    }
}
