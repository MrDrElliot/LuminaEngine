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

        AddPerFileOption("CpuCheck.cpp", Target.Platform == BuildPlatform.Windows64 ? "/arch:AVX" : "-march=x86-64");

        if (Target.bWithEditor)
        {
            PublicDependencyModuleNames.Add("Editor");
        }
    }
}
