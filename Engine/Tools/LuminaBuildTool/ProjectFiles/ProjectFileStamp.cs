using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Rules;

namespace LuminaBuildTool.ProjectFiles;

/// <summary>Record of the rules the project files on disk were generated from.</summary>
public sealed class ProjectFileStampData
{
    public string RulesHash { get; set; } = string.Empty;

    public string ToolVersion { get; set; } = string.Empty;
}

/// <summary>Tracks whether generated project files still describe what the rules say.</summary>
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

    /// <summary>True when project files exist and no longer match the rules.</summary>
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
