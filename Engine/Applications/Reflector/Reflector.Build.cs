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

        // Windows has no rpath: the loader searches the executable's directory, so libclang.dll has to
        // be staged beside it. Deliberately NOT optional -- a missing copy means Setup never fetched
        // External/LLVM, and failing here names the file instead of surfacing as an access violation
        // inside the Reflector.
        //
        // ELF does NOT get the same treatment, and staging the libraries there is actively wrong. The
        // distribution's libLLVM.so.19.1 carries DT_RUNPATH=$ORIGIN/../lib, and DT_RUNPATH on an object
        // suppresses the inherited DT_RPATH of everything above it when resolving THAT object's own
        // dependencies. Copied into Binaries/Linux64 its $ORIGIN/../lib becomes Binaries/lib, which does
        // not exist, so libLLVM's libxml2.so.2 falls through to the system path and fails -- the host
        // ships libxml2.so.16. Left in External/LLVM/lib the same RUNPATH resolves to that directory,
        // which is exactly where the bundle stages the closure. See the rpath block below.
        if (Target.Platform == BuildPlatform.Windows64)
        {
            AddRuntimeDependency("../../../External/LLVM/bin/libclang.dll");
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
            //
            // Inherited, but not unconditionally: the chain stops at any object that has a RUNPATH of
            // its own, and the distribution's libLLVM has one. That is why the LLVM libraries are not
            // staged into Binaries -- see the runtime-dependency block above. This flag is still what
            // resolves the transitive needs of the bundle's RUNPATH-less libraries (libedit's libtinfo,
            // libbsd's libmd).
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
