using System;
using LuminaSharp;

namespace Lumina;

/// A view over a native FInstancedStruct, which owns a reflected struct value whose type is chosen at runtime.
public readonly unsafe struct FInstancedStruct
{
    private readonly nint Address;

    public FInstancedStruct(nint address)
    {
        Address = address;
    }

    /// False when the view has no storage, as opposed to holding no value.
    public bool IsBound => Address != 0;

    /// The stored struct's registered name, empty when nothing is stored.
    public string TypeName => IsBound ? Native.InstancedStructGetType(Address) : string.Empty;

    public bool IsValid => IsBound && Native.InstancedStructGetMemory(Address) != IntPtr.Zero;

    /// True when the stored value is T or derives from it.
    public bool Is<T>() where T : NativeStruct
        => IsBound && Native.InstancedStructIsA(Address, NativeTypeName.Of<T>()) != 0;

    /// A wrapper over the stored value, or null when it is not a T. Writes through it edit in place.
    public T? Get<T>() where T : NativeStruct
    {
        if (!Is<T>())
        {
            return null;
        }
        IntPtr Memory = Native.InstancedStructGetMemory(Address);
        return Memory == IntPtr.Zero ? null : Wrapper<T>.Create(Memory);
    }

    /// Replaces the value with a fresh default-constructed T and returns the view of it.
    public T? InitializeAs<T>() where T : NativeStruct
    {
        if (!IsBound)
        {
            return null;
        }
        Native.InstancedStructInitializeAs(Address, NativeTypeName.Of<T>());
        return Get<T>();
    }

    /// Replaces the value with a fresh default-constructed instance of a runtime-named struct.
    public void InitializeAs(string StructName)
    {
        if (IsBound)
        {
            Native.InstancedStructInitializeAs(Address, StructName ?? string.Empty);
        }
    }

    /// Destroys the stored value, leaving the slot empty.
    public void Reset()
    {
        if (IsBound)
        {
            Native.InstancedStructReset(Address);
        }
    }
}

/// A view over a native TInstancedStruct, whose stored value always derives from TBase.
public readonly unsafe struct TInstancedStruct<TBase> where TBase : NativeStruct
{
    private readonly FInstancedStruct Inner;

    public TInstancedStruct(nint address)
    {
        Inner = new FInstancedStruct(address);
    }

    public bool IsBound => Inner.IsBound;

    public bool IsValid => Inner.IsValid;

    public string TypeName => Inner.TypeName;

    /// The stored value viewed as the base type, or null when the slot is empty.
    public TBase? Get() => Inner.Get<TBase>();

    /// The stored value viewed as a derived type, or null when it is something else.
    public T? Get<T>() where T : NativeStruct => Inner.Get<T>();

    public bool Is<T>() where T : NativeStruct => Inner.Is<T>();

    public T? InitializeAs<T>() where T : NativeStruct => Inner.InitializeAs<T>();

    public void InitializeAs(string StructName) => Inner.InitializeAs(StructName);

    public void Reset() => Inner.Reset();

    /// Drops the compile-time base, for code that handles any instanced struct.
    public FInstancedStruct AsUntyped() => Inner;
}
