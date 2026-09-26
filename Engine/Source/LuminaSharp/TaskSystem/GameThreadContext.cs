using System;
using System.Collections.Concurrent;
using System.Threading;

namespace LuminaSharp;

// Every continuation resumes on the game thread, because a job worker runs on a fiber that may migrate.
internal sealed class GameThreadContext : SynchronizationContext
{
    private static readonly ConcurrentQueue<FWork> Pending = new();

    private static int GameThreadId = -1;

    private readonly struct FWork
    {
        internal FWork(SendOrPostCallback Callback, object? State)
        {
            this.Callback = Callback;
            this.State = State;
        }

        internal readonly SendOrPostCallback Callback;
        internal readonly object?            State;
    }

    internal static bool OnGameThread => Environment.CurrentManagedThreadId == GameThreadId;

    // Called from bootstrap, which runs on the game thread, so that thread is the one continuations return to.
    internal static void Install()
    {
        GameThreadId = Environment.CurrentManagedThreadId;
        SetSynchronizationContext(new GameThreadContext());
    }

    public override SynchronizationContext CreateCopy()
    {
        return this;
    }

    public override void Post(SendOrPostCallback Callback, object? State)
    {
        Pending.Enqueue(new FWork(Callback, State));
    }

    // Blocking a worker on the thread that drains the queue would deadlock, so this refuses rather than waits.
    public override void Send(SendOrPostCallback Callback, object? State)
    {
        if (OnGameThread)
        {
            Callback(State);
            return;
        }

        throw new InvalidOperationException(
            "A synchronous Send to the game thread would deadlock, since the game thread is what runs the "
            + "queue. Post the work instead, or await it from the game thread.");
    }

    // Drained once per frame from the managed tick, so a continuation runs with the world in a known state.
    internal static void Drain()
    {
        int Budget = Pending.Count;
        while (Budget-- > 0 && Pending.TryDequeue(out FWork Work))
        {
            try
            {
                Work.Callback(Work.State);
            }
            catch (Exception Exception)
            {
                Interop.LogException(Exception);
            }
        }
    }

    // A queued continuation closes over user code, so an unloading generation drops the queue rather than running it.
    internal static void Clear()
    {
        while (Pending.TryDequeue(out _))
        {
        }
    }
}
