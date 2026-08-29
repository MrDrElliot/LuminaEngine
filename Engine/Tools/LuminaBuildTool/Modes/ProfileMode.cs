using System.Diagnostics;
using System.Globalization;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Platform;
using LuminaBuildTool.Rules;

namespace LuminaBuildTool.Modes;

// One zone's self time across a capture, which is what csvexport reports with -e.
public sealed record ProfileEntry(string Name, string Source, double SelfNanoseconds, long Count, double MeanNanoseconds)
{
    public double SelfMilliseconds => SelfNanoseconds / 1_000_000.0;

    // Spread across the repeats behind this entry, which is the noise a change has to beat.
    public double SpreadPercent { get; init; }

    // What the zone cost including everything nested in it. Equal to self for a CPU zone.
    public double InclusiveNanoseconds { get; init; }
}

// Builds a target, captures a Tracy trace from a live run, and ranks its zones by self time.
public static class ProfileMode
{
    private const string CaptureToolName = "tracy-capture";

    private const string ExportToolName = "tracy-csvexport";

    // Not a comma: a zone name is a C++ signature and template arguments carry their own.
    private const string FieldSeparator = ";";

    // Printed by Benchmark::Tick once the warmup frames are done and the requested map is confirmed loaded.
    private const string WarmupMarker = "warmup complete";

    public static async Task<int> RunAsync(CommandLine Arguments, BuildDirectories Directories, CancellationToken Cancellation)
    {
        string? TargetName = Arguments.GetPositional(1);

        if (string.IsNullOrEmpty(TargetName))
        {
            throw new BuildException("Profile requires a target name. Usage: LuminaBuildTool Profile <Target> [options]");
        }

        BuildPlatform PlatformValue = Arguments.GetEnum("Platform", BuildPlatformRegistry.HostPlatform);

        if (PlatformValue != BuildPlatformRegistry.HostPlatform)
        {
            throw new BuildException($"Cannot profile a {PlatformValue} binary on {BuildPlatformRegistry.HostPlatform}.");
        }

        BuildConfiguration ConfigurationValue = Arguments.GetEnum("Configuration", BuildConfiguration.Development);
        TargetType TypeValue = Arguments.GetEnum("TargetType", TargetType.Editor);

        // Shipping strips the instrumentation the capture reads, so there would be nothing to record.
        if (ConfigurationValue == BuildConfiguration.Shipping)
        {
            throw new BuildException("A Shipping build carries no Tracy zones. Profile Development or Debug.");
        }

        string CaptureTool = ResolveTool(Directories, CaptureToolName);
        string ExportTool = ResolveTool(Directories, ExportToolName);

        RulesAssembly Assembly = RulesAssembly.Create(Directories, Arguments.HasFlag("RecompileRules"));

        if (!Arguments.HasFlag("NoBuild"))
        {
            int BuildResult = await BuildMode.BuildTargetsAsync(
                new[] { TargetName },
                Arguments,
                Directories,
                Assembly,
                TypeValue,
                PlatformValue,
                ConfigurationValue,
                Cancellation).ConfigureAwait(false);

            if (BuildResult != 0)
            {
                return BuildResult;
            }
        }

        IBuildPlatform PlatformSupport = BuildPlatformRegistry.Get(PlatformValue);
        BuildOptions Options = BuildOptions.Load(Directories, Arguments);

        TargetInfo Info = new(TargetName, TypeValue, PlatformValue, ConfigurationValue, Directories, Options);
        BuildTarget Target = new TargetAssembler(Assembly, Directories, PlatformSupport).Assemble(TargetName, Info);

        string Executable = Target.Rules.DebuggerCommand.Length > 0
            ? Target.Rules.DebuggerCommand
            : Target.LaunchModule?.OutputFile ?? string.Empty;

        if (Executable.Length == 0 || !File.Exists(Executable))
        {
            throw new BuildException($"Target '{TargetName}' has no executable to profile at '{Executable}'.");
        }

        int Seconds = Arguments.GetInt("Seconds", 15);
        string OutputDirectory = Path.Combine(Directories.IntermediatesDirectory, "Profiling");
        Directory.CreateDirectory(OutputDirectory);

        bool bGpu = Arguments.HasFlag("Gpu");

        string? BenchmarkMap = Arguments.GetString("Benchmark");
        int Warmup = Math.Max(0, Arguments.GetInt("Warmup", 900));
        int Frames = Math.Max(1, Arguments.GetInt("Frames", 4000));

        if (Arguments.HasFlag("ClearShaderCache"))
        {
            string ShaderCache = Path.Combine(Directories.IntermediatesDirectory, "ShaderCache");
            if (Directory.Exists(ShaderCache))
            {
                Directory.Delete(ShaderCache, recursive: true);
                Log.Info("Cleared the shader cache, so this run pays the full compile cost.");
            }
        }

        // GPU and CPU numbers are different quantities, so they cannot share a baseline file.
        string RunName = $"{TargetName}-{TypeValue}-{ConfigurationValue}";
        string ResultName = bGpu ? RunName + "-gpu" : RunName;

        string TracePath = Path.Combine(OutputDirectory, RunName + ".tracy");
        string LatestPath = Path.Combine(OutputDirectory, ResultName + ".csv");
        string BaselinePath = Path.Combine(OutputDirectory, ResultName + ".baseline.csv");

        int Delay = Math.Max(0, Arguments.GetInt("Delay", 0));
        int Repeat = Math.Max(1, Arguments.GetInt("Repeat", 1));

        List<List<ProfileEntry>> Runs = CaptureRepeatedly(
            Executable, Target, Arguments, CaptureTool, ExportTool, TracePath, Seconds, Delay, Repeat, bGpu,
            BenchmarkMap, Warmup, Frames);

        if (Runs.Count == 0)
        {
            return 1;
        }

        List<ProfileEntry> Entries = Merge(Runs);
        File.WriteAllText(LatestPath, Serialize(Entries));

        if (Entries.Count == 0)
        {
            Log.Error(
                "The capture holds no zones. {0} is instrumented only where LUMINA_PROFILE_SCOPE appears, and a "
                + "run shorter than the capture window records nothing.",
                Path.GetFileName(Executable));
            return 1;
        }

        Report(Entries, BaselinePath, Arguments.GetInt("Top", 25), bGpu);

        if (Arguments.HasFlag("Baseline"))
        {
            File.Copy(LatestPath, BaselinePath, overwrite: true);
            Log.Info("Stored this run as the baseline for {0}.", ResultName);
        }
        else if (File.Exists(BaselinePath))
        {
            Log.Info("Compared against {0}. Pass -Baseline to replace it.", BaselinePath);
        }
        else
        {
            Log.Info("No baseline yet. Pass -Baseline to record this run as the one to compare against.");
        }

        Log.Info("Trace: {0}", TracePath);
        return 0;
    }

    private static string ResolveTool(BuildDirectories Directories, string ToolName)
    {
        string Extension = BuildPlatformRegistry.HostPlatform.GetExecutableExtension();
        string Path_ = Path.Combine(Directories.EngineRoot, "External", "Tracy", ToolName + Extension);

        if (!File.Exists(Path_))
        {
            throw new BuildException(
                $"'{Path_}' is missing. The Tracy command line tools ship in the dependency bundle; run Setup.");
        }

        return Path_;
    }

    // One run, several captures. Restarting the process between passes would put each capture at a
    // different point in startup, which is what made the repeats incomparable.
    private static List<List<ProfileEntry>> CaptureRepeatedly(
        string Executable,
        BuildTarget Target,
        CommandLine Arguments,
        string CaptureTool,
        string ExportTool,
        string TracePath,
        int Seconds,
        int Delay,
        int Repeat,
        bool bGpu,
        string? BenchmarkMap,
        int Warmup,
        int Frames)
    {
        List<List<ProfileEntry>> Runs = new();

        ProcessStartInfo RunInfo = new(Executable)
        {
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(Executable) ?? string.Empty,
        };

        if (Target.Rules.DebuggerArguments.Length > 0)
        {
            foreach (string Argument in RunMode.SplitArguments(Target.Rules.DebuggerArguments))
            {
                RunInfo.ArgumentList.Add(Argument);
            }
        }

        foreach (string Argument in Arguments.ForwardedArguments)
        {
            RunInfo.ArgumentList.Add(Argument);
        }

        // A frame count settles the world where a wall clock only guesses at it.
        if (BenchmarkMap is not null)
        {
            RunInfo.ArgumentList.Add($"-benchmark={BenchmarkMap}");
            RunInfo.ArgumentList.Add($"-warmup={Warmup}");
            RunInfo.ArgumentList.Add($"-frames={Frames}");
            RunInfo.RedirectStandardOutput = true;
        }

        Log.Info("Running {0}", Path.GetFileName(Executable));

        using Process? Run = Process.Start(RunInfo);

        if (Run is null)
        {
            Log.Error("Could not start '{0}'.", Executable);
            return Runs;
        }

        using ManualResetEventSlim WarmupDone = new(false);

        if (BenchmarkMap is not null)
        {
            Run.OutputDataReceived += (_, Event) =>
            {
                if (Event.Data is null)
                {
                    return;
                }

                Log.Verbose("{0}", Event.Data);

                if (Event.Data.Contains(WarmupMarker, StringComparison.Ordinal))
                {
                    WarmupDone.Set();
                }
            };

            Run.BeginOutputReadLine();
        }

        try
        {
            if (BenchmarkMap is not null)
            {
                Log.Info("Waiting for {0} warmup frames on {1}", Warmup, BenchmarkMap);

                if (!WarmupDone.Wait(TimeSpan.FromMinutes(10)))
                {
                    Log.Error(
                        "The run never reported warmup complete. Check that '{0}' is a map the project can "
                        + "open, and that -Warmup is below -Frames.",
                        BenchmarkMap);

                    return Runs;
                }

                Log.Info("Warmup done, the world has settled.");
            }

            // On-demand recording starts when the capture connects, so a delay is how startup and level
            // load stay out of the trace.
            if (Delay > 0)
            {
                Log.Info("Letting it settle for {0}s before connecting", Delay);
                Thread.Sleep(TimeSpan.FromSeconds(Delay));
            }

            for (int Pass = 0; Pass < Repeat; ++Pass)
            {
                if (Run.HasExited)
                {
                    Log.Error(
                        "The run ended after {0} of {1} captures. Raise -Frames so the measured window "
                        + "outlives every capture.",
                        Pass,
                        Repeat);

                    return Runs;
                }

                if (Repeat > 1)
                {
                    Log.Info("Capture {0} of {1}, {2}s", Pass + 1, Repeat, Seconds);
                }

                if (!CaptureOnce(CaptureTool, TracePath, Seconds, Delay))
                {
                    return new List<List<ProfileEntry>>();
                }

                string Exported = Export(ExportTool, TracePath, Arguments.GetString("Filter") ?? string.Empty, bGpu);
                List<ProfileEntry> Parsed = bGpu ? ParseGpuZones(Exported) : Parse(Exported);

                // Merging an empty pass silently yields an empty report, which reads as a total collapse.
                if (Parsed.Count == 0)
                {
                    Log.Error(
                        "Capture {0} of {1} recorded no zones, so the whole run is discarded. The workload "
                        + "most likely ended mid-capture; raise -Frames so the measured window outlives "
                        + "-Delay plus -Repeat times -Seconds.",
                        Pass + 1,
                        Repeat);

                    return new List<List<ProfileEntry>>();
                }

                Runs.Add(Parsed);
            }
        }
        finally
        {
            if (!Run.HasExited)
            {
                try
                {
                    Run.Kill(entireProcessTree: true);
                    Run.WaitForExit(5000);
                }
                catch (Exception Ex) when (Ex is InvalidOperationException or System.ComponentModel.Win32Exception)
                {
                    Log.Warning("Could not stop the profiled process: {0}", Ex.Message);
                }
            }
        }

        return Runs;
    }

    private static bool CaptureOnce(string CaptureTool, string TracePath, int Seconds, int Delay)
    {
        ProcessStartInfo CaptureInfo = new(CaptureTool) { UseShellExecute = false };
        CaptureInfo.ArgumentList.Add("-o");
        CaptureInfo.ArgumentList.Add(TracePath);
        CaptureInfo.ArgumentList.Add("-s");
        CaptureInfo.ArgumentList.Add(Seconds.ToString(CultureInfo.InvariantCulture));
        CaptureInfo.ArgumentList.Add("-f");

        using Process? Capture_ = Process.Start(CaptureInfo);

        if (Capture_ is null)
        {
            Log.Error("Could not start '{0}'.", CaptureTool);
            return false;
        }

        // It retries the connection forever, so a workload that ended before the connect landed would
        // leave it spinning with nothing to attach to.
        int BudgetSeconds = Delay + Seconds + 30;

        if (!Capture_.WaitForExit(BudgetSeconds * 1000))
        {
            Log.Error(
                "Capture did not finish within {0}s. The run most likely ended before the capture "
                + "connected; lower -Delay or give the workload more frames.",
                BudgetSeconds);

            try
            {
                Capture_.Kill(entireProcessTree: true);
            }
            catch (Exception Ex) when (Ex is InvalidOperationException or System.ComponentModel.Win32Exception)
            {
                Log.Warning("Could not stop the capture: {0}", Ex.Message);
            }

            return false;
        }

        if (Capture_.ExitCode != 0)
        {
            Log.Error("Capture failed with exit code {0}.", Capture_.ExitCode);
            return false;
        }

        return File.Exists(TracePath);
    }

    private static string Export(string ExportTool, string TracePath, string Filter, bool bGpu)
    {
        ProcessStartInfo Info = new(ExportTool)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
        };

        // Self time, which is what attributes cost to a zone rather than to everything it calls. The GPU
        // export has no such option, so nesting is resolved here instead.
        Info.ArgumentList.Add(bGpu ? "-g" : "-e");
        Info.ArgumentList.Add("-s");
        Info.ArgumentList.Add(FieldSeparator);

        if (Filter.Length > 0)
        {
            Info.ArgumentList.Add("-f");
            Info.ArgumentList.Add(Filter);
        }

        Info.ArgumentList.Add(TracePath);

        using Process? Export_ = Process.Start(Info)
            ?? throw new BuildException($"Could not start '{ExportTool}'.");

        string Output = Export_.StandardOutput.ReadToEnd();
        Export_.WaitForExit();

        if (Export_.ExitCode != 0)
        {
            throw new BuildException($"'{ExportTool}' failed with exit code {Export_.ExitCode}.");
        }

        return Output;
    }

    private static List<ProfileEntry> Parse(string Csv)
    {
        List<ProfileEntry> Entries = new();
        string[] Lines = Csv.Split('\n', StringSplitOptions.RemoveEmptyEntries);

        if (Lines.Length < 2)
        {
            return Entries;
        }

        string[] Header = Lines[0].Trim().Split(FieldSeparator);
        int NameColumn = Array.IndexOf(Header, "name");
        int SourceColumn = Array.IndexOf(Header, "src_file");
        int LineColumn = Array.IndexOf(Header, "src_line");
        int TotalColumn = Array.IndexOf(Header, "total_ns");
        int CountColumn = Array.IndexOf(Header, "counts");
        int MeanColumn = Array.IndexOf(Header, "mean_ns");

        if (NameColumn < 0 || TotalColumn < 0)
        {
            throw new BuildException("The exporter's CSV has no name or total_ns column; the tool version may differ.");
        }

        foreach (string Line in Lines.Skip(1))
        {
            string[] Fields = Line.Trim().Split(FieldSeparator);

            if (Fields.Length <= TotalColumn)
            {
                continue;
            }

            double Total = ParseNumber(Fields, TotalColumn);
            string Source = SourceColumn >= 0 && LineColumn >= 0 && Fields.Length > LineColumn
                ? $"{Path.GetFileName(Fields[SourceColumn])}:{Fields[LineColumn]}"
                : string.Empty;

            Entries.Add(new ProfileEntry(
                Fields[NameColumn],
                Source,
                Total,
                (long)ParseNumber(Fields, CountColumn),
                ParseNumber(Fields, MeanColumn)));
        }

        Entries.Sort((A, B) => B.SelfNanoseconds.CompareTo(A.SelfNanoseconds));
        return Entries;
    }

    // Median across the repeats, keeping only zones every pass saw. A zone that appears in some runs
    // and not others is not something a delta can be read from.
    private static List<ProfileEntry> Merge(List<List<ProfileEntry>> Runs)
    {
        if (Runs.Count == 1)
        {
            return Runs[0];
        }

        Dictionary<string, List<ProfileEntry>> ByName = new(StringComparer.Ordinal);

        foreach (List<ProfileEntry> Run in Runs)
        {
            foreach (ProfileEntry Entry in Run)
            {
                if (!ByName.TryGetValue(Entry.Name, out List<ProfileEntry>? Bucket))
                {
                    Bucket = new List<ProfileEntry>();
                    ByName[Entry.Name] = Bucket;
                }

                Bucket.Add(Entry);
            }
        }

        List<ProfileEntry> Merged = new();

        foreach (KeyValuePair<string, List<ProfileEntry>> Pair in ByName)
        {
            if (Pair.Value.Count != Runs.Count)
            {
                continue;
            }

            double[] Self = Pair.Value.Select(E => E.SelfNanoseconds).OrderBy(V => V).ToArray();
            double Low = Self[0];
            double High = Self[^1];

            Merged.Add(Pair.Value[0] with
            {
                SelfNanoseconds = Median(Self),
                Count = (long)Median(Pair.Value.Select(E => (double)E.Count).OrderBy(V => V).ToArray()),
                MeanNanoseconds = Median(Pair.Value.Select(E => E.MeanNanoseconds).OrderBy(V => V).ToArray()),
                SpreadPercent = Low > 0.0 ? (High - Low) / Low * 100.0 : 0.0,
            });
        }

        Merged.Sort((A, B) => B.SelfNanoseconds.CompareTo(A.SelfNanoseconds));
        return Merged;
    }

    private static double Median(double[] Sorted)
    {
        if (Sorted.Length == 0)
        {
            return 0.0;
        }

        int Middle = Sorted.Length / 2;
        return Sorted.Length % 2 == 1 ? Sorted[Middle] : (Sorted[Middle - 1] + Sorted[Middle]) / 2.0;
    }

    private static string Serialize(List<ProfileEntry> Entries)
    {
        System.Text.StringBuilder Builder = new();
        Builder.Append("name;src_file;src_line;total_ns;total_perc;counts;mean_ns;min_ns;max_ns;std_ns\n");

        foreach (ProfileEntry Entry in Entries)
        {
            string[] Source = Entry.Source.Split(':');
            string File_ = Source.Length > 0 ? Source[0] : string.Empty;
            string Line = Source.Length > 1 ? Source[1] : "0";

            Builder.Append(CultureInfo.InvariantCulture, $"{Entry.Name};{File_};{Line};");
            Builder.Append(CultureInfo.InvariantCulture, $"{Entry.SelfNanoseconds:F0};0;{Entry.Count};");
            Builder.Append(CultureInfo.InvariantCulture, $"{Entry.MeanNanoseconds:F0};0;0;0\n");
        }

        return Builder.ToString();
    }

    // The GPU export is one row per event with no parent link, so nesting is rebuilt from the intervals
    // and self time is the part of a pass not spent inside something nested in it.
    private static List<ProfileEntry> ParseGpuZones(string Csv)
    {
        string[] Lines = Csv.Split('\n', StringSplitOptions.RemoveEmptyEntries);

        if (Lines.Length < 2)
        {
            return new List<ProfileEntry>();
        }

        string[] Header = Lines[0].Trim().Split(FieldSeparator);
        int NameColumn = Array.IndexOf(Header, "name");
        int SourceColumn = Array.IndexOf(Header, "src_file");
        int StartColumn = Array.IndexOf(Header, "Time from start of program");
        int DurationColumn = Array.IndexOf(Header, "GPU execution time");

        if (NameColumn < 0 || StartColumn < 0 || DurationColumn < 0)
        {
            throw new BuildException("The GPU export has no start or duration column; the tool version may differ.");
        }

        List<(string Name, string Source, double Start, double Duration)> Events = new();

        foreach (string Line in Lines.Skip(1))
        {
            string[] Fields = Line.Trim().Split(FieldSeparator);

            if (Fields.Length <= Math.Max(StartColumn, DurationColumn))
            {
                continue;
            }

            Events.Add((
                Fields[NameColumn],
                SourceColumn >= 0 && Fields.Length > SourceColumn ? Path.GetFileName(Fields[SourceColumn]) : string.Empty,
                ParseNumber(Fields, StartColumn),
                ParseNumber(Fields, DurationColumn)));
        }

        // A parent opens no later than its child and closes no earlier, so this ordering makes the stack
        // walk see every parent before the zones inside it.
        Events.Sort((A, B) => A.Start != B.Start ? A.Start.CompareTo(B.Start) : B.Duration.CompareTo(A.Duration));

        double[] ChildTime = new double[Events.Count];
        List<int> Open = new();

        for (int Index = 0; Index < Events.Count; ++Index)
        {
            double Start = Events[Index].Start;

            while (Open.Count > 0 && Events[Open[^1]].Start + Events[Open[^1]].Duration <= Start)
            {
                Open.RemoveAt(Open.Count - 1);
            }

            if (Open.Count > 0)
            {
                ChildTime[Open[^1]] += Events[Index].Duration;
            }

            Open.Add(Index);
        }

        Dictionary<string, (double Self, double Inclusive, long Count, string Source)> ByName = new(StringComparer.Ordinal);

        for (int Index = 0; Index < Events.Count; ++Index)
        {
            (string Name, string Source, double _, double Duration) = Events[Index];
            double Self = Math.Max(0.0, Duration - ChildTime[Index]);

            ByName.TryGetValue(Name, out (double Self, double Inclusive, long Count, string Source) Existing);
            ByName[Name] = (Existing.Self + Self, Existing.Inclusive + Duration, Existing.Count + 1, Source);
        }

        List<ProfileEntry> Entries = new();

        foreach (KeyValuePair<string, (double Self, double Inclusive, long Count, string Source)> Pair in ByName)
        {
            Entries.Add(new ProfileEntry(
                Pair.Key,
                Pair.Value.Source,
                Pair.Value.Self,
                Pair.Value.Count,
                Pair.Value.Count > 0 ? Pair.Value.Inclusive / Pair.Value.Count : 0.0)
            {
                InclusiveNanoseconds = Pair.Value.Inclusive,
            });
        }

        Entries.Sort((A, B) => B.SelfNanoseconds.CompareTo(A.SelfNanoseconds));
        return Entries;
    }

    private static double ParseNumber(string[] Fields, int Column)
    {
        if (Column < 0 || Column >= Fields.Length)
        {
            return 0.0;
        }

        return double.TryParse(Fields[Column], NumberStyles.Float, CultureInfo.InvariantCulture, out double Value)
            ? Value
            : 0.0;
    }

    private static void Report(List<ProfileEntry> Entries, string BaselinePath, int Top, bool bGpu)
    {
        Dictionary<string, ProfileEntry> Baseline = new(StringComparer.Ordinal);

        if (File.Exists(BaselinePath))
        {
            foreach (ProfileEntry Entry in Parse(File.ReadAllText(BaselinePath)))
            {
                Baseline[Entry.Name] = Entry;
            }
        }

        double Total = Entries.Sum(E => E.SelfNanoseconds);

        Log.Raw("");
        Log.Raw(bGpu
            ? $"GPU passes by self time, {Entries.Count} passes, {Total / 1_000_000.0:F1} ms total"
            : $"Hotspots by self time, {Entries.Count} zones, {Total / 1_000_000.0:F1} ms total");
        Log.Raw("");
        bool bHasSpread = Entries.Any(E => E.SpreadPercent > 0.0);
        string SpreadHeader = bHasSpread ? $" {"noise",7}" : string.Empty;

        Log.Raw($"{"self ms",10} {"share",7} {"calls",10} {"mean us",10}{SpreadHeader} {"vs base",9}  zone");
        Log.Raw(new string('-', bHasSpread ? 118 : 110));

        foreach (ProfileEntry Entry in Entries.Take(Top))
        {
            string Delta = "-";

            if (Baseline.TryGetValue(Entry.Name, out ProfileEntry? Before) && Before.SelfNanoseconds > 0.0)
            {
                double Change = (Entry.SelfNanoseconds - Before.SelfNanoseconds) / Before.SelfNanoseconds * 100.0;
                Delta = $"{Change:+0.0;-0.0;0.0}%";
            }
            else if (Baseline.Count > 0)
            {
                Delta = "new";
            }

            double Share = Total > 0.0 ? Entry.SelfNanoseconds / Total * 100.0 : 0.0;

            string Spread = bHasSpread ? $" {Entry.SpreadPercent,6:F1}%" : string.Empty;

            Log.Raw($"{Entry.SelfMilliseconds,10:F1} {Share,6:F1}% {Entry.Count,10} "
                + $"{Entry.MeanNanoseconds / 1000.0,10:F2}{Spread} {Delta,9}  {Entry.Name}");

            if (Entry.Source.Length > 0)
            {
                Log.Raw($"{string.Empty,49}  {Entry.Source}");
            }
        }

        if (bHasSpread)
        {
            double[] Spreads = Entries.Where(E => E.SpreadPercent > 0.0).Select(E => E.SpreadPercent).OrderBy(V => V).ToArray();

            if (Spreads.Length > 0)
            {
                Log.Raw("");
                Log.Raw($"Noise floor across the repeats: median {Median(Spreads):F1}%, worst {Spreads[^1]:F1}%. "
                    + "Treat a smaller delta as no result.");
            }
        }

        if (Baseline.Count == 0)
        {
            return;
        }

        // Whole-capture movement, which a single zone's share cannot show.
        double BaselineTotal = Baseline.Values.Sum(E => E.SelfNanoseconds);

        if (BaselineTotal > 0.0)
        {
            Log.Raw("");
            Log.Raw($"Total self time {(Total - BaselineTotal) / BaselineTotal * 100.0:+0.0;-0.0;0.0}% "
                + $"against the baseline ({BaselineTotal / 1_000_000.0:F1} ms -> {Total / 1_000_000.0:F1} ms)");
        }
    }
}
