using LuminaBuildTool.Configuration;

public class RenderDoc : LuminaThirdPartyModuleRules
{
    public RenderDoc(TargetInfo Target)
        : base(Target)
    {
        // In-app frame capture API; headers only, the runtime loads the DLL itself.
        BinaryType = ModuleBinaryType.HeaderOnly;
        PublicIncludePaths.Add(".");
    }
}
