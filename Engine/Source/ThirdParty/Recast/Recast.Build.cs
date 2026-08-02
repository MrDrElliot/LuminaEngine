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
        // Not merged: RecastMeshDetail.cpp has file-scope names that collide with a neighbour's,
        // so an initializer binds to a function rather than the intended variable (C2440).
        bUseUnityBuild = false;
    }
}
