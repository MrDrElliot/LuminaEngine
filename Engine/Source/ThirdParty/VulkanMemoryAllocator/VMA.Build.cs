using LuminaBuildTool.Configuration;

public class VMA : LuminaThirdPartyModuleRules
{
    public VMA(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.HeaderOnly;
        PublicIncludePaths.Add(".");
        PublicDependencyModuleNames.Add("Vulkan");
    }
}
