using System.Diagnostics;
using System.Text;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Modes;

namespace LuminaBuildTool.Platform.Linux;

// A Windows host builds Linux64 inside WSL, from a mirror on WSL's own disk since /mnt is case-insensitive and slow.
public static class WslBuild
{
    private const string MirrorsDirectory = "$HOME/.lumina/wsl";

    public static bool IsNeeded(BuildPlatform Platform) => Platform == BuildPlatform.Linux64 && BuildPlatformRegistry.HostPlatform == BuildPlatform.Windows64;

    public static int Run(
        IReadOnlyList<string> TargetNames,
        CommandLine Arguments,
        BuildDirectories Directories,
        TargetType Type,
        BuildConfiguration Configuration)
    {
        string EngineMirror = MirrorOf(Directories.EngineRoot);
        string? ProjectMirror = Directories.ProjectRoot is null ? null : MirrorOf(Directories.ProjectRoot);

        string Forwarded = string.Join(' ', Arguments
            .OptionArguments("EngineRoot", "Project", "Platform", "TargetType", "Configuration")
            .Select(PathUtils.QuoteUnix));
        string Project = ProjectMirror is null ? string.Empty : $" -Project=\"{ProjectMirror}\"";

        List<string> Script = new() { "set -euo pipefail", SyncIn(Directories.EngineRoot, EngineMirror) };
        if (ProjectMirror is not null)
        {
            Script.Add(SyncIn(Directories.ProjectRoot!, ProjectMirror));
        }

        // A Windows checkout carries the script with CRLF endings, which bash reads as part of each command.
        Script.Add($"cd \"{EngineMirror}\" && sed -i 's/\\r$//' LuminaBuild.sh");

        // Setup fetches the Linux dependency bundle once and returns at once while it is present.
        Script.Add("./LuminaBuild.sh Setup -Yes");
        foreach (string Target in TargetNames)
        {
            Script.Add(
                $"./LuminaBuild.sh Build {PathUtils.QuoteUnix(Target)} -TargetType={Type} -Configuration={Configuration} -Platform=Linux64{Project} {Forwarded}");
        }

        Script.Add(SyncOut(EngineMirror, Directories.EngineRoot));
        Script.Add(SyncRuntimeOut(EngineMirror, Directories.EngineRoot));
        if (ProjectMirror is not null)
        {
            Script.Add(SyncOut(ProjectMirror, Directories.ProjectRoot!));
        }

        string ScriptPath = Path.Combine(Directories.BuildToolIntermediatesDirectory, "WslBuild.sh");
        PathUtils.EnsureDirectoryForFile(ScriptPath);
        File.WriteAllText(ScriptPath, string.Join('\n', Script) + '\n', new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));

        Log.Info("Building Linux64 inside WSL at {0}.", EngineMirror);

        // A login shell, so the distribution's profile picks the compiler and the .NET SDK.
        ProcessStartInfo StartInfo = new("wsl.exe") { UseShellExecute = false };
        foreach (string Argument in new[] { "-e", "bash", "-l", ToWslPath(ScriptPath) })
        {
            StartInfo.ArgumentList.Add(Argument);
        }

        using Process Child = Process.Start(StartInfo)
            ?? throw new BuildException("Could not start wsl.exe. Building Linux64 on Windows needs WSL with a Linux distribution installed.");
        Child.WaitForExit();
        return Child.ExitCode;
    }

    // H:\Engine becomes /mnt/h/Engine, where WSL mounts every Windows drive.
    public static string ToWslPath(string WindowsPath)
    {
        string Full = Path.GetFullPath(WindowsPath).Replace('\\', '/');
        return Full.Length >= 2 && Full[1] == ':' ? $"/mnt/{char.ToLowerInvariant(Full[0])}{Full.Substring(2)}" : Full;
    }

    // One mirror per Windows tree, named after its path so two checkouts never share intermediates.
    private static string MirrorOf(string WindowsRoot)
    {
        string Name = string.Concat(Path.GetFullPath(WindowsRoot).TrimEnd('\\', '/').Replace(":", string.Empty)
            .Select(Character => char.IsLetterOrDigit(Character) || Character is '-' or '.' ? Character : '_'));
        return $"{MirrorsDirectory}/{Name}";
    }

    // Build output and External stay out of --delete, and External has no slash since a junction is a link to rsync.
    private static string SyncIn(string WindowsRoot, string Mirror)
    {
        IEnumerable<string> Excludes = PackageMode.ExcludedDirectoryNames.Select(Name => $"--exclude={Name}/")
            .Append("--exclude=/.git").Append("--exclude=/External").Append("--exclude=/Build").Append("--exclude=*.sln");
        return $"mkdir -p \"{Mirror}\" && rsync -a --delete {string.Join(' ', Excludes)} \"{ToWslPath(WindowsRoot)}/\" \"{Mirror}/\"";
    }

    // Only Binaries/Linux64 comes back, at the root, inside each plugin and in the project.
    private static string SyncOut(string Mirror, string WindowsRoot)
    {
        return "rsync -a --prune-empty-dirs --exclude=Intermediates/ --exclude=/External/ --include='*/' --include='Binaries/Linux64/***' --exclude='*' "
            + $"\"{Mirror}/\" \"{ToWslPath(WindowsRoot)}/\"";
    }

    // External may be a junction into another checkout, so the runtime Setup fetched comes back beside the Linux binaries.
    public static string SyncedDotNetRuntime(string EngineRoot) =>
        Path.Combine(EngineRoot, "Binaries", BuildPlatform.Linux64.GetOutputDirectoryName(), "DotNet", "runtime", "linux-x64");

    private static string SyncRuntimeOut(string Mirror, string WindowsRoot)
    {
        string Destination = ToWslPath(SyncedDotNetRuntime(WindowsRoot));
        return $"mkdir -p \"{Destination}\" && rsync -a --delete \"{Mirror}/External/DotNet/runtime/linux-x64/\" \"{Destination}/\"";
    }
}
