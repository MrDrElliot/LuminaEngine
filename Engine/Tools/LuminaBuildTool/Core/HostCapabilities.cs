using System.Diagnostics;
using System.Runtime.InteropServices;

namespace LuminaBuildTool.Core;

/// <summary>
/// Facts about the machine running the build that rules may branch on. Probes run once and are
/// cached, because a rules file queries them once per module.
/// </summary>
public static class HostCapabilities
{
    private static string? CachedAdapterNames;

    /// <summary>
    /// True when an NVIDIA display adapter is present. Used to decide whether vendor-specific
    /// tooling is worth building in.
    /// </summary>
    public static bool bHasNvidiaGpu => AdapterNames.Contains("nvidia", StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// True when an AMD display adapter is present. Adapter strings read "AMD Radeon ..." for
    /// discrete parts and "AMD Radeon(TM) Graphics" for integrated ones, so either token matches.
    /// </summary>
    public static bool bHasAmdGpu =>
        AdapterNames.Contains("amd", StringComparison.OrdinalIgnoreCase)
        || AdapterNames.Contains("radeon", StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// Comma-joined display adapter names, or empty when the probe could not run. A machine with
    /// both vendors present reports both, and both vendor features turn on.
    /// </summary>
    private static string AdapterNames
    {
        get
        {
            CachedAdapterNames ??= ProbeAdapterNames();
            return CachedAdapterNames;
        }
    }

    private static string ProbeAdapterNames()
    {
        if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            return string.Empty;
        }

        try
        {
            ProcessStartInfo StartInfo = new("powershell")
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            };

            StartInfo.ArgumentList.Add("-NoProfile");
            StartInfo.ArgumentList.Add("-Command");
            StartInfo.ArgumentList.Add("(Get-CimInstance Win32_VideoController).Name -join ','");

            using Process? Runner = Process.Start(StartInfo);

            if (Runner is null)
            {
                return string.Empty;
            }

            string Output = Runner.StandardOutput.ReadToEnd();

            if (!Runner.WaitForExit(20_000))
            {
                return string.Empty;
            }

            return Output;
        }
        catch (Exception Ex) when (Ex is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            Log.Verbose("GPU probe failed: {0}", Ex.Message);
            return string.Empty;
        }
    }
}
