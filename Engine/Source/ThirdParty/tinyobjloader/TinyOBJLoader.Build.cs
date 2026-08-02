using LuminaBuildTool.Configuration;

public class TinyOBJLoader : LuminaThirdPartyModuleRules
{
    public TinyOBJLoader(TargetInfo Target)
        : base(Target)
    {
        // Included as <tinyobjloader/tiny_obj_loader.h>, so the ThirdParty root is the include path.
        PublicIncludePaths.Add("..");
    }
}
