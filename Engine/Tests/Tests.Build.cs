using LuminaBuildTool.Configuration;

public class Tests : LuminaModuleRules
{
    public Tests(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.ConsoleApplication;
        HostType = ModuleHostType.Developer;

        bEnableReflection = false;

        PrivateIncludePaths.Add(".");

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Runtime",
            "GoogleTest",
            "Box3D",
            "ImGui",
            "RPMalloc",
            "Entt",
        });

        if (Target.bWithEditor)
        {
            PrivateDependencyModuleNames.Add("Editor");
        }
    }
}
