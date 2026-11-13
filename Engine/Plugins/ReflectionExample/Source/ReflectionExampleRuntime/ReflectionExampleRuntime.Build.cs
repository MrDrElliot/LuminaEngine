using LuminaBuildTool.Configuration;

public class ReflectionExampleRuntime : LuminaModuleRules
{
    public ReflectionExampleRuntime(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;
        HostType = ModuleHostType.Runtime;

        PublicIncludePaths.Add(".");

        PublicDependencyModuleNames.Add("Runtime");
    }
}
