using LuminaBuildTool.Configuration;

public class MSDFGen : LuminaThirdPartyModuleRules
{
    public MSDFGen(TargetInfo Target)
        : base(Target)
    {
        // Does not compile under the engine's standard.
        CppStandardOverride = "c++17";

        // Included as <msdfgen/msdfgen.h>, so the ThirdParty root is the public include path.
        PublicIncludePaths.Add("..");

        // Its own headers self-include as <msdfgen/...>, which needs this directory too.
        PrivateIncludePaths.Add(".");

        PublicDependencyModuleNames.Add("FreeType");

        // Skia, PNG and SVG support are compiled out in msdfgen-config.h; only core plus the
        // FreeType importer are built.
        SourceDirectories.Add("core");
        ExtraSourceFiles.Add("ext/import-font.cpp");
        // Not merged: these sources define _USE_MATH_DEFINES before including <cmath> to get M_PI.
        // The macro only has an effect on the first include, so whichever source is second in a
        // merged translation unit finds M_PI undeclared (C2065).
        bUseUnityBuild = false;
    }
}
