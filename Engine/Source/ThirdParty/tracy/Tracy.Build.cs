using LuminaBuildTool.Configuration;

public class Tracy : LuminaThirdPartyModuleRules
{
    public Tracy(TargetInfo Target)
        : base(Target)
    {
        // Engine headers include <tracy/TracyC.h> unconditionally, and without TRACY_ENABLE it
        // compiles to nothing. So the include path is always exposed and only the client library
        // is conditional: with profiling off there is nothing to build and nothing to link.
        PublicIncludePaths.Add("public");

        if (!LuminaFeatures.IsActive(Target, LuminaFeatures.Tracy))
        {
            BinaryType = ModuleBinaryType.HeaderOnly;
            return;
        }

        // A shared library so the client lives in exactly one image; every module then talks to
        // the same profiler instance.
        BinaryType = ModuleBinaryType.SharedLibrary;

        PrivateDefinitions.Add("TRACY_EXPORTS");
        PublicDefinitions.Add("TRACY_ALLOW_SHADOW_WARNING");

        bUseExplicitSourceList = true;
        ExtraSourceFiles.Add("public/TracyClient.cpp");
    }
}
