using LuminaBuildTool.Configuration;

public class NsightPerf : LuminaThirdPartyModuleRules
{
    public NsightPerf(TargetInfo Target)
        : base(Target)
    {
        // Vendored NVIDIA SDK: prebuilt import library plus a runtime DLL.
        BinaryType = ModuleBinaryType.HeaderOnly;

        // The NvPerf headers include each other by bare name, so every include root has to be
        // on the path rather than just the top one.
        PublicIncludePaths.Add("include");
        PublicIncludePaths.Add("include/windows-desktop-x64");
        PublicIncludePaths.Add("NvPerfUtility/include");

        // NvPerfUtility's HUD and config parsing depend on rapidyaml, vendored as a single-header
        // amalgamation.
        PublicIncludePaths.Add("imports/rapidyaml-0.4.0");

        PublicLibraryPaths.Add(ModulePath("lib"));
        PublicSystemLibraries.Add("nvperf_grfx_host");

        // Copied next to the executable so the loader resolves it when the plugin DLL imports it.
        AddRuntimeDependency("lib/nvperf_grfx_host.dll", bOptional: true);
    }
}
