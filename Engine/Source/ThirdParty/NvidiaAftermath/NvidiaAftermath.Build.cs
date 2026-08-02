using LuminaBuildTool.Configuration;

public class NvidiaAftermath : LuminaThirdPartyModuleRules
{
    public NvidiaAftermath(TargetInfo Target)
        : base(Target)
    {
        // GPU crash dump SDK; prebuilt import library plus a DLL loaded from beside the executable.
        BinaryType = ModuleBinaryType.HeaderOnly;

        // Included as <NvidiaAftermath/GFSDK_Aftermath.h>, so the ThirdParty root is the path.
        PublicIncludePaths.Add("..");

        PublicLibraryPaths.Add(ModulePath("lib"));
        PublicSystemLibraries.Add("GFSDK_Aftermath_Lib");

        AddRuntimeDependency("lib/GFSDK_Aftermath_Lib.x64.dll", bOptional: true);
    }
}
