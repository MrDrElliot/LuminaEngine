using System;

namespace LuminaSharp;

/** A live subscription to a script delegate. Unbind it before the handler's script goes away, in OnDetach
 *  if you bound in OnAttach: nothing detaches it for you, and a binding left alive at a hot reload pins the
 *  script load context so the reload cannot unload it. Unbinding twice is safe, but unbinding after the
 *  object owning the delegate has been destroyed is not, because the delegate is gone with it. */
public struct DelegateBinding : IDisposable
{
    private IntPtr Address;
    private ulong  Id;

    internal DelegateBinding(IntPtr Address, ulong Id)
    {
        this.Address = Address;
        this.Id = Id;
    }

    public bool IsValid => Id != 0;

    public void Unbind()
    {
        if (Id != 0)
        {
            Native.DelegateUnbind(Address, Id);
            Address = IntPtr.Zero;
            Id = 0;
        }
    }

    public void Dispose() => Unbind();
}

// Transient handle to a no-payload multicast event; do not store it, re-fetch the accessor.
[NativeSlotView]
public readonly unsafe struct ScriptDelegate
{
    private readonly void* Address;

    public ScriptDelegate(void* Address)
    {
        this.Address = Address;
    }

    // The slot address as an integer, which is the shape a binder building this view by reflection has.
    public ScriptDelegate(nint Address)
    {
        this.Address = (void*)Address;
    }

    public bool IsValid => Address != null;

    public DelegateBinding Bind(Action Handler)
    {
        if (Address == null || Handler == null)
        {
            return default;
        }

        // Owner is carried so the handler runs with this script's Game context, NOT to auto-unbind it.
        EntityScript? Owner = Game.ActiveScript;
        return DelegateBindings.Bind(Address, new VoidInvoker { Handler = Handler, Owner = Owner });
    }
}
