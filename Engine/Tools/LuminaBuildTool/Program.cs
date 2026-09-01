using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Modes;

namespace LuminaBuildTool;

public static class Program
{
    public static async Task<int> Main(string[] Args)
    {
        CommandLine Arguments = new(Args);

        if (Arguments.HasFlag("Verbose"))
        {
            Log.MinLevel = LogLevel.Verbose;
        }

        if (Arguments.HasFlag("Trace"))
        {
            Log.MinLevel = LogLevel.Trace;
        }

        string Mode = Arguments.GetPositional(0) ?? (Arguments.HasFlag("Help") ? "Help" : string.Empty);

        if (Mode.Length == 0 || Mode.Equals("Help", StringComparison.OrdinalIgnoreCase))
        {
            PrintUsage();
            return Mode.Length == 0 ? 1 : 0;
        }

        using CancellationTokenSource Cancellation = new();

        Console.CancelKeyPress += (_, EventArgs) =>
        {
            EventArgs.Cancel = true;
            Log.Warning("Cancelling...");
            Cancellation.Cancel();
        };

        try
        {
            BuildDirectories Directories = BuildDirectories.Discover(
                Arguments.GetString("EngineRoot"),
                Arguments.GetString("Project"));

            Log.Verbose("Engine root: {0}", Directories.EngineRoot);

            return Mode.ToLowerInvariant() switch
            {
                "build" => await BuildMode.RunAsync(Arguments, Directories, Cancellation.Token).ConfigureAwait(false),
                "run" => RunMode.Run(Arguments, Directories),
                "test" => await TestMode.RunAsync(Arguments, Directories, Cancellation.Token).ConfigureAwait(false),
                "profile" => await ProfileMode.RunAsync(Arguments, Directories, Cancellation.Token).ConfigureAwait(false),
                "clean" => CleanMode.Run(Arguments, Directories),
                "query" => QueryMode.Run(Arguments, Directories),
                "includes" => AnalyzeMode.RunIncludes(Arguments, Directories),
                "deps" => AnalyzeMode.RunDependencies(Arguments, Directories),
                "setup" => await SetupMode.RunAsync(Arguments, Directories, Cancellation.Token).ConfigureAwait(false),
                "generateprojectfiles" or "genprojects" => ProjectFilesMode.Run(Arguments, Directories),
                _ => UnknownMode(Mode),
            };
        }
        catch (BuildException Ex)
        {
            Log.Error("{0}", Ex.Message);
            return 1;
        }
        catch (OperationCanceledException)
        {
            Log.Error("Build cancelled.");
            return 130;
        }
        catch (Exception Ex)
        {
            Log.Error("Unhandled {0}: {1}{2}{3}", Ex.GetType().Name, Ex.Message, Environment.NewLine, Ex.StackTrace ?? string.Empty);
            return 1;
        }
    }

    private static int UnknownMode(string Mode)
    {
        Log.Error("Unknown mode '{0}'.", Mode);
        PrintUsage();
        return 1;
    }

    private static void PrintUsage()
    {
        Log.Raw(
            """
            LuminaBuildTool - build and project generation for Lumina Engine

            Usage:
              LuminaBuildTool <Mode> [Target] [options] [-- <arguments for the target>]

            Modes:
              Setup                     Fetch external dependencies and configure the environment
              Build <Target>            Compile and link a target
              Run <Target>              Launch a target's executable, located from its own rules
              Test [Module]             Build and run test suites; a module with a Tests or Benchmarks
                                        directory beside its Build.cs gets one binary per directory
              Profile <Target>          Run a target under Tracy and rank its zones by self time
              Clean [Target]            Delete a target's outputs, or all intermediates
              Query [Target]            List targets, modules and plugins, or describe one target
              Includes <Target>         Rank headers by how many translation units include them
              Deps <Target>             Compare declared module dependencies against reached ones
              GenerateProjectFiles      Write IDE project and solution files

            Profile options:
              -Seconds=<n>              Capture window, default 15
              -Delay=<n>                Wait n seconds before connecting, to skip startup and load
              -Repeat=<n>               Capture n times and report the median, with a noise floor
              -Top=<n>                  Hotspots to print, default 25
              -Baseline                 Store this run as the one later runs are compared against
              -Gpu                      Rank GPU passes instead of CPU zones, nesting resolved
              -Filter=<pattern>         Only zones whose name matches
              -NoBuild                  Profile what is already built
              -Benchmark=<map>          Drive a deterministic run, capturing only measured frames
              -Warmup=<n>               Frames to settle before capturing, default 900
              -Frames=<n>               Measured frames, must outlive every capture, default 4000
              -ClearShaderCache         Delete the shader cache first, so compiles are paid in warmup
            Traces and CSVs land in Intermediates/Profiling. Only instrumented zones are visible, so a
            path with no LUMINA_PROFILE_SCOPE will not appear however hot it is.

            Test options:
              -List                     List the discovered suites and where they live
              -Filter=<pattern>         Passed on as --gtest_filter
              -BuildOnly                Compile the suites without running them
              -Benchmarks               Run the Benchmarks suites instead of the Tests ones

            Includes options (reads the graph the last build recorded):
              -Top=<n>                  How many headers to list (default: 40)
              -Module=<name>            Rank within one module only
              -All                      Include toolchain and SDK headers

            Options:
              -Platform=<name>          Windows64 | Linux64 (default: host platform)
              -Configuration=<name>     Debug | Development | Shipping (default: Development)
              -TargetType=<name>        Editor | Game | Program (default: from the target rules)
              -EngineRoot=<path>        Engine installation root (default: LUMINA_DIR or auto-detected)
              -Project=<path>           Game project root, for building against an installed engine
              -MaxParallel=<n>          Concurrent actions (default: processor count minus one,
                                        lowered to fit available memory)
              -Clean                    Delete outputs before building
              -NoUnity                  Compile every source on its own, no unity files
              -NoAdaptiveUnity          Keep recently edited sources in their unity file
              -KeepGoing                Keep building after a failure
              -DryRun                   List the outdated actions without running them
              -RecompileRules           Force the Target.cs and Build.cs assembly to rebuild
              -NoProjectFileUpdate      Do not refresh IDE project files when a Build.cs changed
              -Timeline                 Write a Perfetto / chrome://tracing trace of the build
              -Pgo=<mode>               off | instrument | optimize

            Profile guided optimization (two passes, Shipping is the one worth profiling):
              1. Build with -Pgo=instrument
              2. Run the binary through a representative workload, then exit it cleanly
              3. Build again with -Pgo=optimize
            On Windows the .pgd sits beside the binary, because that is the only place LINK looks for
            the .pgc run files, and pgort140.dll is staged next to it so the instrumented build starts.
            On Linux the raw profiles land in Intermediates/PGO/<Platform> and must be merged first:
              llvm-profdata merge -output=<name>.profdata <name>.raw/*.profraw
            A profile is stale once the source moves; rerun the cycle rather than trusting it.
              -Verbose / -Trace         More diagnostic output

            Feature switches (default in Engine/Build/BuildConfiguration.json):
              -Tracy=<mode>             auto | on | off
              -Validation=<mode>        auto | on | off
              -Aftermath=<mode>         auto | on | off   (NVIDIA crash dumps)
              -RadeonGpuDetective=<mode> auto | on | off  (AMD crash analysis)
              -VerboseLogging=<mode>    auto | on | off
              -ForceInlineHint=<mode>   auto | on | off   (off lets the optimizer decide)

            Setup options:
              -Force                    Re-download the dependency bundle even if External/ exists
              -Yes                      Skip the download confirmation prompt
            """);
    }
}
