using System.Diagnostics;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Platform;
using LuminaBuildTool.Rules;

namespace LuminaBuildTool.Modes;

// Builds and runs the test suites discovered beside modules, one binary per module.
public static class TestMode
{
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

        // A suite testing an editor module only exists in an editor target, and asking for one by name
        // should not require knowing that.
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

        return RunSuites(Suites, Arguments, Directories, Assembly, TypeValue, PlatformValue, ConfigurationValue, Timer);
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

    private static int RunSuites(
        IReadOnlyList<TestSuite> Suites,
        CommandLine Arguments,
        BuildDirectories Directories,
        RulesAssembly Assembly,
        TargetType TypeValue,
        BuildPlatform PlatformValue,
        BuildConfiguration ConfigurationValue,
        Stopwatch Timer)
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

            if (Launch(Executable, SuiteArguments, EngineBinaries) != 0)
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

    private static int Launch(string Executable, IReadOnlyList<string> LaunchArguments, string EngineBinaries)
    {
        ProcessStartInfo StartInfo = new(Executable)
        {
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(Executable) ?? string.Empty,
        };

        // A project's suite links the engine's module DLLs but is written beside the project's own binaries,
        // so without this the loader finds nothing and the process dies before main.
        if (EngineBinaries.Length > 0)
        {
            string Existing = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;
            StartInfo.Environment["PATH"] = EngineBinaries + Path.PathSeparator + Existing;
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

            Runner.WaitForExit();
            return Runner.ExitCode;
        }
        catch (Exception Ex) when (Ex is System.ComponentModel.Win32Exception or IOException)
        {
            Log.Error("Could not start '{0}': {1}", Executable, Ex.Message);
            return 1;
        }
    }
}
