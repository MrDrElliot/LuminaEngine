using LuminaBuildTool.Configuration;

public class Recast : LuminaThirdPartyModuleRules
{
    public Recast(TargetInfo Target)
        : base(Target)
    {
        PublicIncludePaths.Add("Recast/Include");
        PublicIncludePaths.Add("Detour/Include");

        SourceDirectories.Add("Recast/Source");
        SourceDirectories.Add("Detour/Source");
        // These three each define their own file-scope prev/next/area2/left/vequal helpers, so any
        // two of them in one translation unit redefine each other, and an initializer then binds to
        // a function rather than the intended variable (C2084, C2440). Held back individually; the
        // rest of Recast and all of Detour merge.
        ExcludeFromUnity.Add("RecastMesh.cpp");
        ExcludeFromUnity.Add("RecastContour.cpp");
        ExcludeFromUnity.Add("RecastMeshDetail.cpp");
    }
}
