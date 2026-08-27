using LuminaBuildTool.Configuration;

public class GameplayExtrasRuntime : LuminaModuleRules
{
    public GameplayExtrasRuntime(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;
        HostType = ModuleHostType.Runtime;

        PublicIncludePaths.Add(".");

        PublicDependencyModuleNames.Add("Runtime");

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "ImGui",
            "RPMalloc",
        });
    }
}
