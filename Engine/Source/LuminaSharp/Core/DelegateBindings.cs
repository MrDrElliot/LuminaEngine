using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

internal interface IDelegateInvoker
{
    void Invoke(IntPtr Payload);
}

internal sealed class VoidInvoker : IDelegateInvoker
{
    public Action Handler = null!;
    public EntityScript? Owner;

    public void Invoke(IntPtr Payload)
    {
        if (Owner is { } Script)
        {
            using (Game.Push(Script.World, Script.Entity))
            {
                Handler();
            }
        }
        else
        {
            Handler();
        }
    }
}

internal sealed class PayloadInvoker<T> : IDelegateInvoker where T : unmanaged
{
    public Action<T> Handler = null!;
    public EntityScript? Owner;

    public unsafe void Invoke(IntPtr Payload)
    {
        T Argument = Unsafe.Read<T>((void*)Payload);
        if (Owner is { } Script)
        {
            using (Game.Push(Script.World, Script.Entity))
            {
                Handler(Argument);
            }
        }
        else
        {
            Handler(Argument);
        }
    }
}

// Per-process registry owning every managed delegate binding's GCHandle. Game thread only.
internal static unsafe class DelegateBindings
{
    private struct Record
    {
        public IntPtr   Address;
        public ulong    Id;
        public GCHandle Handle;
    }

    private static readonly Dictionary<ulong, Record> ById = new();

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void Thunk(IntPtr Context, IntPtr Payload)
    {
        try
        {
            if (GCHandle.FromIntPtr(Context).Target is IDelegateInvoker Invoker)
            {
                Invoker.Invoke(Payload);
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    private static readonly IntPtr ThunkPtr =
        (IntPtr)(delegate* unmanaged[Cdecl]<IntPtr, IntPtr, void>)&Thunk;

    internal static DelegateBinding Bind(void* Address, IDelegateInvoker Invoker)
    {
        if (Address == null)
        {
            return default;
        }

        GCHandle Handle = GCHandle.Alloc(Invoker);
        ulong Id = Native.DelegateBind((IntPtr)Address, ThunkPtr, GCHandle.ToIntPtr(Handle));
        if (Id == 0)
        {
            Handle.Free();
            return default;
        }

        ById[Id] = new Record { Address = (IntPtr)Address, Id = Id, Handle = Handle };
        return new DelegateBinding(Id);
    }

    internal static void Unbind(ulong Id)
    {
        if (Id == 0 || !ById.Remove(Id, out Record Rec))
        {
            return;
        }
        Native.DelegateUnbind(Rec.Address, Rec.Id);
        if (Rec.Handle.IsAllocated)
        {
            Rec.Handle.Free();
        }
    }

    // Native delegate at Address was destroyed; free our handles without a native unbind.
    internal static void ForgetByAddress(IntPtr Address)
    {
        if (ById.Count == 0)
        {
            return;
        }

        List<ulong>? Dead = null;
        foreach (KeyValuePair<ulong, Record> Pair in ById)
        {
            if (Pair.Value.Address == Address)
            {
                (Dead ??= new List<ulong>()).Add(Pair.Key);
            }
        }
        if (Dead == null)
        {
            return;
        }

        foreach (ulong Id in Dead)
        {
            if (ById.Remove(Id, out Record Rec) && Rec.Handle.IsAllocated)
            {
                Rec.Handle.Free();
            }
        }
    }

    // Hot-reload or shutdown; unbind every binding and free all handles.
    internal static void PurgeAll()
    {
        foreach (KeyValuePair<ulong, Record> Pair in ById)
        {
            Record Rec = Pair.Value;
            Native.DelegateUnbind(Rec.Address, Rec.Id);
            if (Rec.Handle.IsAllocated)
            {
                Rec.Handle.Free();
            }
        }
        ById.Clear();
    }
}
