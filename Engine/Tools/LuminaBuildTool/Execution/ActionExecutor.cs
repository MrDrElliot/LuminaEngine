using System.Diagnostics;
using System.Text;
using System.Threading.Channels;
using LuminaBuildTool.Core;
using LuminaBuildTool.Graph;

namespace LuminaBuildTool.Execution;

public sealed class ExecutionResult
{
    public int ActionsExecuted { get; set; }

    /// <summary>
    /// Actions the plan expected to run that turned out to be current once their dependencies
    /// finished without changing anything.
    /// </summary>
    public int ActionsSkipped { get; set; }

    public int ActionsFailed { get; set; }

    public TimeSpan Elapsed { get; set; }

    public bool bSucceeded => ActionsFailed == 0;
}

/// <summary>
/// Runs outdated actions in dependency order, as many at a time as the machine allows.
/// </summary>
public sealed class ActionExecutor
{
    private readonly ActionGraph Graph;

    private readonly ActionHistory History;

    private readonly DependencyCache Dependencies;

    private readonly int MaxParallelism;

    private readonly bool bStopOnFirstError;

    /// <summary>One slot per concurrently runnable action; an exclusive action takes them all.</summary>
    private readonly SemaphoreSlim ParallelSlots;

    /// <summary>Serializes exclusive actions so only one ever drains the slot pool.</summary>
    private readonly SemaphoreSlim ExclusiveGate = new(1, 1);

    public ActionExecutor(
        ActionGraph Graph,
        ActionHistory History,
        DependencyCache Dependencies,
        int MaxParallelism,
        bool bStopOnFirstError)
    {
        this.Graph = Graph;
        this.History = History;
        this.Dependencies = Dependencies;
        this.MaxParallelism = Math.Max(1, MaxParallelism);
        this.bStopOnFirstError = bStopOnFirstError;

        ParallelSlots = new SemaphoreSlim(this.MaxParallelism, this.MaxParallelism);
    }

    public async Task<ExecutionResult> ExecuteAsync(IReadOnlyList<BuildAction> OutdatedActions, CancellationToken Cancellation)
    {
        ExecutionResult Result = new();
        Stopwatch Timer = Stopwatch.StartNew();

        if (OutdatedActions.Count == 0)
        {
            Result.Elapsed = Timer.Elapsed;
            return Result;
        }

        HashSet<BuildAction> Pending = new(OutdatedActions);
        Dictionary<BuildAction, int> RemainingDependencies = new();
        Queue<BuildAction> Ready = new();

        // Dependencies outside the outdated set are already up to date, so they do not block.
        foreach (BuildAction Action in OutdatedActions)
        {
            int Blocking = Graph.GetDependencies(Action).Count(Pending.Contains);
            RemainingDependencies[Action] = Blocking;

            if (Blocking == 0)
            {
                Ready.Enqueue(Action);
            }
        }

        if (Ready.Count == 0)
        {
            throw new BuildException("Every outdated action is blocked, which means the action graph has a cycle.");
        }

        using CancellationTokenSource Aborted = CancellationTokenSource.CreateLinkedTokenSource(Cancellation);

        Channel<BuildAction> Queue = Channel.CreateUnbounded<BuildAction>(new UnboundedChannelOptions
        {
            SingleReader = false,
            SingleWriter = false,
        });

        object Gate = new();
        int Total = OutdatedActions.Count;
        int Started = 0;
        int Unfinished = Total;

        foreach (BuildAction Action in Ready)
        {
            Queue.Writer.TryWrite(Action);
        }

        // A remaining count of zero marks an action as retired, so it is never counted twice.
        // Runs under Gate.
        void RetireLocked(BuildAction Action, bool bSucceeded)
        {
            Unfinished--;

            foreach (BuildAction Dependent in Graph.GetDependents(Action))
            {
                if (!RemainingDependencies.TryGetValue(Dependent, out int Remaining) || Remaining <= 0)
                {
                    continue;
                }

                if (!bSucceeded)
                {
                    // A failed dependency permanently blocks its dependents, which is what stops
                    // a broken build from linking against stale outputs.
                    RemainingDependencies[Dependent] = 0;
                    RetireLocked(Dependent, bSucceeded: false);
                    continue;
                }

                RemainingDependencies[Dependent] = Remaining - 1;

                if (Remaining - 1 == 0)
                {
                    Queue.Writer.TryWrite(Dependent);
                }
            }
        }

        void Retire(BuildAction Action, bool bSucceeded)
        {
            lock (Gate)
            {
                RemainingDependencies[Action] = 0;
                RetireLocked(Action, bSucceeded);

                if (Unfinished <= 0)
                {
                    Queue.Writer.TryComplete();
                }
            }
        }

        async Task WorkerAsync()
        {
            while (await Queue.Reader.WaitToReadAsync(Aborted.Token).ConfigureAwait(false))
            {
                while (Queue.Reader.TryRead(out BuildAction? Action))
                {
                    int Index;

                    lock (Gate)
                    {
                        Index = ++Started;
                    }

                    // Every dependency has finished, so the inputs are final. Rechecking here
                    // catches the case where a dependency ran but rewrote nothing, which is the
                    // normal outcome for a code generator whose inputs changed cosmetically.
                    if (!ActionGraph.IsOutdated(Action, History, Dependencies, out string Reason))
                    {
                        Log.Verbose("Skipping {0}: still up to date", DescribeAction(Action));

                        lock (Gate)
                        {
                            Result.ActionsSkipped++;
                        }

                        Retire(Action, bSucceeded: true);
                        continue;
                    }

                    Log.Info("[{0}/{1}] {2}", Index, Total, DescribeAction(Action));
                    Log.Trace("  because: {0}", Reason);

                    bool bSucceeded = await RunWithConcurrencyLimitAsync(Action, Aborted.Token).ConfigureAwait(false);

                    lock (Gate)
                    {
                        if (bSucceeded)
                        {
                            Result.ActionsExecuted++;
                        }
                        else
                        {
                            Result.ActionsFailed++;
                        }
                    }

                    Retire(Action, bSucceeded);

                    if (!bSucceeded && bStopOnFirstError)
                    {
                        Queue.Writer.TryComplete();
                        return;
                    }
                }
            }
        }

        Task[] Workers = Enumerable.Range(0, MaxParallelism).Select(_ => WorkerAsync()).ToArray();

        try
        {
            await Task.WhenAll(Workers).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // Cancellation is reported through ActionsFailed.
        }

        Result.Elapsed = Timer.Elapsed;

        return Result;
    }

    /// <summary>
    /// Applies the action's concurrency requirement. An action that cannot run in parallel gets
    /// the machine to itself: a code generator or an MSBuild invocation already parallelizes
    /// internally, and a second one would contend for the same cores and the same locks.
    /// </summary>
    /// <remarks>
    /// The exclusive gate is what makes draining the slots safe. Without it two exclusive actions
    /// could each hold part of the pool and wait forever for the rest.
    /// </remarks>
    private async Task<bool> RunWithConcurrencyLimitAsync(BuildAction Action, CancellationToken Cancellation)
    {
        if (Action.bCanExecuteInParallel)
        {
            // Blocks while an exclusive action is collecting the pool, so it cannot be starved.
            await ExclusiveGate.WaitAsync(Cancellation).ConfigureAwait(false);
            ExclusiveGate.Release();

            await ParallelSlots.WaitAsync(Cancellation).ConfigureAwait(false);

            try
            {
                return await ExecuteActionAsync(Action, Cancellation).ConfigureAwait(false);
            }
            finally
            {
                ParallelSlots.Release();
            }
        }

        await ExclusiveGate.WaitAsync(Cancellation).ConfigureAwait(false);

        try
        {
            for (int Slot = 0; Slot < MaxParallelism; Slot++)
            {
                await ParallelSlots.WaitAsync(Cancellation).ConfigureAwait(false);
            }

            try
            {
                return await ExecuteActionAsync(Action, Cancellation).ConfigureAwait(false);
            }
            finally
            {
                ParallelSlots.Release(MaxParallelism);
            }
        }
        finally
        {
            ExclusiveGate.Release();
        }
    }

    private async Task<bool> ExecuteActionAsync(BuildAction Action, CancellationToken Cancellation)
    {
        try
        {
            PrepareDirectories(Action);

            if (Action.Operation is not null)
            {
                return ExecuteOperation(Action, Action.Operation);
            }

            WriteResponseFile(Action);

            ProcessResult Process = await ProcessRunner.RunAsync(
                Action.ToolPath,
                Action.Arguments,
                Action.WorkingDirectory,
                Action.EnvironmentOverrides,
                Cancellation).ConfigureAwait(false);

            string Output = FilterToolOutput(Action, Process.Output);

            if (Output.Length > 0)
            {
                Log.Raw(Output);
            }

            if (!Process.bSucceeded && !Action.bIgnoreExitCode)
            {
                Log.Error("{0} failed with exit code {1}", DescribeAction(Action), Process.ExitCode);
                InvalidateOutputs(Action);
                return false;
            }

            RecordSuccess(Action);
            return true;
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (BuildException Ex)
        {
            Log.Error("{0}: {1}", DescribeAction(Action), Ex.Message);
            return false;
        }
        catch (IOException Ex)
        {
            Log.Error("{0}: {1}", DescribeAction(Action), Ex.Message);
            return false;
        }
    }

    /// <summary>
    /// Runs an action that does its work in process.
    /// </summary>
    private bool ExecuteOperation(BuildAction Action, BuildOperation Operation)
    {
        try
        {
            Operation.Execute();
            RecordSuccess(Action);
            return true;
        }
        catch (Exception Ex) when (Ex is IOException or UnauthorizedAccessException or FileNotFoundException)
        {
            // A tolerated failure only counts as success when the output is already what the
            // operation would have written. Recording success on an unverified output would
            // freeze a stale file in place: the input stamp would advance and the build would
            // never try again.
            if (Action.bIgnoreExitCode && Operation is CopyFileOperation Copy && Copy.IsAlreadyStaged())
            {
                Log.Verbose(
                    "'{0}' is already up to date on disk; leaving it alone ({1})",
                    Path.GetFileName(Copy.Destination),
                    Ex.Message);

                RecordSuccess(Action);
                return true;
            }

            if (Action.bIgnoreExitCode)
            {
                // Deliberately not recorded, so the next build retries rather than trusting
                // whatever happens to be on disk.
                Log.Warning("{0} did not complete: {1}", DescribeAction(Action), Ex.Message);
                return true;
            }

            Log.Error("{0} failed: {1}", DescribeAction(Action), Ex.Message);
            return false;
        }
    }

    private void RecordSuccess(BuildAction Action)
    {
        // Drop the cached stats so the recheck of any dependent sees the new timestamps.
        foreach (FileItem Produced in Action.AllProducedItems)
        {
            Produced.Invalidate();
        }

        if (Action.ProducedItems.Count > 0)
        {
            string PrimaryOutput = Action.ProducedItems[0].Location;

            History.RecordCommand(PrimaryOutput, Action.GetCommandKey());

            // Headers first: they are part of what this object was built from, so the fingerprint
            // has to see the list the compiler just reported rather than the previous build's.
            if (Action.DependencyListFile is not null)
            {
                Dependencies.RecordFromCompilerOutput(PrimaryOutput, Action.DependencyListFile);
            }

            History.RecordInputFingerprint(PrimaryOutput, ActionGraph.ComputeInputFingerprint(Action, Dependencies));
        }
    }

    /// <summary>
    /// Drops the cached state for a failed action's outputs so a partial write is never trusted
    /// as up to date on the next build.
    /// </summary>
    private void InvalidateOutputs(BuildAction Action)
    {
        foreach (FileItem Produced in Action.AllProducedItems)
        {
            Produced.Invalidate();
            History.Forget(Produced.Location);
            Dependencies.Forget(Produced.Location);
        }
    }

    private static void PrepareDirectories(BuildAction Action)
    {
        foreach (FileItem Produced in Action.AllProducedItems)
        {
            PathUtils.EnsureDirectoryForFile(Produced.Location);
        }

        if (Action.WorkingDirectory.Length > 0)
        {
            PathUtils.EnsureDirectory(Action.WorkingDirectory);
        }
    }

    private static void WriteResponseFile(BuildAction Action)
    {
        if (Action.ResponseFilePath is null || Action.ResponseFileContents is null)
        {
            return;
        }

        PathUtils.EnsureDirectoryForFile(Action.ResponseFilePath);
        File.WriteAllText(Action.ResponseFilePath, Action.ResponseFileContents, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
    }

    /// <summary>
    /// Strips the source file name cl.exe echoes before its diagnostics, which would otherwise
    /// double every compile line in the log.
    /// </summary>
    private static string FilterToolOutput(BuildAction Action, string Output)
    {
        if (Action.Type != ActionType.Compile || Output.Length == 0 || Action.EchoedInputName.Length == 0)
        {
            return Output;
        }

        string[] Lines = Output.Split('\n');
        StringBuilder Filtered = new(Output.Length);

        foreach (string Line in Lines)
        {
            string Trimmed = Line.TrimEnd('\r');

            if (Trimmed.Equals(Action.EchoedInputName, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            if (Trimmed.Length > 0)
            {
                Filtered.AppendLine(Trimmed);
            }
        }

        return Filtered.ToString();
    }

    private static string DescribeAction(BuildAction Action)
    {
        return Action.Type switch
        {
            ActionType.Compile => $"Compile {Action.ModuleName}: {Action.StatusText}",
            ActionType.Link => $"Link {Action.StatusText}",
            ActionType.Archive => $"Archive {Action.StatusText}",
            ActionType.Generate => $"Generate {Action.ModuleName}: {Action.StatusText}",
            _ => $"{Action.Type} {Action.StatusText}",
        };
    }
}
