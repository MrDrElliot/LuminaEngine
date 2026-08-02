using LuminaBuildTool.Configuration;

public class MeshOptimizer : LuminaThirdPartyModuleRules
{
    public MeshOptimizer(TargetInfo Target)
        : base(Target)
    {
        PublicIncludePaths.Add("src");
        SourceDirectories.Add("src");
    }
}
