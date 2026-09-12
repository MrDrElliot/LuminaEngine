using System.Diagnostics;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;
using LuminaBuildTool.Platform;
using LuminaBuildTool.Rules;

namespace LuminaBuildTool.Modes;

/// <summary>Removes a target's intermediate and output files.</summary>
public static class CleanMode
{
    public static int Run(CommandLine Arguments, BuildDirectories Directories)
    {
        string? TargetName = Arguments.GetPositional(1);
        bool bDryRun = Arguments.HasFlag("DryRun");

        if (string.IsNullOrEmpty(TargetName))
        {
            bool bFull = Arguments.HasFlag("Full");

            bool bCleaned = CleanRoot(Directories.OutputRoot, bFull, bDryRun);

            // A project build leaves output in both trees, so stopping at the project would regenerate
            // against a stale engine.
            if (bFull && Directories.ProjectRoot is not null)
            {
                bCleaned &= CleanRoot(Directories.EngineRoot, bFull: true, bDryRun);
            }

            // Every delete is attempted before this, so one locked file still reports the rest.
            if (!bCleaned)
            {
                Log.Error(
                    "Some outputs could not be deleted, so the tree is half cleaned. Close Visual Studio, "
                    + "Rider and the editor, then run this again.");

                return 1;
            }

            return Arguments.HasFlag("Regenerate") ? Regenerate(Arguments, Directories, bDryRun) : 0;
        }

        BuildPlatform PlatformValue = Arguments.GetEnum("Platform", BuildPlatformRegistry.HostPlatform);
        BuildConfiguration ConfigurationValue = Arguments.GetEnum("Configuration", BuildConfiguration.Development);
        TargetType TypeValue = Arguments.GetEnum("TargetType", TargetType.Editor);

        RulesAssembly Assembly = RulesAssembly.Create(Directories, bForceRecompile: false);
        IBuildPlatform PlatformSupport = BuildPlatformRegistry.Get(PlatformValue);
        BuildOptions Options = BuildOptions.Load(Directories, Arguments);

        TargetInfo Info = new(TargetName, TypeValue, PlatformValue, ConfigurationValue, Directories, Options);
        BuildTarget Target = new TargetAssembler(Assembly, Directories, PlatformSupport).Assemble(TargetName, Info);

        CleanTarget(Target);

        return 0;
    }

    /// <summary>Deletes one tree's intermediates, plus its binaries and solution files when full.</summary>
    private static bool CleanRoot(string Root, bool bFull, bool bDryRun)
    {
        // Generated code goes with the objects: a .generated.h for a deleted type stays includable.
        bool bDeleted = DeleteDirectory(Path.Combine(Root, "Intermediates"), bDryRun);

        if (bFull)
        {
            // Non short-circuiting, so a failed binaries delete still leaves the solution cleaned up.
            bDeleted &= DeleteBinaries(Path.Combine(Root, "Binaries"), bDryRun);
            bDeleted &= DeleteSolutionFiles(Root, bDryRun);
        }

        // A dry run already listed every path, and nothing was cleaned.
        if (bDeleted && !bDryRun)
        {
            Log.Info(bFull ? "Cleaned '{0}'." : "Cleaned intermediates under '{0}'.", Root);
        }

        return bDeleted;
    }

    /// <summary>Deletes compiled output but keeps Binaries/DotNet, which this tool is running out of.</summary>
    private static bool DeleteBinaries(string BinariesDirectory, bool bDryRun)
    {
        if (!Directory.Exists(BinariesDirectory))
        {
            return true;
        }

        bool bDeleted = true;

        foreach (string Child in Directory.GetDirectories(BinariesDirectory))
        {
            if (Path.GetFileName(Child).Equals("DotNet", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            bDeleted &= DeleteDirectory(Child, bDryRun);
        }

        return bDeleted;
    }

    /// <summary>Matches on the exact extension, so a sibling .sln.DotSettings.user is left alone.</summary>
    private static bool DeleteSolutionFiles(string Root, bool bDryRun)
    {
        bool bDeleted = true;

        foreach (string SolutionFile in Directory.GetFiles(Root))
        {
            string Extension = Path.GetExtension(SolutionFile);

            if (Extension.Equals(".sln", StringComparison.OrdinalIgnoreCase)
                || Extension.Equals(".slnx", StringComparison.OrdinalIgnoreCase))
            {
                bDeleted &= DeleteFile(SolutionFile, bDryRun);
            }
        }

        return bDeleted;
    }

    /// <summary>Rewrites both workspaces, engine first so the project resolves against fresh engine rules.</summary>
    private static int Regenerate(CommandLine Arguments, BuildDirectories Directories, bool bDryRun)
    {
        if (bDryRun)
        {
            Log.Info("Would regenerate project files.");
            return 0;
        }

        // The engine solution is a separate workspace, written only when no project root is set.
        if (Directories.ProjectRoot is not null)
        {
            Log.Info("Generating engine project files...");

            int EngineExitCode = GenerateEngineWorkspace(Directories);

            if (EngineExitCode != 0)
            {
                return EngineExitCode;
            }

            Log.Info("Generating project files for '{0}'...", Directories.ProjectRoot);
        }

        ProjectFilesMode.Generate(Arguments, Directories, RulesAssembly.Create(Directories, bForceRecompile: false));

        return 0;
    }

    private static readonly HashSet<string> CleanOnlyOptions = new(StringComparer.OrdinalIgnoreCase)
    {
        "Project", "Full", "Regenerate", "EngineRoot",
    };

    /// <summary>Relaunches this tool as GenerateProjectFiles without -Project, forwarding every other option.</summary>
    private static int GenerateEngineWorkspace(BuildDirectories Directories)
    {
        // Both rules assemblies load as 'LuminaRules' into the default context, so a second in-process load throws.
        string HostPath = Environment.ProcessPath
            ?? throw new BuildException("Could not locate the running build tool to regenerate the engine workspace.");

        string[] OwnArguments = Environment.GetCommandLineArgs();

        ProcessStartInfo StartInfo = new(HostPath)
        {
            WorkingDirectory = Directories.EngineRoot,
            UseShellExecute = false,
        };

        // Under 'dotnet Tool.dll' the first argument is the assembly and the host needs it back.
        if (OwnArguments[0].EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
        {
            StartInfo.ArgumentList.Add(OwnArguments[0]);
        }

        StartInfo.ArgumentList.Add("GenerateProjectFiles");

        foreach (string Argument in OwnArguments.Skip(1))
        {
            if (Argument == "--")
            {
                break;
            }

            string? Option = OptionName(Argument);

            if (Option is null || CleanOnlyOptions.Contains(Option))
            {
                continue;
            }

            StartInfo.ArgumentList.Add(Argument);
        }

        StartInfo.ArgumentList.Add("-EngineRoot=" + Directories.EngineRoot);

        using Process Child = Process.Start(StartInfo)
            ?? throw new BuildException("Could not relaunch the build tool to regenerate the engine workspace.");

        Child.WaitForExit();

        return Child.ExitCode;
    }

    /// <summary>Option name as CommandLine parses it, or null for a positional.</summary>
    private static string? OptionName(string Argument)
    {
        if (Argument.Length == 0 || Argument[0] is not ('-' or '/'))
        {
            return null;
        }

        string Body = Argument.Substring(1).TrimStart('-');
        int Split = Body.IndexOfAny(new[] { '=', ':' });

        return Split >= 0 ? Body.Substring(0, Split) : Body;
    }

    public static void CleanTarget(BuildTarget Target)
    {
        DeleteDirectory(Target.IntermediateDirectory, bDryRun: false);

        foreach (BuildModule Module in Target.Modules)
        {
            // Asked per module: engine modules share one intermediate set across targets, so deleting only the
            // target's own directory would leave them behind.
            DeleteDirectory(Module.IntermediateDirectory, bDryRun: false);
            DeleteDirectory(Module.GeneratedCodeDirectory, bDryRun: false);

            DeleteFile(Module.OutputFile, bDryRun: false);
            DeleteFile(Module.ImportLibraryFile, bDryRun: false);

            if (Module.OutputFile.Length > 0)
            {
                DeleteFile(Path.ChangeExtension(Module.OutputFile, ".pdb"), bDryRun: false);
            }
        }

        Log.Info("Cleaned {0}.", Target);
    }

    private static bool DeleteDirectory(string DirectoryPath, bool bDryRun)
    {
        if (!Directory.Exists(DirectoryPath))
        {
            return true;
        }

        if (bDryRun)
        {
            Log.Info("Would delete '{0}'", DirectoryPath);
            return true;
        }

        try
        {
            Directory.Delete(DirectoryPath, recursive: true);
            Log.Verbose("Deleted '{0}'", DirectoryPath);
            return true;
        }
        catch (Exception Ex) when (Ex is IOException or UnauthorizedAccessException)
        {
            Log.Warning("Could not delete '{0}': {1}", DirectoryPath, Ex.Message);
            return false;
        }
    }

    private static bool DeleteFile(string FilePath, bool bDryRun)
    {
        if (FilePath.Length == 0 || !File.Exists(FilePath))
        {
            return true;
        }

        if (bDryRun)
        {
            Log.Info("Would delete '{0}'", FilePath);
            return true;
        }

        try
        {
            File.Delete(FilePath);
            FileItem.Get(FilePath).Invalidate();
            return true;
        }
        catch (Exception Ex) when (Ex is IOException or UnauthorizedAccessException)
        {
            Log.Warning("Could not delete '{0}': {1}", FilePath, Ex.Message);
            return false;
        }
    }
}
