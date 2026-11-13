using LuminaBuildTool.Core;
using Xunit;

namespace LuminaBuildTool.Tests;

public sealed class BuildLockTests : IDisposable
{
    private readonly string TempDirectory = Directory.CreateTempSubdirectory("BuildLockTests").FullName;

    public void Dispose()
    {
        Directory.Delete(TempDirectory, recursive: true);
    }

    [Fact]
    public void AcquireFile_SecondAcquireBlocksUntilFirstIsDisposed()
    {
        string LockFile = Path.Combine(TempDirectory, "Shared.lock");
        BuildLock First = BuildLock.AcquireFile(LockFile, "first holder", TimeSpan.FromSeconds(5));

        Task<BuildLock> SecondAcquire = Task.Run(
            () => BuildLock.AcquireFile(LockFile, "second holder", TimeSpan.FromSeconds(5)));

        Assert.False(SecondAcquire.Wait(TimeSpan.FromMilliseconds(500)));

        First.Dispose();

        Assert.True(SecondAcquire.Wait(TimeSpan.FromSeconds(5)));
        SecondAcquire.Result.Dispose();
    }

    [Fact]
    public void AcquireFile_ThrowsBuildExceptionWhenTimeoutElapsesWhileHeld()
    {
        string LockFile = Path.Combine(TempDirectory, "Shared.lock");
        using BuildLock First = BuildLock.AcquireFile(LockFile, "first holder", TimeSpan.FromSeconds(5));

        Assert.Throws<BuildException>(
            () => BuildLock.AcquireFile(LockFile, "second holder", TimeSpan.FromMilliseconds(50)));
    }

    [Fact]
    public void AcquireFile_SucceedsImmediatelyAfterHolderDisposes()
    {
        string LockFile = Path.Combine(TempDirectory, "Shared.lock");
        BuildLock First = BuildLock.AcquireFile(LockFile, "first holder", TimeSpan.FromSeconds(5));
        First.Dispose();

        using BuildLock Second = BuildLock.AcquireFile(LockFile, "second holder", TimeSpan.Zero);
    }
}
