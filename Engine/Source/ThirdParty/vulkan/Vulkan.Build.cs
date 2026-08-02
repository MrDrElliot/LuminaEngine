using LuminaBuildTool.Configuration;

public class Vulkan : LuminaThirdPartyModuleRules
{
    public Vulkan(TargetInfo Target)
        : base(Target)
    {
        // Headers only: entry points are loaded through Volk, never linked directly.
        BinaryType = ModuleBinaryType.HeaderOnly;
        PublicIncludePaths.Add(".");
        PublicIncludePaths.Add("..");
    }
}
