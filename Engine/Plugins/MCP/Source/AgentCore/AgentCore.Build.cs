using LuminaBuildTool.Configuration;

public class AgentCore : LuminaModuleRules
{
    public AgentCore(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;
        HostType = ModuleHostType.Editor;

        PrecompiledHeader = new PrecompiledHeaderRules("AgentCorePCH.h", "AgentCorePCH.cpp");

        PublicIncludePaths.Add(".");

        PublicDependencyModuleNames.AddRange(new[] { "Runtime" });

        // Editor is reached by the two tools that transact an edit through an open asset editor.
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Editor",
            "RPMalloc",
        });
    }
}
