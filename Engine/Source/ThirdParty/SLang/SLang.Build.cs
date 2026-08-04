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
        //
        // slang-glslang.dll is a DOWNSTREAM compiler, resolved by name through the DLL search path
        // (which starts at the executable's directory) the first time Slang needs spirv-opt. Leaving
        // it out did not fail the build or the load of slang.dll -- it only surfaced as
        // "failed to load downstream compiler 'spirv-opt'" during compilation, so a warm shader
        // cache hid it completely and only a cold cache (a fresh clone, a cleared cache) hit it.
        // Every shader compiled that way skips SPIR-V optimisation.
        //
        // Not slang-glsl-module.dll, which despite the similar name is Slang's GLSL-compatibility
        // module rather than the glslang/SPIRV-Tools backend.
        foreach (string Library in new[]
        {
            "slang.dll",
            "slang-compiler.dll",
            "slang-glsl-module.dll",
            "slang-glslang.dll",
            "slang-rt.dll",
        })
        {
            AddRuntimeDependency("../../../../External/SLang/bin/" + Library, bOptional: true);
        }
    }
}
