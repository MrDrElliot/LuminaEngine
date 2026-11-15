using System.Diagnostics;
using LuminaBuildTool.Execution;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class ProcessRunnerTests
{
    [Fact]
    public void RunCaptured_ReturnsWithinTimeout_WhenGrandchildHoldsOutputHandleOpen()
    {
        string WorkingDirectory = Directory.CreateTempSubdirectory("ProcessRunnerTests").FullName;
        string Marker = Path.Combine(WorkingDirectory, "survived.txt");

        // The grandchild holds the inherited output pipe for about three seconds, then leaves a marker if it was not killed.
        ProcessStartInfo StartInfo = new("cmd.exe")
        {
            Arguments = "/c start /B cmd /c \"ping -n 4 127.0.0.1 >nul & type nul > survived.txt\"",
            WorkingDirectory = WorkingDirectory,
        };

        const int TimeoutMs = 1000;
        const int WatchdogMs = TimeoutMs + 10_000;

        try
        {
            Task<ProcessResult?> Run = Task.Run(() => ProcessRunner.RunCaptured(StartInfo, TimeoutMs));

            Assert.True(Run.Wait(WatchdogMs), "RunCaptured did not return even after waiting past its own timeout.");
            Assert.Null(Run.Result);

            Thread.Sleep(4000);
            Assert.False(File.Exists(Marker), "The grandchild holding the output pipe outlived RunCaptured's timeout.");
        }
        finally
        {
            Directory.Delete(WorkingDirectory, recursive: true);
        }
    }
}
