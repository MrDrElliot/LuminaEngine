using LuminaBuildTool.Configuration;

public class MikkTSpace : LuminaThirdPartyModuleRules
{
    public MikkTSpace(TargetInfo Target)
        : base(Target)
    {
        bCompileAsC = true;
        PublicIncludePaths.Add("src");
        SourceDirectories.Add("src");
    }
}
