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

        // Deliberately NOT optional: a missing copy means Setup never fetched External/LLVM, and failing
        // here names the file instead of surfacing as an access violation inside the Reflector.
        //
        // ELF: staged under the SONAMEs recorded in the binary, not the unversioned development
        // symlinks. Staging libclang.so alone leaves the loader asking for libclang-19.so.19, which it
        // then satisfies from a distro install if one happens to exist -- so the omission is invisible
        // on a developer box and fatal on a clean one.
        foreach (string Library in Target.Platform == BuildPlatform.Windows64
            ? new[] { "bin/libclang.dll" }
            : new[] { "lib/libclang-19.so.19", "lib/libLLVM.so.19.1" })
        {
            AddRuntimeDependency($"../../../External/LLVM/{Library}");
        }

        if (Target.Platform == BuildPlatform.Windows64)
        {
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

            PrivateLinkerOptions.Add("/NODEFAULTLIB:MSVCRTD");
        }
        else
        {
            PublicSystemLibraries.Add("clang");
            PublicSystemLibraries.Add("clang-cpp");
            PublicSystemLibraries.Add("LLVM-19");
        }
    }
}
