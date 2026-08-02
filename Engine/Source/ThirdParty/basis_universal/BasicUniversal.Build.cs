using LuminaBuildTool.Configuration;

public class BasicUniversal : LuminaThirdPartyModuleRules
{
    public BasicUniversal(TargetInfo Target)
        : base(Target)
    {
        PublicIncludePaths.Add(".");

        PublicDefinitions.Add("BASISD_SUPPORT_KTX2");
        PublicDefinitions.Add("BASISD_SUPPORT_KTX2_ZSTD=0");
        // Not merged: basisu_comp.cpp resolves 'astc_6x6_hdr' ambiguously once it shares a
        // translation unit with its neighbours, which each bring their own declaration of it
        // into scope (C2872).
        bUseUnityBuild = false;
    }
}
