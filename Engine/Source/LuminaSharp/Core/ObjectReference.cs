using System;
using System.Collections.Concurrent;

namespace LuminaSharp;

// The managed side of a TStrongObjectPtr, counting every reference the canonical wrapper of a CObject has handed out.
internal sealed class ObjectReference
{
    // Dropping the last reference can destroy the object, which only the game thread may do.
    private static readonly ConcurrentQueue<IntPtr> PendingReleases = new();
    private static volatile bool bHostShutDown;

    private IntPtr Object;
    private IntPtr Pin;
    private long Count;
    private bool bOwnerReleased;

    public ObjectReference(IntPtr InObject)
    {
        Object = InObject;
    }

    ~ObjectReference()
    {
        if (Pin != IntPtr.Zero && !bHostShutDown)
        {
            PendingReleases.Enqueue(Pin);
        }
    }

    public void Add()
    {
        lock (this)
        {
            if (!bOwnerReleased && Count++ == 0)
            {
                Pin = Native.PinObject(Object);
            }
        }
    }

    public void Release()
    {
        lock (this)
        {
            if (Count == 0)
            {
                return;
            }
            if (--Count == 0)
            {
                Unpin();
            }
        }
    }

    // The owner is destroying the object, so nothing managed may hold it from here on.
    public void ReleaseAll()
    {
        lock (this)
        {
            bOwnerReleased = true;
            Count = 0;
            Unpin();
        }
    }

    // A reinstance moves the object, so the held reference moves with it; the old one is freed next tick.
    public void Redirect(IntPtr NewObject)
    {
        lock (this)
        {
            Object = NewObject;
            if (Pin != IntPtr.Zero)
            {
                PendingReleases.Enqueue(Pin);
                Pin = Native.PinObject(NewObject);
            }
        }
    }

    private void Unpin()
    {
        if (Pin == IntPtr.Zero)
        {
            return;
        }
        if (GameThreadContext.OnGameThread)
        {
            Native.UnpinObject(Pin);
        }
        else
        {
            PendingReleases.Enqueue(Pin);
        }
        Pin = IntPtr.Zero;
    }

    internal static void DrainReleases()
    {
        while (PendingReleases.TryDequeue(out IntPtr Pending))
        {
            Native.UnpinObject(Pending);
        }
    }

    // Past this point the object array may already be gone, so a late finalizer leaves its pin alone.
    internal static void OnHostShutdown()
    {
        DrainReleases();
        bHostShutDown = true;
    }
}
