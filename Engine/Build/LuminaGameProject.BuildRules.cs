using System.IO;
using LuminaBuildTool.Configuration;

/// <summary>
/// Base class for a game project's C++ module. The engine is built from source alongside it and
/// keeps its output in the engine tree, so one engine build serves every project.
/// </summary>
public abstract class LuminaGameModuleRules : LuminaModuleRules
{
    protected LuminaGameModuleRules(TargetInfo Target)
        : base(Target)
    {
        BinaryType = ModuleBinaryType.SharedLibrary;
        bEnableReflection = true;

        PublicIncludePaths.Add("Source");

        PublicDependencyModuleNames.Add("Runtime");

        if (Target.bWithEditor)
        {
            PublicDependencyModuleNames.Add("Editor");
        }

        // The minimum third-party set a game translation unit needs to satisfy the engine's
        // template instantiations. The engine's public headers expose more, but those are either
        // header only or already absorbed into the engine binaries.
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

/// <summary>
/// Base class for a game project's target. Adds the engine's reflection generator as a
/// prerequisite and routes generated C# bindings into the project's own script assembly.
/// </summary>
public abstract class LuminaGameTargetRules : LuminaTargetRules
{
    protected LuminaGameTargetRules(TargetInfo Target)
        : base(Target)
    {
        Type = Target.Type;
        PreBuildTargetNames.Add("Reflector");

        // This target builds a library, and the thing that loads it is the engine's application,
        // which lives in a target of its own. Building a project has to build that too or there is
        // nothing to run afterwards; it also carries the managed engine assembly the editor loads
        // at startup, so without it C# scripting comes up silently dead.
        RequiredTargetNames.Add("Lumina");

        // A project reads the engine's reflection manifest; publishing one would replace the
        // engine's list with one that includes this project's own modules.
        bPublishesEngineReflectionManifest = false;

        // A game module loads into the editor process, so it stays a shared library even in
        // Shipping; only a packaged standalone game links monolithically.
        bMonolithic = false;

        // This target builds a library the editor loads, so running it means running the editor
        // with this project opened rather than launching the library.
        string EngineBinaries = Path.Combine(Target.EngineDirectory, "Binaries", Target.PlatformName);

        DebuggerCommand = Path.Combine(EngineBinaries, $"Lumina-{Target.Configuration}.exe");
        DebuggerWorkingDirectory = EngineBinaries;
    }

    /// <summary>
    /// Points the IDE's run command at the editor with this project's .lproject opened.
    /// </summary>
    protected void SetProjectFileToOpen(string ProjectFilePath)
    {
        DebuggerArguments = $"--Project=\"{TargetPath(ProjectFilePath)}\"";
    }
}
