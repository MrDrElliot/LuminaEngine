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

// The bridge from a native broadcast into a managed handler. Holds no state: a binding's GCHandle belongs
// to the native delegate from the moment Bind hands it over, so nothing here has to be tracked or drained.
internal static unsafe class DelegateBindings
{
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

        // Handed to the delegate, which frees it when the binding is unbound or the delegate dies.
        GCHandle Handle = GCHandle.Alloc(Invoker);
        ulong Id = Native.DelegateBind((IntPtr)Address, ThunkPtr, GCHandle.ToIntPtr(Handle));
        if (Id == 0)
        {
            Handle.Free();
            return default;
        }

        return new DelegateBinding((IntPtr)Address, Id);
    }
}
