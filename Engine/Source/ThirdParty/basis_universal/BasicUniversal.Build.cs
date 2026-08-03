using LuminaBuildTool.Configuration;

public class BasicUniversal : LuminaThirdPartyModuleRules
{
    public BasicUniversal(TargetInfo Target)
        : base(Target)
    {
        PublicIncludePaths.Add(".");

        PublicDefinitions.Add("BASISD_SUPPORT_KTX2");
        PublicDefinitions.Add("BASISD_SUPPORT_KTX2_ZSTD=0");
        // WebAssembly and Python entry points, unguarded by any platform check, so they were being
        // compiled into a Windows desktop engine that has no caller for them. Not merely excluded
        // from unity: excluded from the build.
        ExcludedSourcePathFragments.Add("basisu_wasm");

        // These two carry using-directives that bring both basisu:: and basist:: declarations of
        // astc_6x6_hdr into scope, so the name is ambiguous the moment either shares a translation
        // unit with a neighbour (C2872). Held back individually; the rest merge.
        ExcludeFromUnity.Add("basisu_comp.cpp");
        ExcludeFromUnity.Add("basisu_enc.cpp");
    }
}
