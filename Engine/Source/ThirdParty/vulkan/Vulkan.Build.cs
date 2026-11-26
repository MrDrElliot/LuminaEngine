using LuminaBuildTool.Configuration;

public class Vulkan : LuminaThirdPartyModuleRules
{
    public Vulkan(TargetInfo Target)
        : base(Target)
    {
        // Headers only, since the RHI resolves every entry point at runtime and never links the loader.
        BinaryType = ModuleBinaryType.HeaderOnly;
        PublicIncludePaths.Add(".");
        PublicIncludePaths.Add("..");
        PublicDefinitions.Add("VK_NO_PROTOTYPES");
    }
}
