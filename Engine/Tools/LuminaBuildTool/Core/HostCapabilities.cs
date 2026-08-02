using System.Diagnostics;
using System.Runtime.InteropServices;

namespace LuminaBuildTool.Core;

/// <summary>
/// Facts about the machine running the build that rules may branch on. Probes run once and are
/// cached, because a rules file queries them once per module.
/// </summary>
public static class HostCapabilities
{
    private static bool? CachedNvidiaGpu;

    /// <summary>
    /// True when an NVIDIA display adapter is present. Used to decide whether vendor-specific
    /// tooling is worth building in.
    /// </summary>
    public static bool bHasNvidiaGpu
    {
        get
        {
            CachedNvidiaGpu ??= DetectNvidiaGpu();
            return CachedNvidiaGpu.Value;
        }
    }

    private static bool DetectNvidiaGpu()
    {
        if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
        {
            return false;
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
                return false;
            }

            string Output = Runner.StandardOutput.ReadToEnd();

            if (!Runner.WaitForExit(20_000))
            {
                return false;
            }

            return Output.Contains("nvidia", StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception Ex) when (Ex is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            Log.Verbose("GPU probe failed: {0}", Ex.Message);
            return false;
        }
    }
}
