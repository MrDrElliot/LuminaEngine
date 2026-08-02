using LuminaBuildTool.Configuration;

public class Volk : LuminaThirdPartyModuleRules
{
    public Volk(TargetInfo Target)
        : base(Target)
    {
        // Included as <volk/volk.h>, so the ThirdParty root is the include path.
        PublicIncludePaths.Add("..");
        PrivateIncludePaths.Add(".");

        PublicDependencyModuleNames.Add("Vulkan");
    }
}
