using LuminaBuildTool.Configuration;

public class CGLTF : LuminaThirdPartyModuleRules
{
    public CGLTF(TargetInfo Target)
        : base(Target)
    {
        bCompileAsC = true;
        PublicIncludePaths.Add(".");
        SourceDirectories.Add("src");
    }
}
