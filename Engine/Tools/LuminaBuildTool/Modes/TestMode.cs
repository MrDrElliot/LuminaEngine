using System.Diagnostics;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Execution;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Platform;
using LuminaBuildTool.Rules;

namespace LuminaBuildTool.Modes;

// Builds and runs the test suites discovered beside modules, one binary per module.
public static class TestMode
{
    private const int DefaultSuiteTimeoutSeconds = 1800;

    public static async Task<int> RunAsync(CommandLine Arguments, BuildDirectories Directories, CancellationToken Cancellation)
    {
        RulesAssembly Assembly = RulesAssembly.Create(Directories, Arguments.HasFlag("RecompileRules"));

        if (Assembly.DiscoveredTestSuites.Count == 0)
        {
            Log.Warning("No test suites found. A module gets one by adding a Tests directory beside its Build.cs.");
            return 0;
        }

        string? Requested = Arguments.GetPositional(1);

        if (Arguments.HasFlag("List"))
        {
            ListSuites(Assembly);
            return 0;
        }

        // Benchmarks measure rather than assert, so a plain run stays to the correctness suites.
        TestSuiteKind Kind = Arguments.HasFlag("Benchmarks") ? TestSuiteKind.Benchmarks : TestSuiteKind.Tests;

        List<TestSuite> Suites = SelectSuites(Assembly, Requested, Kind);

        if (Suites.Count == 0)
        {
            Log.Warning("No {0} suites found.", Kind);
            return 0;
        }

        BuildPlatform PlatformValue = Arguments.GetEnum("Platform", BuildPlatformRegistry.HostPlatform);
        BuildConfiguration ConfigurationValue = Arguments.GetEnum("Configuration", BuildConfiguration.Development);

        // A suite testing an editor module only exists in an editor target, which asking by name should not require knowing.
        TargetType TypeValue = Arguments.GetEnum("TargetType", TargetType.Editor);

        bool bRunAfterBuild = !Arguments.HasFlag("BuildOnly");

        if (bRunAfterBuild && PlatformValue != BuildPlatformRegistry.HostPlatform)
        {
            throw new BuildException(
                $"Cannot run a {PlatformValue} test binary on {BuildPlatformRegistry.HostPlatform}. Pass -BuildOnly to compile it here.");
        }

        Stopwatch Timer = Stopwatch.StartNew();

        int BuildResult = await BuildMode.BuildTargetsAsync(
            Suites.Select(S => S.SuiteName).ToList(),
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

        if (!bRunAfterBuild)
        {
            Log.Info("Built {0} test suite(s) in {1:F2}s.", Suites.Count, Timer.Elapsed.TotalSeconds);
            return 0;
        }

        return await RunSuitesAsync(Suites, Arguments, Directories, Assembly, TypeValue, PlatformValue, ConfigurationValue, Timer, Cancellation)
            .ConfigureAwait(false);
    }

    private static void ListSuites(RulesAssembly Assembly)
    {
        Log.Raw("Test suites:");

        foreach (TestSuite Suite in Assembly.DiscoveredTestSuites.Values.OrderBy(S => S.SuiteName, StringComparer.OrdinalIgnoreCase))
        {
            Log.Raw($"  {Suite.SuiteName,-32} {Suite.SourceDirectory}");
        }

        Log.Raw("");
        Log.Raw("A Benchmarks suite runs only with -Benchmarks, or by naming it in full.");
    }

    // Resolves the requested name to suites, accepting the module name or the suite name.
    private static List<TestSuite> SelectSuites(RulesAssembly Assembly, string? Requested, TestSuiteKind Kind)
    {
        if (string.IsNullOrEmpty(Requested))
        {
            return Assembly.DiscoveredTestSuites.Values
                .Where(S => S.Kind == Kind)
                .OrderBy(S => S.SuiteName, StringComparer.OrdinalIgnoreCase)
                .ToList();
        }

        // A bare module name picks the kind being run, so "Test Runtime" never builds its benchmarks.
        foreach (TestSuite Suite in Assembly.DiscoveredTestSuites.Values)
        {
            if (Suite.SuiteName.Equals(Requested, StringComparison.OrdinalIgnoreCase)
                || (Suite.Kind == Kind && Suite.ModuleName.Equals(Requested, StringComparison.OrdinalIgnoreCase)))
            {
                return new List<TestSuite> { Suite };
            }
        }

        throw new BuildException(
            $"No suite for '{Requested}'. Known suites: "
            + string.Join(", ", Assembly.DiscoveredTestSuites.Keys.OrderBy(K => K, StringComparer.OrdinalIgnoreCase)));
    }

    private static async Task<int> RunSuitesAsync(
        IReadOnlyList<TestSuite> Suites,
        CommandLine Arguments,
        BuildDirectories Directories,
        RulesAssembly Assembly,
        TargetType TypeValue,
        BuildPlatform PlatformValue,
        BuildConfiguration ConfigurationValue,
        Stopwatch Timer,
        CancellationToken Cancellation)
    {
        IBuildPlatform PlatformSupport = BuildPlatformRegistry.Get(PlatformValue);
        BuildOptions Options = BuildOptions.Load(Directories, Arguments);

        // A project build writes its suite beside the project's binaries, not the engine's.
        string EngineBinaries = Path.Combine(
            Directories.EngineRoot, "Binaries", PlatformValue.GetOutputDirectoryName());

        List<string> SuiteArguments = new();

        if (Arguments.GetString("Filter") is { Length: > 0 } Filter)
        {
            SuiteArguments.Add($"--gtest_filter={Filter}");
        }

        SuiteArguments.AddRange(Arguments.ForwardedArguments);

        TimeSpan SuiteTimeout = TimeSpan.FromSeconds(Arguments.GetInt("SuiteTimeoutSeconds", DefaultSuiteTimeoutSeconds));

        List<string> Failed = new();

        foreach (TestSuite Suite in Suites)
        {
            TargetInfo Info = new(Suite.SuiteName, TypeValue, PlatformValue, ConfigurationValue, Directories, Options);
            BuildTarget Target = new TargetAssembler(Assembly, Directories, PlatformSupport).Assemble(Suite.SuiteName, Info);

            string Executable = Target.LaunchModule?.OutputFile ?? string.Empty;

            if (Executable.Length == 0 || !File.Exists(Executable))
            {
                Log.Error("Test suite '{0}' produced no executable at '{1}'.", Suite.SuiteName, Executable);
                Failed.Add(Suite.SuiteName);
                continue;
            }

            Log.Info("Running {0}", Suite.SuiteName);

            // Plugin modules write beside their plugin, so the loader needs every directory this suite linked into.
            List<string> SearchDirectories = new() { EngineBinaries };
            SearchDirectories.AddRange(Target.Modules
                .Where(M => M.BinaryType == ModuleBinaryType.SharedLibrary && M.OutputDirectory.Length > 0)
                .Select(M => M.OutputDirectory));
            string LibraryPath = string.Join(Path.PathSeparator, SearchDirectories.Distinct(StringComparer.OrdinalIgnoreCase));

            if (await LaunchAsync(Suite.SuiteName, Executable, SuiteArguments, LibraryPath, SuiteTimeout, Cancellation).ConfigureAwait(false) != 0)
            {
                Failed.Add(Suite.SuiteName);
            }
        }

        if (Failed.Count > 0)
        {
            Log.Error("{0} of {1} test suite(s) failed: {2}", Failed.Count, Suites.Count, string.Join(", ", Failed));
            return 1;
        }

        Log.Info("{0} test suite(s) passed in {1:F2}s.", Suites.Count, Timer.Elapsed.TotalSeconds);
        return 0;
    }

    private static async Task<int> LaunchAsync(
        string SuiteName,
        string Executable,
        IReadOnlyList<string> LaunchArguments,
        string LibrarySearchPath,
        TimeSpan Timeout,
        CancellationToken Cancellation)
    {
        Cancellation.ThrowIfCancellationRequested();

        ProcessStartInfo StartInfo = new(Executable)
        {
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(Executable) ?? string.Empty,
        };

        // Windows finds DLLs through PATH, while the Linux loader reads only LD_LIBRARY_PATH beyond the rpath.
        if (LibrarySearchPath.Length > 0)
        {
            string Variable = OperatingSystem.IsWindows() ? "PATH" : "LD_LIBRARY_PATH";
            string Existing = Environment.GetEnvironmentVariable(Variable) ?? string.Empty;
            StartInfo.Environment[Variable] = Existing.Length > 0 ? LibrarySearchPath + Path.PathSeparator + Existing : LibrarySearchPath;
        }

        foreach (string Argument in LaunchArguments)
        {
            StartInfo.ArgumentList.Add(Argument);
        }

        try
        {
            using Process? Runner = Process.Start(StartInfo);

            if (Runner is null)
            {
                Log.Error("Could not start '{0}'.", Executable);
                return 1;
            }

            using CancellationTokenSource Deadline = CancellationTokenSource.CreateLinkedTokenSource(Cancellation);
            if (Timeout > TimeSpan.Zero)
            {
                Deadline.CancelAfter(Timeout);
            }

            try
            {
                await Runner.WaitForExitAsync(Deadline.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                ProcessRunner.TryKill(Runner);

                if (Cancellation.IsCancellationRequested)
                {
                    throw;
                }

                Log.Error("Test suite '{0}' timed out after {1}s and was killed.", SuiteName, Timeout.TotalSeconds);
                return 1;
            }

            // Ctrl+C on the shared console can end the suite before the cancel handler runs.
            Cancellation.ThrowIfCancellationRequested();
            return Runner.ExitCode;
        }
        catch (Exception Ex) when (Ex is System.ComponentModel.Win32Exception or IOException)
        {
            Log.Error("Could not start '{0}': {1}", Executable, Ex.Message);
            return 1;
        }
    }
}
