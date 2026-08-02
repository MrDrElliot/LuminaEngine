using LuminaBuildTool.Configuration;

public class StbImage : LuminaThirdPartyModuleRules
{
    public StbImage(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.HeaderOnly;
        PublicIncludePaths.Add(".");
    }
}
