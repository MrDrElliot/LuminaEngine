using LuminaBuildTool.Configuration;

public class NetworkingEditor : LuminaModuleRules
{
    public NetworkingEditor(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;
        HostType = ModuleHostType.Editor;

        PublicIncludePaths.Add(".");

        PublicDependencyModuleNames.Add("Runtime");
        PublicDependencyModuleNames.Add("Editor");

        // The netcode this tooling inspects. Nothing in the engine's own Editor module depends on it.
        PublicDependencyModuleNames.Add("NetworkingRuntime");

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "ImGui",
            "RPMalloc",
            "EA",
            "Entt",
            "Tracy",
        });
    }
}
