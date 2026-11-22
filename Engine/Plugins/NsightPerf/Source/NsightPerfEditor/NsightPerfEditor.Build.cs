using LuminaBuildTool.Configuration;

public class NsightPerfEditor : LuminaModuleRules
{
    public NsightPerfEditor(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;
        HostType = ModuleHostType.Editor;

        // No reflected types here, and skipping the generator keeps it from parsing the heavy NvPerf headers.
        bEnableReflection = false;

        PublicIncludePaths.Add(".");

        // Editor is public because NsightPerfTool.h, a public header, derives from FEditorTool.
        PublicDependencyModuleNames.AddRange(new[] { "Runtime", "Editor" });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "ImGui",
            "RPMalloc",
            "Volk",
            "NsightPerf",
        });

        // Carries the rapidyaml implementation, so it cannot share a translation unit with anything else.
        ExcludeFromUnity.Add("RymlImpl.cpp");

        // Force-included because this module has no engine PCH to prime the ECS headers.
        ForceIncludeFiles.Add("World/ECS/Registry.h");
    }
}
