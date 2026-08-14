using System.Runtime.InteropServices;
using LuminaBuildTool.Configuration;
using LuminaBuildTool.Core;
using LuminaBuildTool.Platform.Linux;
using LuminaBuildTool.Platform.Windows;

namespace LuminaBuildTool.Platform;

/// <summary>Maps a BuildPlatform onto its implementation.</summary>
public static class BuildPlatformRegistry
{
    private static readonly Dictionary<BuildPlatform, IBuildPlatform> Registered = new();

    static BuildPlatformRegistry()
    {
        Register(new WindowsPlatform());
        Register(new LinuxPlatform());
    }

    public static void Register(IBuildPlatform Platform)
    {
        Registered[Platform.Platform] = Platform;
    }

    public static IBuildPlatform Get(BuildPlatform Platform)
    {
        if (!Registered.TryGetValue(Platform, out IBuildPlatform? Found))
        {
            throw new BuildException(
                $"Platform '{Platform}' is not supported by this build. Supported: {string.Join(", ", Registered.Keys)}");
        }

        return Found;
    }

    public static bool IsSupported(BuildPlatform Platform) => Registered.ContainsKey(Platform);

    /// <summary>Platform matching the machine the tool is running on.</summary>
    public static BuildPlatform HostPlatform
    {
        get
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                return BuildPlatform.Windows64;
            }

            if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
            {
                return BuildPlatform.Linux64;
            }

            if (RuntimeInformation.IsOSPlatform(OSPlatform.OSX))
            {
                return BuildPlatform.Mac64;
            }

            throw new BuildException("Unrecognized host operating system.");
        }
    }
}
