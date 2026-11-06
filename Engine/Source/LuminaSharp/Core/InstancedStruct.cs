using System;
using LuminaSharp;

namespace Lumina;

// View over a native FInstancedStruct, which owns a struct value whose type is chosen at runtime.
[global::LuminaSharp.NativeSlotView]
public readonly unsafe struct FInstancedStruct
{
    private readonly nint Address;

    public FInstancedStruct(nint address)
    {
        Address = address;
    }

    // False when the view has no storage, as opposed to holding no value.
    public bool IsBound => Address != 0;

    // Empty when nothing is stored.
    public string TypeName => IsBound ? Native.InstancedStructGetType(Address) : string.Empty;

    public bool IsValid => IsBound && Native.InstancedStructGetMemory(Address) != IntPtr.Zero;

    // True when the stored value is T or derives from it.
    public bool Is<T>() where T : struct, INativeStruct<T>
        => IsBound && Native.InstancedStructIsA(Address, T.NativeName) != 0;

    // A handle on the stored value whose writes edit in place, throwing when it holds something other than a T.
    public T Get<T>() where T : struct, INativeStruct<T> => TryGet<T>() ?? throw NotA(T.NativeName);

    // Null when it holds something other than a T, so a write reads as Instanced.TryGet<T>()?.Field = Value.
    public T? TryGet<T>() where T : struct, INativeStruct<T>
    {
        return Is<T>() ? NativeStructs.FromAddressOrNull<T>(Native.InstancedStructGetMemory(Address)) : null;
    }

    // The stored script struct seen through an interface it implements, throwing when it implements none such.
    public TInterface As<TInterface>() where TInterface : class
        => TryAs<TInterface>() ?? throw NotA(typeof(TInterface).Name);

    // Null when nothing is stored or the stored struct does not implement TInterface.
    public TInterface? TryAs<TInterface>() where TInterface : class
    {
        if (!IsBound)
        {
            return null;
        }
        nint Memory = Native.InstancedStructGetMemory(Address);
        return Memory == 0 ? null : Host.Library?.ViewScriptStruct(TypeName, Memory) as TInterface;
    }

    private InvalidOperationException NotA(string Expected)
    {
        string Held = TypeName;
        return new InvalidOperationException($"Instanced struct holds {(Held.Length == 0 ? "nothing" : Held)}, not a {Expected}.");
    }

    public T InitializeAs<T>() where T : struct, INativeStruct<T>
    {
        if (IsBound)
        {
            Native.InstancedStructInitializeAs(Address, T.NativeName);
        }
        return Get<T>();
    }

    public void InitializeAs(string StructName)
    {
        if (IsBound)
        {
            Native.InstancedStructInitializeAs(Address, StructName ?? string.Empty);
        }
    }

    public void Reset()
    {
        if (IsBound)
        {
            Native.InstancedStructReset(Address);
        }
    }
}

// The stored value always derives from TBase.
public readonly unsafe struct TInstancedStruct<TBase> where TBase : struct, INativeStruct<TBase>
{
    private readonly FInstancedStruct Inner;

    public TInstancedStruct(nint address)
    {
        Inner = new FInstancedStruct(address);
    }

    public bool IsBound => Inner.IsBound;

    public bool IsValid => Inner.IsValid;

    public string TypeName => Inner.TypeName;

    public TBase Get() => Inner.Get<TBase>();

    public TInterface As<TInterface>() where TInterface : class => Inner.As<TInterface>();

    public TInterface? TryAs<TInterface>() where TInterface : class => Inner.TryAs<TInterface>();

    public TBase? TryGet() => Inner.TryGet<TBase>();

    public T Get<T>() where T : struct, INativeStruct<T> => Inner.Get<T>();

    public T? TryGet<T>() where T : struct, INativeStruct<T> => Inner.TryGet<T>();

    public bool Is<T>() where T : struct, INativeStruct<T> => Inner.Is<T>();

    public T InitializeAs<T>() where T : struct, INativeStruct<T> => Inner.InitializeAs<T>();

    public void InitializeAs(string StructName) => Inner.InitializeAs(StructName);

    public void Reset() => Inner.Reset();

    public FInstancedStruct AsUntyped() => Inner;
}
