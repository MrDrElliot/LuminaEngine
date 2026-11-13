using System.Diagnostics;
using LuminaBuildTool.Core;
using LuminaBuildTool.Execution;

namespace LuminaBuildTool.Toolchain.Windows;

/// <summary>A located MSVC toolset, with compiler, linker, archiver, and their include and library paths.</summary>
public sealed class MsvcInstallation
{
    public required string InstallationPath { get; init; }

    public required string DisplayName { get; init; }

    public required string ToolsVersion { get; init; }

    /// <summary>Root of the versioned toolset, VC/Tools/MSVC/&lt;version&gt;.</summary>
    public required string ToolsRoot { get; init; }

    /// <summary>Directory containing cl.exe, link.exe and lib.exe for the host and target architecture.</summary>
    public required string BinDirectory { get; init; }

    public string CompilerPath => Path.Combine(BinDirectory, "cl.exe");

    public string LinkerPath => Path.Combine(BinDirectory, "link.exe");

    public string ArchiverPath => Path.Combine(BinDirectory, "lib.exe");

    public string IncludeDirectory => Path.Combine(ToolsRoot, "include");

    public string LibraryDirectory => Path.Combine(ToolsRoot, "lib", "x64");

    public override string ToString() => $"{DisplayName} (MSVC {ToolsVersion})";
}

/// <summary>Finds an installed Visual Studio C++ toolset.</summary>
public static class VisualStudioLocator
{
    private static MsvcInstallation? Cached;

    public static MsvcInstallation Locate()
    {
        if (Cached is not null)
        {
            return Cached;
        }

        List<string> Candidates = new();

        string? FromVsWhere = QueryVsWhere();

        if (FromVsWhere is not null)
        {
            Candidates.Add(FromVsWhere);
        }

        Candidates.AddRange(EnumerateWellKnownInstallRoots());

        foreach (string InstallPath in Candidates)
        {
            MsvcInstallation? Installation = TryCreate(InstallPath);

            if (Installation is not null)
            {
                Log.Verbose("Using {0} at '{1}'", Installation, Installation.InstallationPath);
                Cached = Installation;
                return Installation;
            }
        }

        throw new BuildException(
            "No Visual Studio C++ toolset was found. Install the 'Desktop development with C++' "
            + "workload, or set VCToolsInstallDir to a valid MSVC toolset.");
    }

    private static string? QueryVsWhere()
    {
        string? ProgramFilesX86 = Environment.GetEnvironmentVariable("ProgramFiles(x86)")
            ?? Environment.GetEnvironmentVariable("ProgramFiles");

        if (ProgramFilesX86 is null)
        {
            return null;
        }

        string VsWherePath = Path.Combine(ProgramFilesX86, "Microsoft Visual Studio", "Installer", "vswhere.exe");

        if (!File.Exists(VsWherePath))
        {
            return null;
        }

        ProcessStartInfo StartInfo = new(VsWherePath);

        foreach (string Argument in new[]
        {
            "-latest",
            "-products", "*",
            "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property", "installationPath",
            "-prerelease",
        })
        {
            StartInfo.ArgumentList.Add(Argument);
        }

        string? First = ProcessRunner.RunCaptured(StartInfo, 30_000)?.Output
            .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .FirstOrDefault();

        return string.IsNullOrEmpty(First) ? null : First;
    }

    private static IEnumerable<string> EnumerateWellKnownInstallRoots()
    {
        // An already-initialized developer prompt points straight at the toolset.
        string? VcTools = Environment.GetEnvironmentVariable("VCToolsInstallDir");

        if (!string.IsNullOrEmpty(VcTools) && Directory.Exists(VcTools))
        {
            // Walk back up from VC/Tools/MSVC/<version> to the installation root.
            DirectoryInfo? Walk = new DirectoryInfo(VcTools).Parent?.Parent?.Parent?.Parent;

            if (Walk is not null)
            {
                yield return Walk.FullName;
            }
        }

        foreach (string ProgramFiles in new[]
        {
            Environment.GetEnvironmentVariable("ProgramFiles") ?? string.Empty,
            Environment.GetEnvironmentVariable("ProgramFiles(x86)") ?? string.Empty,
        })
        {
            if (ProgramFiles.Length == 0)
            {
                continue;
            }

            string VisualStudioRoot = Path.Combine(ProgramFiles, "Microsoft Visual Studio");

            if (!Directory.Exists(VisualStudioRoot))
            {
                continue;
            }

            // Newest release year first, then edition.
            foreach (string YearDirectory in Directory.EnumerateDirectories(VisualStudioRoot).OrderByDescending(D => D))
            {
                foreach (string EditionDirectory in Directory.EnumerateDirectories(YearDirectory))
                {
                    yield return EditionDirectory;
                }
            }
        }
    }

    private static MsvcInstallation? TryCreate(string InstallationPath)
    {
        string MsvcRoot = Path.Combine(InstallationPath, "VC", "Tools", "MSVC");

        if (!Directory.Exists(MsvcRoot))
        {
            return null;
        }

        string? ToolsVersion = ReadDefaultToolsVersion(InstallationPath, MsvcRoot) ?? PickNewestToolset(MsvcRoot);

        if (ToolsVersion is null)
        {
            return null;
        }

        string ToolsRoot = Path.Combine(MsvcRoot, ToolsVersion);
        string BinDirectory = Path.Combine(ToolsRoot, "bin", "Hostx64", "x64");

        if (!File.Exists(Path.Combine(BinDirectory, "cl.exe")))
        {
            return null;
        }

        return new MsvcInstallation
        {
            InstallationPath = PathUtils.Normalize(InstallationPath),
            DisplayName = Path.GetFileName(Path.GetDirectoryName(InstallationPath) ?? InstallationPath)
                + " " + Path.GetFileName(InstallationPath),
            ToolsVersion = ToolsVersion,
            ToolsRoot = ToolsRoot,
            BinDirectory = BinDirectory,
        };
    }

    /// <summary>Honors the installation's pinned default toolset so builds match what the IDE uses.</summary>
    private static string? ReadDefaultToolsVersion(string InstallationPath, string MsvcRoot)
    {
        string VersionFile = Path.Combine(
            InstallationPath, "VC", "Auxiliary", "Build", "Microsoft.VCToolsVersion.default.txt");

        if (!File.Exists(VersionFile))
        {
            return null;
        }

        string Version = File.ReadAllText(VersionFile).Trim();

        return Version.Length > 0 && Directory.Exists(Path.Combine(MsvcRoot, Version)) ? Version : null;
    }

    private static string? PickNewestToolset(string MsvcRoot)
    {
        return Directory.EnumerateDirectories(MsvcRoot)
            .Select(Path.GetFileName)
            .Where(Name => Name is not null)
            .Select(Name => Name!)
            .OrderByDescending(WindowsSdkLocator.ParseVersion)
            .FirstOrDefault();
    }

    /// <summary>Product version of the installed IDE, which is what a solution file's version fields carry.</summary>
    public static Version? TryGetIdeVersion()
    {
        try
        {
            string DevEnv = Path.Combine(Locate().InstallationPath, "Common7", "IDE", "devenv.exe");

            if (!File.Exists(DevEnv))
            {
                return null;
            }

            FileVersionInfo Info = FileVersionInfo.GetVersionInfo(DevEnv);

            return Info.FileMajorPart > 0
                ? new Version(Info.FileMajorPart, Info.FileMinorPart, Info.FileBuildPart, Info.FilePrivatePart)
                : null;
        }
        catch (Exception Ex) when (Ex is BuildException or IOException or UnauthorizedAccessException)
        {
            Log.Verbose("Could not read the IDE version: {0}", Ex.Message);
            return null;
        }
    }
}
