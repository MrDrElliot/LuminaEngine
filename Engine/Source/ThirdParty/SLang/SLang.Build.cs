using LuminaBuildTool.Configuration;

public class SLang : LuminaThirdPartyModuleRules
{
    public SLang(TargetInfo Target)
        : base(Target)
    {
        // The shader compiler ships prebuilt: headers here, import libraries and DLLs under
        // the engine's External directory.
        BinaryType = ModuleBinaryType.HeaderOnly;
        PublicIncludePaths.Add(".");

        PublicLibraryPaths.Add(ModulePath("../../../../External/SLang/lib"));
        PublicSystemLibraries.Add("slang");
        PublicSystemLibraries.Add("slang-compiler");

        // Loaded at run time from beside the executable. Optional: a running editor can hold
        // these open while a second configuration builds.
        foreach (string Library in new[]
        {
            "slang.dll",
            "slang-compiler.dll",
            "slang-glsl-module.dll",
            "slang-rt.dll",
        })
        {
            AddRuntimeDependency("../../../../External/SLang/bin/" + Library, bOptional: true);
        }
    }
}
