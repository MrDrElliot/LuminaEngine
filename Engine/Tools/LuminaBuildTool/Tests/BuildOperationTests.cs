using System.Diagnostics;
using LuminaBuildTool.Core;
using LuminaBuildTool.Execution;
using LuminaBuildTool.Graph;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class BuildOperationTests
{
    private readonly string MissingHistoryFile = Path.Combine(Path.GetTempPath(), Guid.NewGuid() + ".json");

    private static string WriteTempFile(string Contents)
    {
        string FilePath = Path.Combine(Path.GetTempPath(), Guid.NewGuid() + ".tmp");
        File.WriteAllText(FilePath, Contents);
        return FilePath;
    }

    // FileSystemAccessRule fails to translate the current SID in a sandboxed session, so this shells out to icacls.
    private static void RunIcacls(string Arguments)
    {
        using Process Tool = Process.Start(new ProcessStartInfo("icacls", Arguments)
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        })!;

        Tool.WaitForExit();
        Assert.Equal(0, Tool.ExitCode);
    }

    private static void DenyAccess(string FilePath, string Rights) => RunIcacls($"\"{FilePath}\" /deny \"{Environment.UserName}:({Rights})\"");

    private static void RestoreAccess(string FilePath) => RunIcacls($"\"{FilePath}\" /remove:d \"{Environment.UserName}\"");

    [Fact]
    public void IsAlreadyStagedReturnsFalseWhenDestinationDeniesRead()
    {
        string Source = WriteTempFile("same bytes");
        string Destination = WriteTempFile("same bytes");

        try
        {
            DenyAccess(Destination, "R");

            CopyFileOperation Operation = new(Source, Destination);
            Assert.False(Operation.IsAlreadyStaged());
        }
        finally
        {
            RestoreAccess(Destination);
            File.Delete(Source);
            File.Delete(Destination);
        }
    }

    [Fact]
    public void WriteFileOperationLeavesPriorContentWhenTheWriteFails()
    {
        string Destination = WriteTempFile("prior valid content");
        string BlockedTempPath = $"{Destination}.{Environment.ProcessId}.tmp";

        try
        {
            // A directory at the temp sibling makes the write fail partway, as a killed build would.
            Directory.CreateDirectory(BlockedTempPath);

            WriteFileOperation Operation = new(Destination, "new content");
            Assert.ThrowsAny<UnauthorizedAccessException>(Operation.Execute);

            Assert.Equal("prior valid content", File.ReadAllText(Destination));
        }
        finally
        {
            Directory.Delete(BlockedTempPath);
            File.Delete(Destination);
        }
    }

    [Fact]
    public async Task LockedDestinationWithIgnoreExitCodeDoesNotFailTheAction()
    {
        string Source = WriteTempFile("same bytes");
        string Destination = WriteTempFile("same bytes");

        try
        {
            DenyAccess(Destination, "R,W");

            BuildAction Action = new(ActionType.Copy, "Test")
            {
                StatusText = "Copy",
                Operation = new CopyFileOperation(Source, Destination),
                bIgnoreExitCode = true,
            };

            ActionExecutor Executor = new(
                ActionGraphTests.BuildGraph(Action),
                ActionHistory.Load(MissingHistoryFile),
                DependencyCache.Load(MissingHistoryFile),
                MaxParallelism: 1,
                bStopOnFirstError: false);

            ExecutionResult Result = await Executor.ExecuteAsync([Action], CancellationToken.None);

            Assert.Equal(1, Result.ActionsExecuted);
            Assert.Equal(0, Result.ActionsFailed);
        }
        finally
        {
            RestoreAccess(Destination);
            File.Delete(Source);
            File.Delete(Destination);
        }
    }
}
