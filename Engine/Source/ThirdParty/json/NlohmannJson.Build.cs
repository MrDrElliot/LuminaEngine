using LuminaBuildTool.Configuration;

public class NlohmannJson : LuminaThirdPartyModuleRules
{
    public NlohmannJson(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.HeaderOnly;
        PublicIncludePaths.Add(".");
    }
}
