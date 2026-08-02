using System.Diagnostics;
using System.Text;
using LuminaBuildTool.Core;

namespace LuminaBuildTool.Execution;

public sealed class ProcessResult
{
    public required int ExitCode { get; init; }

    public required string Output { get; init; }

    public bool bSucceeded => ExitCode == 0;
}

public static class ProcessRunner
{
    /// <summary>
    /// Runs a tool to completion, merging its standard output and error in arrival order.
    /// </summary>
    public static async Task<ProcessResult> RunAsync(
        string ToolPath,
        string Arguments,
        string WorkingDirectory,
        IReadOnlyDictionary<string, string>? EnvironmentOverrides,
        CancellationToken Cancellation)
    {
        ProcessStartInfo StartInfo = new(ToolPath)
        {
            Arguments = Arguments,
            WorkingDirectory = Directory.Exists(WorkingDirectory) ? WorkingDirectory : Directory.GetCurrentDirectory(),
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };

        if (EnvironmentOverrides is not null)
        {
            foreach ((string Key, string Value) in EnvironmentOverrides)
            {
                StartInfo.Environment[Key] = Value;
            }
        }

        using Process Runner = new() { StartInfo = StartInfo };

        StringBuilder Captured = new();
        object Gate = new();

        void Capture(object Sender, DataReceivedEventArgs Args)
        {
            if (Args.Data is not null)
            {
                lock (Gate)
                {
                    Captured.AppendLine(Args.Data);
                }
            }
        }

        Runner.OutputDataReceived += Capture;
        Runner.ErrorDataReceived += Capture;

        try
        {
            Runner.Start();
        }
        catch (Exception Ex) when (Ex is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            throw new BuildException($"Failed to launch '{ToolPath}': {Ex.Message}");
        }

        Runner.BeginOutputReadLine();
        Runner.BeginErrorReadLine();

        try
        {
            await Runner.WaitForExitAsync(Cancellation).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            TryKill(Runner);
            throw;
        }

        string Output;

        lock (Gate)
        {
            Output = Captured.ToString();
        }

        return new ProcessResult { ExitCode = Runner.ExitCode, Output = Output };
    }

    private static void TryKill(Process Runner)
    {
        try
        {
            if (!Runner.HasExited)
            {
                Runner.Kill(entireProcessTree: true);
            }
        }
        catch (Exception Ex) when (Ex is InvalidOperationException or NotSupportedException or SystemException)
        {
            // The process already exited; nothing to clean up.
        }
    }
}
