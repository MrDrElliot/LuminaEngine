using System;

namespace LuminaSharp;

// Live subscription to a script delegate, owned by whoever called Bind: as in C++, nothing detaches it for you, so Unbind (or Dispose) it before the handler's script goes away.
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

    public ScriptDelegate(void* Address)
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

        // Owner is carried so the handler runs with this script's Game context, NOT to auto-unbind it.
        EntityScript? Owner = Game.ActiveScript;
        return DelegateBindings.Bind(Address, new VoidInvoker { Handler = Handler, Owner = Owner });
    }
}

// Transient handle to a multicast event carrying one blittable payload by value.
public readonly unsafe struct ScriptDelegate<T> where T : unmanaged
{
    private readonly void* Address;

    public ScriptDelegate(void* Address)
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
        return DelegateBindings.Bind(Address, new PayloadInvoker<T> { Handler = Handler, Owner = Owner });
    }
}
