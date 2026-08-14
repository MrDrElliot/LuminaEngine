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
              Clean [Target]            Delete a target's outputs, or all intermediates
              Query [Target]            List targets, modules and plugins, or describe one target
              Includes <Target>         Rank headers by how many translation units include them
              Deps <Target>             Compare declared module dependencies against reached ones
              GenerateProjectFiles      Write IDE project and solution files

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
              -KeepGoing                Keep building after a failure
              -DryRun                   List the outdated actions without running them
              -RecompileRules           Force the Target.cs and Build.cs assembly to rebuild
              -NoProjectFileUpdate      Do not refresh IDE project files when a Build.cs changed
              -Timeline                 Write a Perfetto / chrome://tracing trace of the build
              -Verbose / -Trace         More diagnostic output

            Feature switches (default in Engine/Build/BuildConfiguration.json):
              -Tracy=<mode>             auto | on | off
              -Validation=<mode>        auto | on | off
              -Aftermath=<mode>         auto | on | off   (NVIDIA crash dumps)
              -RadeonGpuDetective=<mode> auto | on | off  (AMD crash analysis)
              -VerboseLogging=<mode>    auto | on | off

            Setup options:
              -Force                    Re-download the dependency bundle even if External/ exists
              -Yes                      Skip the download confirmation prompt
            """);
    }
}
