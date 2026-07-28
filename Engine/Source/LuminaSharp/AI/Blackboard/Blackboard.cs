using System.Collections.Generic;
using System.Runtime.CompilerServices;
using LuminaSharp;

namespace Lumina;

/// <summary>
/// Hand-written ergonomic half of <c>SBlackboardComponent</c> (the rest is Reflector-generated from the
/// <c>FUNCTION(Script)</c> methods). An entity's blackboard is its named value store: a shared
/// <see cref="CBlackboard"/> asset declares the keys and their defaults, this component holds the live
/// per-entity values. Reach it with <c>Registry.Get&lt;SBlackboardComponent&gt;(Entity)</c>, or from an
/// <c>EntityScript</c> via its <c>Blackboard</c> property.
///
/// Getters are typed after the key's declared type: read a Vector key with <see cref="GetVector(string)"/>,
/// an Entity key with <see cref="GetEntity(string)"/>, and so on. Reading a key with the wrong getter isn't
/// an error, it just reads a slot that was never written. Branch on <see cref="GetKeyType(string)"/> when
/// the type isn't known up front. Game thread only.
/// </summary>
public unsafe partial class SBlackboardComponent
{
    /// <summary>Value of a Float key, or 0 when the schema has no such key.</summary>
    public float GetFloat(string Key) => GetFloat(Key, 0.0f);

    /// <summary>Value of an Int key, or 0 when the schema has no such key.</summary>
    public int GetInt(string Key) => GetInt(Key, 0);

    /// <summary>Value of a Bool key, or false when the schema has no such key.</summary>
    public bool GetBool(string Key) => GetBool(Key, false);

    /// <summary>Value of a Vector key, or zero when the schema has no such key.</summary>
    public FVector3 GetVector(string Key) => GetVector(Key, default);

    /// <summary>Type declared for <paramref name="Key"/>, or <see cref="EBlackboardKeyType.Float"/> when
    /// there's no schema or no such key. Pair with <see cref="HasKey"/> to tell the two apart.</summary>
    public EBlackboardKeyType GetKeyType(string Key) => GetKeyType(Key, EBlackboardKeyType.Float);

    //~ Object keys. Named *ObjectValue natively (the Win32 GetObject macro eats a plain GetObject); these
    //~ are the names scripts should use.

    /// <summary>Point an Object key at <paramref name="Value"/> (null clears it).</summary>
    public void SetObject(string Key, NativeObject? Value) => SetObjectValue(Key, Value!);

    /// <summary>Value of an Object key as the untyped root wrapper, or null.</summary>
    public NativeObject? GetObject(string Key) => GetObjectValue(Key);

    /// <summary>Value of an Object key wrapped as <typeparamref name="T"/>, or null when the key is unset.
    /// The cast is unchecked: the native object must really be a T.</summary>
    public T? GetObject<T>(string Key) where T : NativeObject
    {
        NativeObject? Value = GetObjectValue(Key);
        return Value is null ? null : Wrapper<T>.Create(Value.Handle);
    }

    //~ Enum keys. Stored as their integer value; the schema records which reflected enum a key uses.

    /// <summary>Write an Enum (or Int) key from a C# enum value.</summary>
    public void SetEnum<T>(string Key, T Value) where T : struct, System.Enum
        => SetInt(Key, EnumToInt(Value));

    /// <summary>Read an Enum (or Int) key as a C# enum, falling back to <paramref name="Default"/>.</summary>
    public T GetEnum<T>(string Key, T Default = default) where T : struct, System.Enum
        => IntToEnum<T>(GetInt(Key, EnumToInt(Default)));

    // Enum <-> int without boxing; the native side stores every scalar key as a float, so an enum key
    // round-trips through its integer value whatever its underlying width.
    private static int EnumToInt<T>(T Value) where T : struct, System.Enum
    {
        return Unsafe.SizeOf<T>() switch
        {
            1 => Unsafe.As<T, byte>(ref Value),
            2 => Unsafe.As<T, ushort>(ref Value),
            8 => (int)Unsafe.As<T, long>(ref Value),
            _ => Unsafe.As<T, int>(ref Value),
        };
    }

    private static T IntToEnum<T>(int Value) where T : struct, System.Enum
    {
        T Result = default;
        switch (Unsafe.SizeOf<T>())
        {
            case 1: Unsafe.As<T, byte>(ref Result) = (byte)Value; break;
            case 2: Unsafe.As<T, ushort>(ref Result) = (ushort)Value; break;
            case 8: Unsafe.As<T, long>(ref Result) = Value; break;
            default: Unsafe.As<T, int>(ref Result) = Value; break;
        }
        return Result;
    }
}

/// <summary>
/// Hand-written ergonomic half of the <c>CBlackboard</c> asset wrapper: schema introspection over the
/// generated <c>Keys</c> list. The asset is shared and read-only at runtime; live values live on each
/// entity's <see cref="SBlackboardComponent"/>.
/// </summary>
public unsafe partial class CBlackboard
{
    /// <summary>Number of keys the schema declares.</summary>
    public int KeyCount => NumKeys();

    /// <summary>Type declared for <paramref name="Name"/>, or <see cref="EBlackboardKeyType.Float"/> when
    /// there's no such key.</summary>
    public EBlackboardKeyType GetKeyType(string Name) => GetKeyType(Name, EBlackboardKeyType.Float);

    /// <summary>The key declaration called <paramref name="Name"/>, or null.</summary>
    public FBlackboardKey? FindKey(string Name)
    {
        foreach (FBlackboardKey Key in Keys)
        {
            if (Key.Name == Name)
            {
                return Key;
            }
        }
        return null;
    }

    /// <summary>Every declared key name, in schema order. Built eagerly (the wrapper is an unsafe type, so
    /// it can't hand back a lazy iterator).</summary>
    public List<string> GetKeyNames()
    {
        List<string> Names = new(KeyCount);
        foreach (FBlackboardKey Key in Keys)
        {
            Names.Add(Key.Name);
        }
        return Names;
    }
}
