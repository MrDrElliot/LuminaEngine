using LuminaBuildTool.Configuration;

public class Lumina : LuminaModuleRules
{
    public Lumina(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.WindowedApplication;

        // The launcher owns no reflected types of its own.
        bEnableReflection = false;

        PublicDependencyModuleNames.Add("Runtime");

        if (Target.bWithEditor)
        {
            PublicDependencyModuleNames.Add("Editor");
        }
    }
}
