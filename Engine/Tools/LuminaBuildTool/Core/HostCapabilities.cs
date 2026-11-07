using System.Security;
using Microsoft.Win32;

namespace LuminaBuildTool.Core;

/// <summary>Facts about the machine running the build that rules may branch on.</summary>
public static class HostCapabilities
{
    // The display adapter device class; each numbered subkey is one installed adapter driver.
    private const string DisplayAdapterClassKey = @"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}";

    private static string? CachedAdapterNames;

    /// <summary>True when an NVIDIA display adapter is present.</summary>
    public static bool bHasNvidiaGpu => AdapterNames.Contains("nvidia", StringComparison.OrdinalIgnoreCase);

    /// <summary>True when an AMD display adapter is present.</summary>
    public static bool bHasAmdGpu =>
        AdapterNames.Contains("amd", StringComparison.OrdinalIgnoreCase)
        || AdapterNames.Contains("radeon", StringComparison.OrdinalIgnoreCase);

    /// <summary>Comma-joined display adapter names, or empty off Windows.</summary>
    private static string AdapterNames
    {
        get
        {
            CachedAdapterNames ??= ReadAdapterNames();
            return CachedAdapterNames;
        }
    }

    // Read in process rather than through a timed-out WMI query, so a loaded machine cannot flip the rules' defines.
    private static string ReadAdapterNames()
    {
        if (!OperatingSystem.IsWindows())
        {
            return string.Empty;
        }

        using RegistryKey? ClassKey = Registry.LocalMachine.OpenSubKey(DisplayAdapterClassKey);
        if (ClassKey == null)
        {
            return string.Empty;
        }

        List<string> Names = new();
        foreach (string SubKeyName in ClassKey.GetSubKeyNames())
        {
            try
            {
                using RegistryKey? AdapterKey = ClassKey.OpenSubKey(SubKeyName);
                if (AdapterKey?.GetValue("DriverDesc") is string Name)
                {
                    Names.Add(Name);
                }
            }
            catch (SecurityException)
            {
            }
        }

        return string.Join(',', Names);
    }
}
