using LuminaBuildTool.Configuration;

public class FreeType : LuminaThirdPartyModuleRules
{
    public FreeType(TargetInfo Target)
        : base(Target)
    {
        bCompileAsC = true;

        PublicIncludePaths.Add("include");
        PrivateIncludePaths.Add("src");

        PrivateDefinitions.Add("FT2_BUILD_LIBRARY");
        PrivateDefinitions.Add("_CRT_NONSTDC_NO_WARNINGS");

        // Trimmed to TrueType plus OpenType-CFF. Each entry amalgamates its own directory, so
        // re-adding a module means restoring its src subdirectory, listing its entry .c here,
        // and updating config/ftmodule.h.
        bUseExplicitSourceList = true;
        ExtraSourceFiles.AddRange(new[]
        {
            "src/autofit/autofit.c",

            "src/base/ftsystem.c",
            "src/base/ftinit.c",
            "src/base/ftdebug.c",
            "src/base/ftbase.c",
            "src/base/ftbbox.c",
            "src/base/ftbitmap.c",
            "src/base/ftgasp.c",
            "src/base/ftglyph.c",
            "src/base/ftmm.c",
            "src/base/ftpatent.c",
            "src/base/ftstroke.c",
            "src/base/ftsynth.c",
            "src/base/fttype1.c",
            "src/base/ftbdf.c",
            "src/base/ftcid.c",
            "src/base/ftfstype.c",
            "src/base/ftgxval.c",
            "src/base/ftotval.c",
            "src/base/ftpfr.c",
            "src/base/ftwinfnt.c",

            "src/cff/cff.c",
            "src/sfnt/sfnt.c",
            "src/truetype/truetype.c",

            "src/psaux/psaux.c",
            "src/pshinter/pshinter.c",
            "src/psnames/psnames.c",

            "src/raster/raster.c",
            "src/smooth/smooth.c",
        });
    }
}
