using System.Diagnostics;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Platform;
using LuminaBuildTool.Rules;

namespace LuminaBuildTool.Modes;

/// <summary>Launches a target's executable, resolved through the same rules the build used to produce it.</summary>
public static class RunMode
{
    public static int Run(CommandLine Arguments, BuildDirectories Directories)
    {
        string? TargetName = Arguments.GetPositional(1);

        if (string.IsNullOrEmpty(TargetName))
        {
            throw new BuildException("Run requires a target name. Usage: LuminaBuildTool Run <Target> [options] [-- <target args>]");
        }

        BuildPlatform PlatformValue = Arguments.GetEnum("Platform", BuildPlatformRegistry.HostPlatform);

        // A cross-compiled binary is not something this host can execute, and the failure it
        // produces otherwise is an exec format error naming nothing useful.
        if (PlatformValue != BuildPlatformRegistry.HostPlatform)
        {
            throw new BuildException(
                $"Cannot run a {PlatformValue} binary on {BuildPlatformRegistry.HostPlatform}. Build it and run it on that platform.");
        }

        BuildConfiguration ConfigurationValue = Arguments.GetEnum("Configuration", BuildConfiguration.Development);
        TargetType TypeValue = Arguments.GetEnum("TargetType", TargetType.Editor);

        RulesAssembly Assembly = RulesAssembly.Create(Directories, Arguments.HasFlag("RecompileRules"));
        IBuildPlatform PlatformSupport = BuildPlatformRegistry.Get(PlatformValue);
        BuildOptions Options = BuildOptions.Load(Directories, Arguments);

        TargetInfo Info = new(TargetName, TypeValue, PlatformValue, ConfigurationValue, Directories, Options);
        BuildTarget Target = new TargetAssembler(Assembly, Directories, PlatformSupport).Assemble(TargetName, Info);

        // The same fields the IDE's Run button reads. A game target's launch module is the .so the editor
        // loads, so its rules point Run at the editor with --Project set.
        string Executable = Target.Rules.DebuggerCommand.Length > 0
            ? Target.Rules.DebuggerCommand
            : Target.LaunchModule?.OutputFile ?? string.Empty;

        if (Executable.Length == 0)
        {
            throw new BuildException(
                $"Target '{Target.Name}' produces no executable and names no run command, so there is nothing to run.");
        }

        if (!File.Exists(Executable))
        {
            throw new BuildException(
                $"'{Executable}' does not exist. Build it first:{Environment.NewLine}"
                + $"  {HostScriptName("LuminaBuild")} Build {TargetName} -TargetType={TypeValue} -Configuration={ConfigurationValue}");
        }

        // Target arguments first so anything forwarded after "--" can override them, which is the
        // usual precedence for a command line read left to right.
        List<string> LaunchArguments = new();

        if (Target.Rules.DebuggerArguments.Length > 0)
        {
            LaunchArguments.AddRange(SplitArguments(Target.Rules.DebuggerArguments));
        }

        LaunchArguments.AddRange(Arguments.ForwardedArguments);

        return Launch(Executable, Target.Rules.DebuggerWorkingDirectory, LaunchArguments);
    }

    /// <summary>Front-end script for this host, so a diagnostic never names the wrong one.</summary>
    internal static string HostScriptName(string BaseName)
    {
        return OperatingSystem.IsWindows() ? $"{BaseName}.bat" : $"./{BaseName}.sh";
    }

    /// <summary>Splits a rules-authored argument string into arguments, honouring double quotes.</summary>
    internal static List<string> SplitArguments(string Arguments)
    {
        List<string> Result = new();
        System.Text.StringBuilder Current = new();
        bool bInQuotes = false;
        bool bHasArgument = false;

        foreach (char Character in Arguments)
        {
            if (Character == '"')
            {
                bInQuotes = !bInQuotes;
                bHasArgument = true;
                continue;
            }

            if (!bInQuotes && char.IsWhiteSpace(Character))
            {
                if (bHasArgument)
                {
                    Result.Add(Current.ToString());
                    Current.Clear();
                    bHasArgument = false;
                }

                continue;
            }

            Current.Append(Character);
            bHasArgument = true;
        }

        if (bHasArgument)
        {
            Result.Add(Current.ToString());
        }

        return Result;
    }

    private static int Launch(string Executable, string WorkingDirectory, IReadOnlyList<string> LaunchArguments)
    {
        Log.Info("Running {0}", Executable);

        if (LaunchArguments.Count > 0)
        {
            Log.Verbose("Arguments: {0}", string.Join(' ', LaunchArguments));
        }

        ProcessStartInfo StartInfo = new(Executable)
        {
            UseShellExecute = false,
        };

        // Only when the rules asked for one; the engine finds its root from the executable, so otherwise the
        // caller's directory stands and relative forwarded paths mean what they look like.
        if (WorkingDirectory.Length > 0 && Directory.Exists(WorkingDirectory))
        {
            StartInfo.WorkingDirectory = WorkingDirectory;
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

            // Reported rather than swallowed: a target that dies on startup is the common case this
            // mode is used to diagnose, and a zero here would say it ran fine.
            if (Runner.ExitCode != 0)
            {
                Log.Error("{0} exited with code {1}.", Path.GetFileName(Executable), Runner.ExitCode);
            }

            return Runner.ExitCode;
        }
        catch (Exception Ex) when (Ex is System.ComponentModel.Win32Exception or IOException)
        {
            Log.Error("Could not start '{0}': {1}", Executable, Ex.Message);
            return 1;
        }
    }
}
