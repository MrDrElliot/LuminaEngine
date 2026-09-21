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
