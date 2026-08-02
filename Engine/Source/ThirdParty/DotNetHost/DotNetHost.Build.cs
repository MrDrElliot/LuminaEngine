using LuminaBuildTool.Configuration;

public class DotNetHost : LuminaThirdPartyModuleRules
{
    public DotNetHost(TargetInfo Target)
        : base(Target)
    {
        // Headers only: FDotNetHost resolves hostfxr with LoadLibrary at run time, so there is
        // no import library to link.
        BinaryType = ModuleBinaryType.HeaderOnly;
        PublicIncludePaths.Add(ModulePath("../../../../External/DotNet/include"));
    }
}
