using System.IO;
using LuminaBuildTool.Configuration;

/// <summary>Base class for a game project's C++ module.</summary>
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

        // The minimum set a game translation unit needs for the engine's template instantiations.
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "ImGui",
            "RPMalloc",
            "Tracy",
        });
    }
}

/// <summary>Base class for a game project's target.</summary>
public abstract class LuminaGameTargetRules : LuminaTargetRules
{
    protected LuminaGameTargetRules(TargetInfo Target)
        : base(Target)
    {
        Type = Target.Type;
        PreBuildTargetNames.Add("Reflector");

        // A project builds a library; without the engine's application there is nothing to run it, and no
        // managed engine assembly, so C# scripting comes up silently dead.
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

        // Through OutputSuffix so an editor target launches the editor app rather than the game one.
        DebuggerCommand = Path.Combine(
            EngineBinaries,
            $"Lumina{OutputSuffix}{Target.Platform.GetExecutableExtension()}");

        DebuggerWorkingDirectory = EngineBinaries;
    }

    /// <summary>Points the IDE's run command at the editor with this project's .lproject opened.</summary>
    protected void SetProjectFileToOpen(string ProjectFilePath)
    {
        DebuggerArguments = $"--Project=\"{TargetPath(ProjectFilePath)}\"";
    }
}
