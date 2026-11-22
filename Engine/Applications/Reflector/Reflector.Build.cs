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

        // Exports the generator API that Reflection modules link against.
        PrivateDefinitions.Add("LRT_HOST");

        // Names hash exactly as the engine hashes them, through the same header-only XXH64.
        PrivateIncludePaths.Add(ModulePath("../../Source/Runtime/Source/Core/Math/Hash"));

        // Prebuilt libclang and LLVM, vendored under the engine's External directory.
        PublicSystemIncludePaths.Add(ModulePath("../../../External/LLVM/include"));
        PublicLibraryPaths.Add(ModulePath("../../../External/LLVM/lib"));
        PublicLibraryPaths.Add(ModulePath("../../../External/LLVM/bin"));

        // Windows has no rpath so libclang.dll must sit beside the exe. ELF must not stage libLLVM into Binaries because its RUNPATH would then miss the bundled libs.
        if (Target.Platform == BuildPlatform.Windows64)
        {
            AddRuntimeDependency("../../../External/LLVM/bin/libclang.dll");
        }

        if (Target.Platform != BuildPlatform.Windows64)
        {
            // The bundle carries LLVM's own dependencies because current distributions ship newer libxml2 than the packaged LLVM asks for.
            PrivateLinkerOptions.Add($"-Wl,-rpath-link,{ModulePath("../../../External/LLVM/lib")}");
            PrivateLinkerOptions.Add("-Wl,-rpath,$ORIGIN/../../External/LLVM/lib");

            // DT_RPATH is inherited down the dependency chain but RUNPATH covers only direct dependencies, so libLLVM's libxml2 would not resolve.
            PrivateLinkerOptions.Add("-Wl,--disable-new-dtags");

            // A loaded Reflection module resolves the generator API against the executable's own symbols.
            PrivateLinkerOptions.Add("-rdynamic");
        }

        // Only libclang's stable C API, so the Reflector never depends on LLVM's internal layout.
        PublicSystemLibraries.Add(Target.Platform == BuildPlatform.Windows64 ? "libclang" : "clang");
    }
}
