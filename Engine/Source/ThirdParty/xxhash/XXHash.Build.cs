using LuminaBuildTool.Configuration;

public class XXHash : LuminaThirdPartyModuleRules
{
    public XXHash(TargetInfo Target)
        : base(Target)
    {
        // Included as <xxhash.h>, so the library root is the include path.
        PublicIncludePaths.Add(".");
    }
}
