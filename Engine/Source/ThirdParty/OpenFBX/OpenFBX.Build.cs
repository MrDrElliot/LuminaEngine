using LuminaBuildTool.Configuration;

public class OpenFBX : LuminaThirdPartyModuleRules
{
    public OpenFBX(TargetInfo Target)
        : base(Target)
    {
        // Included as "OpenFBX/ofbx.h", so the ThirdParty root is the include path.
        PublicIncludePaths.Add("..");
    }
}
