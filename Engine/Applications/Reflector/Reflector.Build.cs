using LuminaBuildTool.Configuration;

/// <summary>
/// Standalone reflection code generator. Parses engine headers with libclang and emits the
/// generated reflection sources the engine modules compile.
/// </summary>
public class Reflector : ModuleRules
{
    public Reflector(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.ConsoleApplication;
        HostType = ModuleHostType.Program;

        // A tool, not an engine module: no ModuleAPI.h and no generated reflection of its own.
        bEnableReflection = false;

        // The code generator dispatches over reflected type nodes with dynamic_cast.
        bEnableRtti = true;

        PrivateIncludePaths.Add("Source");

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "EA",
            "XXHash",
            "NlohmannJson",
        });

        // Prebuilt libclang and LLVM, vendored under the engine's External directory.
        PublicIncludePaths.Add(ModulePath("../../../External/LLVM/include"));
        PublicLibraryPaths.Add(ModulePath("../../../External/LLVM/lib"));
        PublicLibraryPaths.Add(ModulePath("../../../External/LLVM/bin"));

        PublicSystemLibraries.AddRange(new[]
        {
            "clangBasic",
            "clangLex",
            "clangAST",
            "libclang",
            "LLVMAnalysis",
            "LLVMBinaryFormat",
            "LLVMBitReader",
            "LLVMBitstreamReader",
            "LLVMDemangle",
            "LLVMFrontendOffloading",
            "LLVMFrontendOpenMP",
            "LLVMMC",
            "LLVMProfileData",
            "LLVMRemarks",
            "LLVMScalarOpts",
            "LLVMTargetParser",
            "LLVMTransformUtils",
            "LLVMCore",
            "LLVMSupport",
        });

        // The prebuilt LLVM libraries are release builds, so the debug CRT must stay out.
        PrivateLinkerOptions.Add("/NODEFAULTLIB:MSVCRTD");
    }
}
