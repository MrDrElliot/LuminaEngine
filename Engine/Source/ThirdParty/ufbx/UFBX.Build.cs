using LuminaBuildTool.Configuration;

public class UFBX : LuminaThirdPartyModuleRules
{
    public UFBX(TargetInfo Target)
        : base(Target)
    {
        bCompileAsC = true;

        // Included as "ufbx/ufbx.h" from the engine, so the ThirdParty root is the public include path.
        // src/ufbx.c includes the header unqualified, so the module directory is on its own path too.
        PublicIncludePaths.Add("..");
        PrivateIncludePaths.Add(".");

        SourceDirectories.Add("src");
    }
}
