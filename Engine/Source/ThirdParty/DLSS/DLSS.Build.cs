using LuminaBuildTool.Configuration;

public class DLSS : LuminaThirdPartyModuleRules
{
    public DLSS(TargetInfo Target)
        : base(Target)
    {
        // NVIDIA DLSS SDK 310.3.0, the NGX static library plus the DLSS runtime it loads from beside the executable.
        BinaryType = ModuleBinaryType.HeaderOnly;

        PublicIncludePaths.Add("include");

        // The _d libraries link the dynamic CRT, and the _dbg one matches the debug CRT a Debug build uses.
        PublicLibraryPaths.Add(ModulePath("lib"));
        PublicSystemLibraries.Add(Target.bDebug ? "nvsdk_ngx_d_dbg" : "nvsdk_ngx_d");

        AddRuntimeDependency("lib/nvngx_dlss.dll", bOptional: true);
    }
}
