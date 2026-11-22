using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// A delegate listener, with the broadcast's argument pack as the payload.
internal abstract class DelegateInvoker : ScriptClosure
{
    internal abstract Delegate BoundHandler { get; }

    protected abstract void Run(IntPtr Payload);

    public sealed override ulong Invoke(ulong Payload)
    {
        Run((IntPtr)Payload);
        return 0;
    }
}

internal sealed class VoidInvoker : DelegateInvoker
{
    public Action Handler = null!;

    internal override Delegate BoundHandler => Handler;

    protected override void Run(IntPtr Payload) => Handler();
}

internal sealed class PayloadInvoker<T> : DelegateInvoker where T : unmanaged
{
    public Action<T> Handler = null!;

    internal override Delegate BoundHandler => Handler;

    protected override unsafe void Run(IntPtr Payload) => Handler(Unsafe.Read<T>((void*)Payload));
}

// The delegate owns the callback from the moment it is bound and releases it when the binding retires.
internal static unsafe class DelegateBindings
{
    internal static DelegateBinding Bind(void* Address, DelegateInvoker Invoker)
    {
        if (Address == null)
        {
            return default;
        }

        // A script's own method is tied to it, so the binding ends when the script detaches without an OnDetach unbind.
        IntPtr Owner = Invoker.BoundHandler.Target is Lumina.CEntityScript Script ? Script.Self : IntPtr.Zero;
        ulong Id = Native.DelegateBind((IntPtr)Address, ScriptCallback.Bind(Invoker).Token, Owner);
        return Id == 0 ? default : new DelegateBinding((IntPtr)Address, Id);
    }

    // Like -= on a C# event, this drops the most recent binding whose handler equals Handler, so a method group removes what it added.
    internal static void Remove(void* Address, Delegate? Handler)
    {
        if (Address == null || Handler == null)
        {
            return;
        }

        const int InlineCapacity = 16;
        ulong* InlineIds = stackalloc ulong[InlineCapacity];
        ulong* InlineTokens = stackalloc ulong[InlineCapacity];
        int Count = Native.DelegateManagedListeners((IntPtr)Address, InlineIds, InlineTokens, InlineCapacity);
        if (Count <= InlineCapacity)
        {
            RemoveLastMatch(Address, Handler, InlineIds, InlineTokens, Count);
            return;
        }

        ulong[] Ids = new ulong[Count];
        ulong[] Tokens = new ulong[Count];
        fixed (ulong* IdsPtr = Ids)
        fixed (ulong* TokensPtr = Tokens)
        {
            Count = Math.Min(Count, Native.DelegateManagedListeners((IntPtr)Address, IdsPtr, TokensPtr, Count));
            RemoveLastMatch(Address, Handler, IdsPtr, TokensPtr, Count);
        }
    }

    private static void RemoveLastMatch(void* Address, Delegate Handler, ulong* Ids, ulong* Tokens, int Count)
    {
        for (int Index = Count - 1; Index >= 0; --Index)
        {
            if (Tokens[Index] != 0 && GCHandle.FromIntPtr((IntPtr)(long)Tokens[Index]).Target is DelegateInvoker Invoker && Invoker.BoundHandler.Equals(Handler))
            {
                Native.DelegateUnbind((IntPtr)Address, Ids[Index]);
                return;
            }
        }
    }
}
