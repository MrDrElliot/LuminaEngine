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

        // Stage the vendored libclang next to Reflector.exe. The application directory beats PATH in the
        // Windows DLL search, so this pins the parse to LLVM 19 instead of whatever the host happens to
        // have installed -- an older system libclang loads fine and then dies mid-parse on modern C++.
        // Deliberately NOT optional: a missing copy means Setup never fetched External/LLVM, and failing
        // here names the file instead of surfacing as an access violation inside the Reflector.
        AddRuntimeDependency("../../../External/LLVM/bin/libclang.dll");

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
