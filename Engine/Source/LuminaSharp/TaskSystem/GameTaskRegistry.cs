using System;
using System.Collections.Generic;

namespace LuminaSharp;

// A pending await outlives nothing, so an unloading generation cancels every one before its code goes away.
internal static class GameTaskRegistry
{
    private static readonly HashSet<ICancelPending> Pending = new();

    internal interface ICancelPending
    {
        void Cancel();

        // The world the await is waiting on, zero when it waits on none.
        ulong World { get; }
    }

    internal static int PendingCount
    {
        get
        {
            lock (Pending)
            {
                return Pending.Count;
            }
        }
    }

    internal static void Track(ICancelPending Source)
    {
        lock (Pending)
        {
            Pending.Add(Source);
        }
    }

    internal static void Forget(ICancelPending Source)
    {
        lock (Pending)
        {
            Pending.Remove(Source);
        }
    }

    // Its timers are cleared with the world, so anything still waiting on them would otherwise never resume.
    internal static void CancelWorld(ulong World)
    {
        List<ICancelPending> Doomed = new();
        lock (Pending)
        {
            foreach (ICancelPending Source in Pending)
            {
                if (Source.World == World)
                {
                    Doomed.Add(Source);
                }
            }
        }

        foreach (ICancelPending Source in Doomed)
        {
            try
            {
                Source.Cancel();
            }
            catch (Exception Exception)
            {
                Interop.LogException(Exception);
            }
        }
    }

    // Canceled first so an await site observes it, then the queue is dropped since that code is unloading.
    internal static void CancelAll()
    {
        ICancelPending[] Snapshot;
        lock (Pending)
        {
            Snapshot = new ICancelPending[Pending.Count];
            Pending.CopyTo(Snapshot);
            Pending.Clear();
        }

        foreach (ICancelPending Source in Snapshot)
        {
            try
            {
                Source.Cancel();
            }
            catch (Exception Exception)
            {
                Interop.LogException(Exception);
            }
        }

        GameThreadContext.Clear();
    }
}
