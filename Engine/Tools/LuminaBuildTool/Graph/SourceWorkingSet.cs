using System.Diagnostics;
using System.Text;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Graph;

/// <summary>The sources a developer currently has changed, read from git so two checkouts of one commit agree.</summary>
/// <remarks>Queried once per repository per process, since the answer steers every module's blobs.</remarks>
public static class SourceWorkingSet
{
    private const int QueryTimeoutMs = 10_000;

    private static readonly Dictionary<string, HashSet<string>?> ChangedByRepository = new(StringComparer.OrdinalIgnoreCase);
    private static readonly Dictionary<string, string?> RootByDirectory = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Absolute paths changed against the checked-out commit, or null when git cannot answer for this directory.</summary>
    public static IReadOnlySet<string>? For(string AnyDirectory)
    {
        string? Root = RepositoryRootOf(AnyDirectory);

        if (Root is null)
        {
            return null;
        }

        if (!ChangedByRepository.TryGetValue(Root, out HashSet<string>? Changed))
        {
            Changed = QueryChangedFiles(Root);
            ChangedByRepository[Root] = Changed;

            if (Changed is not null)
            {
                Log.Verbose("Working set: '{0}' reports {1} changed file(s).", Root, Changed.Count);
            }
        }

        return Changed;
    }

    private static string? RepositoryRootOf(string AnyDirectory)
    {
        string Key = PathUtils.Normalize(AnyDirectory);

        if (RootByDirectory.TryGetValue(Key, out string? Cached))
        {
            return Cached;
        }

        string? Root = null;

        if (RunGit(Key, "rev-parse --show-toplevel", out string Output) && Output.Length > 0)
        {
            Root = PathUtils.Normalize(Output.Trim());
        }

        RootByDirectory[Key] = Root;
        return Root;
    }

    private static HashSet<string>? QueryChangedFiles(string Root)
    {
        // NUL separated and rename free, so a path needs no unquoting and every record is one entry.
        if (!RunGit(Root, "status --porcelain -uall --no-renames -z", out string Output))
        {
            return null;
        }

        HashSet<string> Changed = new(StringComparer.OrdinalIgnoreCase);

        foreach (string Record in Output.Split('\0', StringSplitOptions.RemoveEmptyEntries))
        {
            // Two status letters and a space, then the path relative to the repository root.
            if (Record.Length <= 3)
            {
                continue;
            }

            Changed.Add(PathUtils.Normalize(Path.Combine(Root, Record[3..])));
        }

        return Changed;
    }

    private static bool RunGit(string WorkingDirectory, string Arguments, out string Output)
    {
        Output = string.Empty;

        if (!Directory.Exists(WorkingDirectory))
        {
            return false;
        }

        try
        {
            ProcessStartInfo Info = new("git", Arguments)
            {
                WorkingDirectory = WorkingDirectory,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
                StandardOutputEncoding = Encoding.UTF8,
            };

            using Process? Git = Process.Start(Info);

            if (Git is null)
            {
                return false;
            }

            Output = Git.StandardOutput.ReadToEnd();
            Git.StandardError.ReadToEnd();

            if (!Git.WaitForExit(QueryTimeoutMs))
            {
                Git.Kill(true);
                return false;
            }

            return Git.ExitCode == 0;
        }
        catch (Exception Ex) when (Ex is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            // No git on PATH, or no working tree. Adaptive unity falls back to file timestamps.
            Log.Verbose("Working set: git is unavailable in '{0}': {1}", WorkingDirectory, Ex.Message);
            return false;
        }
    }
}
