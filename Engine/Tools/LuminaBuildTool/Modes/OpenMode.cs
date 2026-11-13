using System.Diagnostics;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Platform;
using LuminaBuildTool.Rules;

namespace LuminaBuildTool.Modes;

// Opens a .lproject in the editor, or launches it as a game, building first so a stale checkout never runs old binaries.
public static class OpenMode
{
    private const string EngineTargetName = "Lumina";

    private static readonly TimeSpan EditorCloseTimeout = TimeSpan.FromMinutes(2);

    public static async Task<int> RunAsync(CommandLine Arguments, BuildDirectories ProjectDirectories, TargetType TypeValue, CancellationToken Cancellation)
    {
        string? ProjectRoot = ProjectDirectories.ProjectRoot;
        string? ProjectFile = ProjectRoot is null ? null : EngineAssociation.FindProjectFile(ProjectRoot);
        if (ProjectRoot is null || ProjectFile is null)
        {
            throw new BuildException($"{Arguments.GetPositional(0)} requires a project file. Usage: LuminaBuildTool {Arguments.GetPositional(0)} <Path/To/Game.lproject>");
        }

        if (Arguments.HasFlag("WaitForEditor"))
        {
            await WaitForEditorToCloseAsync(ProjectRoot, Cancellation).ConfigureAwait(false);
        }

        BuildDirectories EngineDirectories = ProjectDirectories.ToEngineOnly();
        BuildPlatform PlatformValue = BuildPlatformRegistry.HostPlatform;
        BuildConfiguration ConfigurationValue = Arguments.GetEnum("Configuration", BuildConfiguration.Development);
        RulesAssembly Assembly = RulesAssembly.Create(ProjectDirectories, Arguments.HasFlag("RecompileRules"));

        // The open editor holds the project's binaries, and gets focus from the one launched below.
        if (TypeValue == TargetType.Editor && IsEditorOpen(ProjectRoot))
        {
            Log.Info("{0} is already open in the editor, so it is not rebuilt.", Path.GetFileName(ProjectFile));
        }
        else
        {
            // A script-only project has no target of its own and needs only the engine, built as the engine's so it lands there.
            string ProjectTargetName = Path.GetFileNameWithoutExtension(ProjectFile);
            bool bHasProjectTarget = Assembly.HasTarget(ProjectTargetName);
            string BuildTargetName = bHasProjectTarget ? ProjectTargetName : EngineTargetName;

            Log.Info("Building {0} before running {1}...", BuildTargetName, Path.GetFileName(ProjectFile));

            int Result = await BuildMode.BuildTargetsAsync(
                new[] { BuildTargetName },
                Arguments,
                bHasProjectTarget ? ProjectDirectories : EngineDirectories,
                Assembly,
                TypeValue,
                PlatformValue,
                ConfigurationValue,
                Cancellation).ConfigureAwait(false);

            if (Result != 0)
            {
                Log.Error("{0} did not build, so nothing was started.", BuildTargetName);
                return Result;
            }
        }

        RunMode.LaunchCommand Command = RunMode.ResolveLaunch(
            Assembly, EngineDirectories, Arguments, EngineTargetName, TypeValue, PlatformValue, ConfigurationValue);
        Command.Arguments.Add($"--Project={ProjectFile}");

        // Its own log, or a game started beside the editor rotates the file the editor is writing.
        if (TypeValue == TargetType.Game)
        {
            Command.Arguments.Add("--logfile=Standalone");
        }

        return RunMode.Launch(Command, bWaitForExit: false);
    }

    // Every editor with the project open holds this file, and the OS lets go of it when that editor exits.
    private static bool IsEditorOpen(string ProjectRoot)
    {
        string LockFile = Path.Combine(ProjectRoot, "Intermediates", "Editor.lock");
        if (!File.Exists(LockFile))
        {
            return false;
        }

        try
        {
            using FileStream Probe = new(LockFile, FileMode.Open, FileAccess.Write, FileShare.Read);
            return false;
        }
        catch (IOException)
        {
            return true;
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }

    private static async Task WaitForEditorToCloseAsync(string ProjectRoot, CancellationToken Cancellation)
    {
        Stopwatch Waited = Stopwatch.StartNew();
        while (IsEditorOpen(ProjectRoot))
        {
            if (Waited.Elapsed > EditorCloseTimeout)
            {
                throw new BuildException("The editor did not close, so its binaries cannot be rebuilt. Close it and try again.");
            }

            await Task.Delay(250, Cancellation).ConfigureAwait(false);
        }
    }
}
