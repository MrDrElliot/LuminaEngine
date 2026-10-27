using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text.Json;
using System.Text.RegularExpressions;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Execution;

namespace LuminaBuildTool.Modes;

// Runs clang-tidy with the workspace .clang-tidy over the first-party sources in compile_commands.json, reporting each finding once.
public static partial class TidyMode
{
    private sealed record Finding(string File, int Line, int Column, string Severity, string Check);

    [GeneratedRegex(@"^(?<File>.+?):(?<Line>\d+):(?<Column>\d+): (?<Severity>warning|error): (?<Message>.*?) \[(?<Check>[^\]]+)\]$")]
    private static partial Regex DiagnosticPattern();

    public static async Task<int> RunAsync(CommandLine Arguments, BuildDirectories Directories, CancellationToken Cancellation)
    {
        string Database = Path.Combine(Directories.OutputRoot, "compile_commands.json");
        if (!File.Exists(Database))
        {
            Log.Error("{0} is missing. GenerateProjectFiles writes it.", Database);
            return 1;
        }

        string? Tool = LocateClangTidy(Arguments);
        if (Tool is null)
        {
            Log.Error("clang-tidy was not found. Put LLVM's bin on PATH, set LUMINA_CLANG_TIDY, or pass -ClangTidy=<path>.");
            return 1;
        }

        string PathFilter = (Arguments.GetPositional(1) ?? string.Empty).Replace(Path.AltDirectorySeparatorChar, Path.DirectorySeparatorChar);
        List<string> Sources = ReadSources(Database, Directories, PathFilter);
        if (Sources.Count == 0)
        {
            Log.Error("No first-party sources in {0} match '{1}'.", Database, PathFilter);
            return 1;
        }

        string? Checks = Arguments.GetString("Checks");
        string ChecksArgument = Checks is null ? string.Empty : $" --checks={Checks}";
        int Jobs = Math.Max(1, Arguments.GetInt("Jobs", Environment.ProcessorCount));
        Log.Info("clang-tidy over {0} source(s) on {1} job(s) with {2}.", Sources.Count, Jobs, Tool);

        // Keyed on place and check, since a header's warning comes back from every source that includes it, worded slightly differently by each.
        ConcurrentDictionary<Finding, string> Findings = new();
        Stopwatch Watch = Stopwatch.StartNew();
        int Completed = 0;

        await Parallel.ForEachAsync(Sources, new ParallelOptions { MaxDegreeOfParallelism = Jobs, CancellationToken = Cancellation }, async (Source, Token) =>
        {
            ProcessResult Result = await ProcessRunner.RunAsync(Tool, $"-p \"{Directories.OutputRoot}\" --quiet{ChecksArgument} \"{Source}\"",
                Directories.OutputRoot, null, Token).ConfigureAwait(false);

            foreach (string Line in Result.Output.Split('\n'))
            {
                Match Diagnostic = DiagnosticPattern().Match(Line.TrimEnd('\r'));
                if (!Diagnostic.Success)
                {
                    continue;
                }

                string FilePath = Path.GetFullPath(Diagnostic.Groups["File"].Value);
                if (IsExternal(FilePath, Directories))
                {
                    continue;
                }

                Findings.TryAdd(new Finding(FilePath, int.Parse(Diagnostic.Groups["Line"].Value), int.Parse(Diagnostic.Groups["Column"].Value),
                    Diagnostic.Groups["Severity"].Value, Diagnostic.Groups["Check"].Value), Diagnostic.Groups["Message"].Value);
            }

            int Done = Interlocked.Increment(ref Completed);
            if (Done % 100 == 0)
            {
                Log.Info("  {0}/{1} sources, {2} finding(s) so far.", Done, Sources.Count, Findings.Count);
            }
        }).ConfigureAwait(false);

        List<Finding> Sorted = Findings.Keys
            .OrderBy(F => F.File, StringComparer.OrdinalIgnoreCase)
            .ThenBy(F => F.Line)
            .ThenBy(F => F.Column)
            .ToList();

        // The MSVC shape, so an IDE's error list picks these up from a build log like any compiler warning.
        foreach (Finding Item in Sorted)
        {
            Log.Raw($"{Item.File}({Item.Line},{Item.Column}): {Item.Severity} {Item.Check}: {Findings[Item]}");
        }

        int Errors = Sorted.Count(F => F.Severity == "error");
        Log.Info("{0} finding(s) in {1} file(s), {2} of them errors, from {3} source(s) in {4:F1}s.",
            Sorted.Count, Sorted.Select(F => F.File).Distinct(StringComparer.OrdinalIgnoreCase).Count(), Errors, Sources.Count, Watch.Elapsed.TotalSeconds);

        return Arguments.HasFlag("WarningsAsErrors") && Sorted.Count > 0 ? 1 : 0;
    }

    private static List<string> ReadSources(string Database, BuildDirectories Directories, string PathFilter)
    {
        using FileStream Stream = File.OpenRead(Database);
        using JsonDocument Document = JsonDocument.Parse(Stream);

        HashSet<string> Seen = new(StringComparer.OrdinalIgnoreCase);
        List<string> Sources = new();
        foreach (JsonElement Entry in Document.RootElement.EnumerateArray())
        {
            string Directory = Entry.GetProperty("directory").GetString() ?? string.Empty;
            string FilePath = Path.GetFullPath(Path.Combine(Directory, Entry.GetProperty("file").GetString() ?? string.Empty));
            if (IsExternal(FilePath, Directories) || !FilePath.Contains(PathFilter, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }
            if (Seen.Add(FilePath))
            {
                Sources.Add(FilePath);
            }
        }
        return Sources;
    }

    // Vendored and generated code is neither checked nor reported, matching ExcludeHeaderFilterRegex in .clang-tidy.
    private static readonly string[] ExternalDirectories = { "ThirdParty", "External", "Intermediates" };

    private static bool IsExternal(string FilePath, BuildDirectories Directories)
    {
        return ExternalDirectories.Any(Name => FilePath.Contains($"{Path.DirectorySeparatorChar}{Name}{Path.DirectorySeparatorChar}", StringComparison.OrdinalIgnoreCase));
    }

    private static string? LocateClangTidy(CommandLine Arguments)
    {
        string Executable = OperatingSystem.IsWindows() ? "clang-tidy.exe" : "clang-tidy";

        foreach (string? Candidate in new[] { Arguments.GetString("ClangTidy"), Environment.GetEnvironmentVariable("LUMINA_CLANG_TIDY") })
        {
            if (!string.IsNullOrEmpty(Candidate) && File.Exists(Candidate))
            {
                return Candidate;
            }
        }

        foreach (string Directory in (Environment.GetEnvironmentVariable("PATH") ?? string.Empty).Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            string Candidate = Path.Combine(Directory.Trim('"'), Executable);
            if (File.Exists(Candidate))
            {
                return Candidate;
            }
        }

        // The one Visual Studio installs with its optional Clang tools, under <Version>/<Edition>/VC/Tools/Llvm.
        string VisualStudio = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Microsoft Visual Studio");
        if (OperatingSystem.IsWindows() && System.IO.Directory.Exists(VisualStudio))
        {
            foreach (string Version in System.IO.Directory.EnumerateDirectories(VisualStudio))
            {
                foreach (string Edition in System.IO.Directory.EnumerateDirectories(Version))
                {
                    string Candidate = Path.Combine(Edition, "VC", "Tools", "Llvm", "x64", "bin", Executable);
                    if (File.Exists(Candidate))
                    {
                        return Candidate;
                    }
                }
            }
        }

        return null;
    }
}
