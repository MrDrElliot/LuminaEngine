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
    /// <summary>Runs a tool to completion, merging its standard output and error in arrival order; StandardInput, when set, is written to the tool and then closed.</summary>
    public static async Task<ProcessResult> RunAsync(
        string ToolPath,
        string Arguments,
        string WorkingDirectory,
        IReadOnlyDictionary<string, string>? EnvironmentOverrides,
        CancellationToken Cancellation,
        string? StandardInput = null)
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
            RedirectStandardInput = StandardInput is not null,
        };

        if (StandardInput is not null)
        {
            StartInfo.StandardInputEncoding = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);
        }

        if (EnvironmentOverrides is not null)
        {
            foreach ((string Key, string Value) in EnvironmentOverrides)
            {
                // An empty value removes the variable, since a list-parsing tool reads "" as one empty entry.
                if (Value.Length == 0)
                {
                    StartInfo.Environment.Remove(Key);
                    continue;
                }

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

        if (StandardInput is not null)
        {
            try
            {
                await Runner.StandardInput.WriteAsync(StandardInput.AsMemory(), Cancellation).ConfigureAwait(false);
                Runner.StandardInput.Close();
            }
            catch (IOException)
            {
                // The tool exited before reading everything; its exit code and output say why.
            }
        }

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
