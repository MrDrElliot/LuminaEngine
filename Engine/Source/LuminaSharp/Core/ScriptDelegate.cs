using System;

namespace LuminaSharp;

// Live subscription to a script delegate; dispose to detach the handler.
public struct DelegateBinding : IDisposable
{
    private ulong Id;

    internal DelegateBinding(ulong Id)
    {
        this.Id = Id;
    }

    public bool IsValid => Id != 0;

    public void Unbind()
    {
        if (Id != 0)
        {
            DelegateBindings.Unbind(Id);
            Id = 0;
        }
    }

    public void Dispose() => Unbind();
}

// Transient handle to a no-payload multicast event; do not store it, re-fetch the accessor.
public readonly unsafe struct ScriptDelegate
{
    private readonly void* Address;

    internal ScriptDelegate(void* Address)
    {
        this.Address = Address;
    }

    public bool IsValid => Address != null;

    public DelegateBinding Bind(Action Handler)
    {
        if (Address == null || Handler == null)
        {
            return default;
        }

        EntityScript? Owner = Game.ActiveScript;
        DelegateBinding Binding = DelegateBindings.Bind(Address, new VoidInvoker { Handler = Handler, Owner = Owner });
        Owner?.TrackBinding(Binding);
        return Binding;
    }
}

// Transient handle to a multicast event carrying one blittable payload by value.
public readonly unsafe struct ScriptDelegate<T> where T : unmanaged
{
    private readonly void* Address;

    internal ScriptDelegate(void* Address)
    {
        this.Address = Address;
    }

    public bool IsValid => Address != null;

    public DelegateBinding Bind(Action<T> Handler)
    {
        if (Address == null || Handler == null)
        {
            return default;
        }

        EntityScript? Owner = Game.ActiveScript;
        DelegateBinding Binding = DelegateBindings.Bind(Address, new PayloadInvoker<T> { Handler = Handler, Owner = Owner });
        Owner?.TrackBinding(Binding);
        return Binding;
    }
}
