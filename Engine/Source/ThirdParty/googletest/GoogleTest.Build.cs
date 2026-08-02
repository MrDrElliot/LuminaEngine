using LuminaBuildTool.Configuration;

public class GoogleTest : LuminaThirdPartyModuleRules
{
    public GoogleTest(TargetInfo Target)
        : base(Target)
    {
        HostType = ModuleHostType.Developer;

        PublicIncludePaths.Add("include");
        PrivateIncludePaths.Add(".");

        // gtest-all.cc textually includes every other source and gtest_main.cc defines main();
        // compiling either alongside the individual sources duplicates symbols.
        bUseExplicitSourceList = true;
        ExtraSourceFiles.AddRange(new[]
        {
            "src/gtest.cc",
            "src/gtest-assertion-result.cc",
            "src/gtest-death-test.cc",
            "src/gtest-filepath.cc",
            "src/gtest-matchers.cc",
            "src/gtest-port.cc",
            "src/gtest-printers.cc",
            "src/gtest-test-part.cc",
            "src/gtest-typed-test.cc",
        });
    }
}
