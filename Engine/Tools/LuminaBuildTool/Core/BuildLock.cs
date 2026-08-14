namespace LuminaBuildTool.Core;

/// <summary>Serializes builds that would write the same output, across processes.</summary>
public sealed class BuildLock : IDisposable
{
    /// <summary>An exclusively opened file rather than a named mutex.</summary>
    private readonly FileStream Handle;

    private BuildLock(FileStream Handle)
    {
        this.Handle = Handle;
    }

    /// <summary>Acquires the lock guarding one shared resource, waiting for whoever already holds it.</summary>
    /// <param name="OutputRoot">Root the lock file is stored under.</param>
    /// <param name="Key">Identifies the shared resource. Two builds with the same key serialize.</param>
    /// <param name="Description">What the caller is waiting for, shown once if it has to wait.</param>
    public static BuildLock Acquire(
        string OutputRoot,
        string Key,
        string Description,
        TimeSpan Timeout,
        CancellationToken Cancellation = default)
    {
        string LockDirectory = Path.Combine(OutputRoot, "Intermediates", "BuildTool", "Locks");
        PathUtils.EnsureDirectory(LockDirectory);

        string LockFile = Path.Combine(LockDirectory, ContentHash.OfString(Key.ToLowerInvariant()) + ".lock");

        DateTime Deadline = DateTime.UtcNow + Timeout;
        bool bReportedWait = false;

        while (true)
        {
            Cancellation.ThrowIfCancellationRequested();

            try
            {
                FileStream Handle = new(
                    LockFile,
                    FileMode.OpenOrCreate,
                    FileAccess.ReadWrite,
                    FileShare.None);

                return new BuildLock(Handle);
            }
            catch (IOException)
            {
                // Held by another build. Anything else that cannot open it is a real problem.
                if (DateTime.UtcNow >= Deadline)
                {
                    throw new BuildException(
                        $"Timed out waiting for {Description}. Close the other build, or wait for it to complete.");
                }

                if (!bReportedWait)
                {
                    Log.Info("Waiting for {0}...", Description);
                    bReportedWait = true;
                }

                Thread.Sleep(250);
            }
            catch (UnauthorizedAccessException Ex)
            {
                throw new BuildException($"Could not create the build lock at '{LockFile}': {Ex.Message}");
            }
        }
    }

    public void Dispose()
    {
        Handle.Dispose();
    }
}
