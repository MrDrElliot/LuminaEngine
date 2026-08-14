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

        if (Target.Platform != BuildPlatform.Windows64)
        {
            // The bundle carries LLVM's own dependencies (libxml2, ICU, libedit, ...) because the
            // versions it was built against are not the versions a current distribution ships:
            // Ubuntu 26.04 has libxml2.so.16, and nothing there can satisfy the libxml2.so.2 that an
            // LLVM 19 built on 22.04 asks for.
            //
            // -rpath-link resolves them while linking (a plain -L does not apply to the transitive
            // needs of a shared library); the $ORIGIN rpath resolves them at load time, and stays
            // relative so the tree can move.
            PrivateLinkerOptions.Add($"-Wl,-rpath-link,{ModulePath("../../../External/LLVM/lib")}");
            PrivateLinkerOptions.Add("-Wl,-rpath,$ORIGIN/../../External/LLVM/lib");

            // DT_RPATH, not the DT_RUNPATH modern linkers default to. RUNPATH applies only to an
            // object's OWN direct dependencies, so it resolves libclang and libLLVM and then leaves
            // the loader to find libLLVM's libxml2 on its own -- which fails with the library sitting
            // in the very directory just named. RPATH is inherited down the dependency chain.
            PrivateLinkerOptions.Add("-Wl,--disable-new-dtags");
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
