using LuminaBuildTool.Core;

namespace LuminaBuildTool.Toolchain.Windows;

/// <summary>A located Windows 10/11 SDK.</summary>
public sealed class WindowsSdkInstallation
{
    public required string RootDirectory { get; init; }

    public required string Version { get; init; }

    public IEnumerable<string> IncludeDirectories
    {
        get
        {
            string VersionRoot = Path.Combine(RootDirectory, "Include", Version);

            foreach (string Subdirectory in new[] { "ucrt", "shared", "um", "winrt", "cppwinrt" })
            {
                string Candidate = Path.Combine(VersionRoot, Subdirectory);

                if (Directory.Exists(Candidate))
                {
                    yield return Candidate;
                }
            }
        }
    }

    public IEnumerable<string> LibraryDirectories
    {
        get
        {
            string VersionRoot = Path.Combine(RootDirectory, "Lib", Version);

            foreach (string Subdirectory in new[] { "ucrt", "um" })
            {
                string Candidate = Path.Combine(VersionRoot, Subdirectory, "x64");

                if (Directory.Exists(Candidate))
                {
                    yield return Candidate;
                }
            }
        }
    }

    /// <summary>Directory holding rc.exe and the other SDK tools.</summary>
    public string BinDirectory => Path.Combine(RootDirectory, "bin", Version, "x64");

    public override string ToString() => $"Windows SDK {Version}";
}

/// <summary>Finds an installed Windows SDK by probing the standard kit roots.</summary>
public static class WindowsSdkLocator
{
    private static WindowsSdkInstallation? Cached;

    public static WindowsSdkInstallation Locate()
    {
        if (Cached is not null)
        {
            return Cached;
        }

        foreach (string Root in EnumerateKitRoots())
        {
            string IncludeRoot = Path.Combine(Root, "Include");

            if (!Directory.Exists(IncludeRoot))
            {
                continue;
            }

            // Newest complete SDK wins. A version directory without um/Windows.h is a partial install.
            string? Version = Directory.EnumerateDirectories(IncludeRoot)
                .Select(Path.GetFileName)
                .Where(Name => Name is not null && File.Exists(Path.Combine(IncludeRoot, Name, "um", "Windows.h")))
                .Select(Name => Name!)
                .OrderByDescending(ParseVersion)
                .FirstOrDefault();

            if (Version is null)
            {
                continue;
            }

            WindowsSdkInstallation Installation = new()
            {
                RootDirectory = PathUtils.Normalize(Root),
                Version = Version,
            };

            if (!Installation.LibraryDirectories.Any())
            {
                Log.Verbose("Windows SDK {0} at '{1}' has no x64 libraries; skipping", Version, Root);
                continue;
            }

            Log.Verbose("Using {0} at '{1}'", Installation, Installation.RootDirectory);
            Cached = Installation;

            return Installation;
        }

        throw new BuildException(
            "No Windows SDK was found. Install the Windows 10 or 11 SDK through the Visual Studio Installer.");
    }

    private static IEnumerable<string> EnumerateKitRoots()
    {
        string? Explicit = Environment.GetEnvironmentVariable("WindowsSdkDir");

        if (!string.IsNullOrEmpty(Explicit))
        {
            yield return Explicit;
        }

        foreach (string ProgramFiles in new[]
        {
            Environment.GetEnvironmentVariable("ProgramFiles(x86)") ?? string.Empty,
            Environment.GetEnvironmentVariable("ProgramFiles") ?? string.Empty,
        })
        {
            if (ProgramFiles.Length > 0)
            {
                yield return Path.Combine(ProgramFiles, "Windows Kits", "10");
            }
        }
    }

    private static Version ParseVersion(string Text)
    {
        return Version.TryParse(Text, out Version? Parsed) ? Parsed : new Version(0, 0);
    }
}
