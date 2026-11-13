using System.Diagnostics;
using System.Xml.Linq;
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

    // CancelAfter rejects a TimeSpan above uint.MaxValue - 1 milliseconds.
    private const int MaxSuiteTimeoutSeconds = (int)((uint.MaxValue - 1) / 1000);

    // 0 is the documented "no timeout" sentinel; LaunchAsync only arms CancelAfter when Timeout > TimeSpan.Zero.
    public static int ValidateSuiteTimeoutSeconds(int SuiteTimeoutSeconds)
    {
        if (SuiteTimeoutSeconds < 0)
        {
            throw new BuildException($"-SuiteTimeoutSeconds={SuiteTimeoutSeconds} cannot be negative. Pass 0 for no timeout.");
        }

        if (SuiteTimeoutSeconds > MaxSuiteTimeoutSeconds)
        {
            throw new BuildException($"-SuiteTimeoutSeconds={SuiteTimeoutSeconds} exceeds the maximum of {MaxSuiteTimeoutSeconds} seconds.");
        }

        return SuiteTimeoutSeconds;
    }

    public static async Task<int> RunAsync(CommandLine Arguments, BuildDirectories Directories, CancellationToken Cancellation)
    {
        RulesAssembly Assembly = RulesAssembly.Create(Directories, Arguments.HasFlag("RecompileRules"));

        string? Requested = Arguments.GetPositional(1);

        if (Arguments.HasFlag("List"))
        {
            ListSuites(Assembly);
            return 0;
        }

        // Benchmarks measure rather than assert, so a plain run stays to the correctness suites.
        TestSuiteKind Kind = Arguments.HasFlag("Benchmarks") ? TestSuiteKind.Benchmarks : TestSuiteKind.Tests;

        bool bRunAfterBuild = !Arguments.HasFlag("BuildOnly");

        // LuminaBuildTool's own xunit tests have no suite name, platform or filter to ask by, so only an unscoped run covers them.
        bool bRunLuminaBuildToolTests = bRunAfterBuild && Kind == TestSuiteKind.Tests && Requested is null
            && string.IsNullOrEmpty(Arguments.GetString("Filter"));

        List<string> Suites = SelectSuites(Assembly, Requested, Kind);

        if (Suites.Count == 0 && !bRunLuminaBuildToolTests)
        {
            Log.Warning(Assembly.DiscoveredTestSuites.Count == 0
                ? "No test suites found. A module gets one by adding a Tests directory beside its Build.cs, a target by [TestTarget]."
                : $"No {Kind} suites found.");
            return 0;
        }

        Stopwatch Timer = Stopwatch.StartNew();

        if (Suites.Count == 0)
        {
            bool bPassed = await RunLuminaBuildToolTestsAsync(Directories, Cancellation).ConfigureAwait(false);
            return bPassed ? 0 : 1;
        }

        BuildPlatform PlatformValue = Arguments.GetEnum("Platform", BuildPlatformRegistry.HostPlatform);
        BuildConfiguration ConfigurationValue = Arguments.GetEnum("Configuration", BuildConfiguration.Development);

        // A suite testing an editor module only exists in an editor target, which asking by name should not require knowing.
        TargetType TypeValue = Arguments.GetEnum("TargetType", TargetType.Editor);

        int SuiteTimeoutSeconds = ValidateSuiteTimeoutSeconds(Arguments.GetInt("SuiteTimeoutSeconds", DefaultSuiteTimeoutSeconds));

        if (bRunAfterBuild && PlatformValue != BuildPlatformRegistry.HostPlatform)
        {
            throw new BuildException(
                $"Cannot run a {PlatformValue} test binary on {BuildPlatformRegistry.HostPlatform}. Pass -BuildOnly to compile it here.");
        }

        int BuildResult = await BuildMode.BuildTargetsAsync(
            Suites,
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

        return await RunSuitesAsync(
                Suites, Arguments, Directories, Assembly, TypeValue, PlatformValue, ConfigurationValue,
                TimeSpan.FromSeconds(SuiteTimeoutSeconds), Timer, Cancellation, bRunLuminaBuildToolTests)
            .ConfigureAwait(false);
    }

    // verify-lumina.ps1 reads failures only from gtest-shaped "[  FAILED  ] " lines, so xunit failures are echoed in that form.
    private static async Task<bool> RunLuminaBuildToolTestsAsync(BuildDirectories Directories, CancellationToken Cancellation)
    {
        string ProjectPath = Path.Combine(Directories.EngineRoot, "Engine", "Tools", "LuminaBuildTool", "Tests", "LuminaBuildTool.Tests.csproj");
        string ResultsDirectory = Path.Combine(Path.GetTempPath(), $"LuminaBuildToolTests-{Guid.NewGuid():N}");

        Log.Info("Running LuminaBuildTool.Tests");

        ProcessResult Result = await ProcessRunner.RunAsync(
            ManagedProjectStep.ResolveDotNet(),
            $"test \"{ProjectPath}\" --nologo --logger \"trx;LogFileName=results.trx\" --results-directory \"{ResultsDirectory}\"",
            Directories.EngineRoot,
            null,
            Cancellation).ConfigureAwait(false);

        if (Result.bSucceeded)
        {
            TryDeleteResultsDirectory(ResultsDirectory);
            return true;
        }

        List<string> FailedTests = ReadFailedTestNamesFromTrx(ResultsDirectory);
        TryDeleteResultsDirectory(ResultsDirectory);

        // No TRX means the tests never ran at all, most likely because the project failed to build or restore.
        foreach (string FailedTest in FailedTests.Count > 0 ? FailedTests : new List<string> { "LuminaBuildTool.Tests.Build" })
        {
            Log.Raw($"[  FAILED  ] {FailedTest}");
        }

        Log.Error("Test suite 'LuminaBuildTool.Tests' failed:\n{0}", Result.Output);
        return false;
    }

    private static List<string> ReadFailedTestNamesFromTrx(string ResultsDirectory)
    {
        List<string> Names = new();

        if (!Directory.Exists(ResultsDirectory))
        {
            return Names;
        }

        foreach (string TrxFile in Directory.GetFiles(ResultsDirectory, "*.trx", SearchOption.AllDirectories))
        {
            try
            {
                XDocument Document = XDocument.Load(TrxFile);
                XNamespace Namespace = Document.Root!.Name.Namespace;

                foreach (XElement Result in Document.Descendants(Namespace + "UnitTestResult"))
                {
                    if ((string?)Result.Attribute("outcome") == "Failed")
                    {
                        Names.Add((string)Result.Attribute("testName")!);
                    }
                }
            }
            catch (Exception Ex) when (Ex is IOException or System.Xml.XmlException)
            {
                Log.Verbose("Could not read '{0}': {1}", TrxFile, Ex.Message);
            }
        }

        return Names;
    }

    private static void TryDeleteResultsDirectory(string ResultsDirectory)
    {
        try
        {
            if (Directory.Exists(ResultsDirectory))
            {
                Directory.Delete(ResultsDirectory, recursive: true);
            }
        }
        catch (IOException)
        {
            // Best effort; a leftover temp directory does not affect the test result.
        }
    }

    private static void ListSuites(RulesAssembly Assembly)
    {
        Log.Raw("Test suites:");

        foreach (TestSuite Suite in Assembly.DiscoveredTestSuites.Values.OrderBy(S => S.SuiteName, StringComparer.OrdinalIgnoreCase))
        {
            Log.Raw($"  {Suite.SuiteName,-32} {Suite.SourceDirectory}");
        }

        foreach (string Target in Assembly.TestTargetNames.Order(StringComparer.OrdinalIgnoreCase))
        {
            Log.Raw($"  {Target,-32} [TestTarget]");
        }

        Log.Raw("");
        Log.Raw("A Benchmarks suite runs only with -Benchmarks, or by naming it in full.");
    }

    // Resolves the requested name to suites, accepting the module name or the suite name.
    private static List<string> SelectSuites(RulesAssembly Assembly, string? Requested, TestSuiteKind Kind)
    {
        if (string.IsNullOrEmpty(Requested))
        {
            // A test target asserts rather than measures, so it runs with the correctness suites.
            return Assembly.DiscoveredTestSuites.Values
                .Where(S => S.Kind == Kind)
                .Select(S => S.SuiteName)
                .Concat(Kind == TestSuiteKind.Tests ? Assembly.TestTargetNames : Enumerable.Empty<string>())
                .Order(StringComparer.OrdinalIgnoreCase)
                .ToList();
        }

        // A bare module name picks the kind being run, so "Test Runtime" never builds its benchmarks.
        foreach (TestSuite Suite in Assembly.DiscoveredTestSuites.Values)
        {
            if (Suite.SuiteName.Equals(Requested, StringComparison.OrdinalIgnoreCase)
                || (Suite.Kind == Kind && Suite.ModuleName.Equals(Requested, StringComparison.OrdinalIgnoreCase)))
            {
                return new List<string> { Suite.SuiteName };
            }
        }

        if (Assembly.TestTargetNames.FirstOrDefault(T => T.Equals(Requested, StringComparison.OrdinalIgnoreCase)) is string Target)
        {
            return new List<string> { Target };
        }

        throw new BuildException(
            $"No suite for '{Requested}'. Known suites: "
            + string.Join(", ", Assembly.DiscoveredTestSuites.Keys.Concat(Assembly.TestTargetNames).Order(StringComparer.OrdinalIgnoreCase)));
    }

    private static async Task<int> RunSuitesAsync(
        IReadOnlyList<string> Suites,
        CommandLine Arguments,
        BuildDirectories Directories,
        RulesAssembly Assembly,
        TargetType TypeValue,
        BuildPlatform PlatformValue,
        BuildConfiguration ConfigurationValue,
        TimeSpan SuiteTimeout,
        Stopwatch Timer,
        CancellationToken Cancellation,
        bool bRunLuminaBuildToolTests)
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

        List<string> Failed = new();

        foreach (string Suite in Suites)
        {
            TargetInfo Info = new(Suite, TypeValue, PlatformValue, ConfigurationValue, Directories, Options);
            BuildTarget Target = new TargetAssembler(Assembly, Directories, PlatformSupport).Assemble(Suite, Info);

            string Executable = Target.LaunchModule?.OutputFile ?? string.Empty;

            if (Executable.Length == 0 || !File.Exists(Executable))
            {
                Log.Error("Test suite '{0}' produced no executable at '{1}'.", Suite, Executable);
                Failed.Add(Suite);
                continue;
            }

            Log.Info("Running {0}", Suite);

            // Plugin modules write beside their plugin, so the loader needs every directory this suite linked into.
            List<string> SearchDirectories = new() { EngineBinaries };
            SearchDirectories.AddRange(Target.Modules
                .Where(M => M.BinaryType == ModuleBinaryType.SharedLibrary && M.OutputDirectory.Length > 0)
                .Select(M => M.OutputDirectory));
            string LibraryPath = string.Join(Path.PathSeparator, SearchDirectories.Distinct(StringComparer.OrdinalIgnoreCase));

            int? ExitCode = await LaunchAsync(Suite, Executable, SuiteArguments, LibraryPath, SuiteTimeout, Cancellation).ConfigureAwait(false);

            if (ExitCode != 0)
            {
                if (ExitCode is int Code)
                {
                    Log.Error("Test suite '{0}' exited with 0x{1:X} ({2})", Suite, (uint)Code, DescribeExitCode(Code));
                }

                Failed.Add(Suite);
            }
        }

        int TotalSuites = Suites.Count;

        if (bRunLuminaBuildToolTests)
        {
            TotalSuites++;

            if (!await RunLuminaBuildToolTestsAsync(Directories, Cancellation).ConfigureAwait(false))
            {
                Failed.Add("LuminaBuildTool.Tests");
            }
        }

        if (Failed.Count > 0)
        {
            Log.Error("{0} of {1} test suite(s) failed: {2}", Failed.Count, TotalSuites, string.Join(", ", Failed));
            return 1;
        }

        Log.Info("{0} test suite(s) passed in {1:F2}s.", TotalSuites, Timer.Elapsed.TotalSeconds);
        return 0;
    }

    private static async Task<int?> LaunchAsync(
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
                return null;
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
                return null;
            }

            // Ctrl+C on the shared console can end the suite before the cancel handler runs.
            Cancellation.ThrowIfCancellationRequested();
            return Runner.ExitCode;
        }
        catch (Exception Ex) when (Ex is System.ComponentModel.Win32Exception or IOException)
        {
            Log.Error("Could not start '{0}': {1}", Executable, Ex.Message);
            return null;
        }
    }

    private static string DescribeExitCode(int ExitCode)
    {
        if (OperatingSystem.IsWindows())
        {
            return (uint)ExitCode switch
            {
                0xC0000005 => "access violation",
                0xC0000409 => "fast fail or stack buffer overrun",
                0x80000003 => "breakpoint or assert",
                _ => ExitCode == 1 ? "test failures" : "unknown",
            };
        }

        if (ExitCode > 128)
        {
            return (ExitCode - 128) switch
            {
                4 => "SIGILL",
                6 => "SIGABRT",
                8 => "SIGFPE",
                9 => "SIGKILL",
                11 => "SIGSEGV",
                int Signal => $"signal {Signal}",
            };
        }

        return ExitCode == 1 ? "test failures" : "unknown";
    }
}
