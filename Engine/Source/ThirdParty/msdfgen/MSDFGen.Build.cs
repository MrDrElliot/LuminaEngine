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
        // equation-solver.cpp defines _USE_MATH_DEFINES before including <cmath> to get M_PI. The
        // macro only has an effect on the first include, so it has to be the one that gets there
        // first, which it cannot be from inside a merged translation unit (C2065). Only that file,
        // so only that file is held back; the rest merge.
        ExcludeFromUnity.Add("equation-solver.cpp");

        // Narrowing in the TIFF writer's templated pixel path, fatal engine-wide but not ours to fix.
        Warnings.Set(WarningSeverity.Off, CompilerWarning.ConversionLoss);
    }
}
