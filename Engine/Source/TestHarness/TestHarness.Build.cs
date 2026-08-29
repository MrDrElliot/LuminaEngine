using LuminaBuildTool.Configuration;

public class TestHarness : LuminaModuleRules
{
    public TestHarness(TargetInfo Target)
        : base(Target)
    {
        // Linked into each test binary rather than shipped as its own image.
        BinaryType = ModuleBinaryType.StaticLibrary;
        HostType = ModuleHostType.Developer;

        bEnableReflection = false;

        PublicIncludePaths.Add("Source");

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Runtime",
            "GoogleTest",
        });
    }
}
