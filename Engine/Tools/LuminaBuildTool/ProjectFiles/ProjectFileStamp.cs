using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Rules;

namespace LuminaBuildTool.ProjectFiles;

/// <summary>
/// Record of the rules the project files on disk were generated from.
/// </summary>
public sealed class ProjectFileStampData
{
    public string RulesHash { get; set; } = string.Empty;

    public string ToolVersion { get; set; } = string.Empty;
}

/// <summary>
/// Tracks whether generated project files still describe what the rules say.
/// </summary>
/// <remarks>
/// A project file bakes in the answers the rules gave when it was written: include paths,
/// definitions and the module's source list. The build re-reads Build.cs every time and so is
/// never wrong, but the IDE reads the project file, so editing a Build.cs leaves completion and
/// error checking describing the previous state with nothing to say so. Recording what generation
/// saw lets a build notice the gap and close it, rather than leaving it to be rediscovered as
/// includes that resolve for the compiler and not for the editor.
/// </remarks>
public static class ProjectFileStamp
{
    private static string GetPath(BuildDirectories Directories)
    {
        return Path.Combine(Directories.ProjectFilesDirectory, "ProjectFiles.manifest.json");
    }

    public static void Write(BuildDirectories Directories, RulesAssembly Assembly)
    {
        JsonStore.Save(GetPath(Directories), new ProjectFileStampData
        {
            RulesHash = Assembly.SourceHash,
            ToolVersion = RulesCompiler.GetToolVersion(),
        });
    }

    /// <summary>
    /// True when project files exist and no longer match the rules. False when none have been
    /// generated: a headless or CI build asked for binaries, not an IDE workspace, and should not
    /// have one appear underneath it.
    /// </summary>
    public static bool IsStale(BuildDirectories Directories, RulesAssembly Assembly)
    {
        if (!Directory.Exists(Directories.ProjectFilesDirectory))
        {
            return false;
        }

        ProjectFileStampData? Stamp = JsonStore.Load<ProjectFileStampData>(GetPath(Directories));

        // No stamp beside existing project files means they predate this check, so they are
        // treated as stale once and stamped on the way through.
        return Stamp is null
            || Stamp.RulesHash != Assembly.SourceHash
            || Stamp.ToolVersion != RulesCompiler.GetToolVersion();
    }
}
