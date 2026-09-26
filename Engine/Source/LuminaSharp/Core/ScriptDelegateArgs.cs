using System;
using System.Collections.Generic;

namespace LuminaSharp;

/** Argument slots for one delegate signature, resolved once and reused by every binding. */
internal static class ScriptDelegateArgs
{
    private sealed class FKey : IEquatable<FKey>
    {
        internal IntPtr Property;
        internal Type[] Declared = Array.Empty<Type>();

        public bool Equals(FKey? Other)
        {
            if (Other is null || Other.Property != Property || Other.Declared.Length != Declared.Length)
            {
                return false;
            }
            for (int Index = 0; Index < Declared.Length; ++Index)
            {
                if (Declared[Index] != Other.Declared[Index])
                {
                    return false;
                }
            }
            return true;
        }

        public override bool Equals(object? Other) => Equals(Other as FKey);

        public override int GetHashCode()
        {
            HashCode Hash = new HashCode();
            Hash.Add(Property);
            foreach (Type T in Declared)
            {
                Hash.Add(T);
            }
            return Hash.ToHashCode();
        }
    }

    private static readonly Dictionary<FKey, FrameMarshal.FSlot[]?> Cache = new();

    /** Null when an argument cannot be bound, so the binding is dropped rather than invoked wrongly. */
    internal static FrameMarshal.FSlot[]? Resolve(IntPtr DelegateProperty, Type[] Declared, string Where)
    {
        if (DelegateProperty == IntPtr.Zero)
        {
            Debug.LogError($"{Where}: the delegate property could not be resolved.");
            return null;
        }

        FKey Key = new FKey { Property = DelegateProperty, Declared = Declared };
        if (Cache.TryGetValue(Key, out FrameMarshal.FSlot[]? Cached))
        {
            return Cached;
        }

        FrameMarshal.FSlot[]? Slots = Bind(DelegateProperty, Declared, Where);
        Cache[Key] = Slots;
        return Slots;
    }

    private static FrameMarshal.FSlot[]? Bind(IntPtr DelegateProperty, Type[] Declared, string Where)
    {
        int NativeCount = Native.DelegateArgCount(DelegateProperty);
        if (NativeCount != Declared.Length)
        {
            Debug.LogError($"{Where}: the delegate takes {NativeCount} arguments but the handler declares {Declared.Length}.");
            return null;
        }

        FrameMarshal.FSlot[] Slots = new FrameMarshal.FSlot[Declared.Length];
        for (int Index = 0; Index < Declared.Length; ++Index)
        {
            IntPtr ArgProperty = Native.DelegateArgProperty(DelegateProperty, Index);
            if (!FrameMarshal.TryBind(ArgProperty, Declared[Index], $"{Where} argument {Index}", out Slots[Index]))
            {
                return null;
            }
        }
        return Slots;
    }

    internal static void PurgeAll() => Cache.Clear();
}

internal sealed class ArgsInvoker2<T1, T2> : IDelegateInvoker
{
    public Action<T1, T2> Handler = null!;
    public FrameMarshal.FSlot[] Slots = null!;
    public EntityScript? Owner;

    public void Invoke(IntPtr Payload)
    {
        T1 A0 = (T1)FrameMarshal.Read(Payload, Slots[0])!;
        T2 A1 = (T2)FrameMarshal.Read(Payload, Slots[1])!;

        if (Owner is { } Script)
        {
            using (Game.Push(Script.World, Script.Entity))
            {
                Handler(A0, A1);
            }
        }
        else
        {
            Handler(A0, A1);
        }
    }
}

internal sealed class ArgsInvoker3<T1, T2, T3> : IDelegateInvoker
{
    public Action<T1, T2, T3> Handler = null!;
    public FrameMarshal.FSlot[] Slots = null!;
    public EntityScript? Owner;

    public void Invoke(IntPtr Payload)
    {
        T1 A0 = (T1)FrameMarshal.Read(Payload, Slots[0])!;
        T2 A1 = (T2)FrameMarshal.Read(Payload, Slots[1])!;
        T3 A2 = (T3)FrameMarshal.Read(Payload, Slots[2])!;

        if (Owner is { } Script)
        {
            using (Game.Push(Script.World, Script.Entity))
            {
                Handler(A0, A1, A2);
            }
        }
        else
        {
            Handler(A0, A1, A2);
        }
    }
}

internal sealed class ArgsInvoker4<T1, T2, T3, T4> : IDelegateInvoker
{
    public Action<T1, T2, T3, T4> Handler = null!;
    public FrameMarshal.FSlot[] Slots = null!;
    public EntityScript? Owner;

    public void Invoke(IntPtr Payload)
    {
        T1 A0 = (T1)FrameMarshal.Read(Payload, Slots[0])!;
        T2 A1 = (T2)FrameMarshal.Read(Payload, Slots[1])!;
        T3 A2 = (T3)FrameMarshal.Read(Payload, Slots[2])!;
        T4 A3 = (T4)FrameMarshal.Read(Payload, Slots[3])!;

        if (Owner is { } Script)
        {
            using (Game.Push(Script.World, Script.Entity))
            {
                Handler(A0, A1, A2, A3);
            }
        }
        else
        {
            Handler(A0, A1, A2, A3);
        }
    }
}

/** Transient handle to a multicast event carrying two reflected arguments. */
[NativeSlotView]
public readonly unsafe struct ScriptDelegate<T1, T2>
{
    private readonly void* Address;
    private readonly IntPtr Property;

    public ScriptDelegate(void* Address, IntPtr Property)
    {
        this.Address = Address;
        this.Property = Property;
    }

    public bool IsValid => Address != null;

    public DelegateBinding Bind(Action<T1, T2> Handler)
    {
        if (Address == null || Handler == null)
        {
            return default;
        }

        FrameMarshal.FSlot[]? Slots = ScriptDelegateArgs.Resolve(
            Property, new[] { typeof(T1), typeof(T2) }, "ScriptDelegate<T1,T2>.Bind");
        if (Slots == null)
        {
            return default;
        }

        EntityScript? Owner = Game.ActiveScript;
        return DelegateBindings.Bind(Address, new ArgsInvoker2<T1, T2> { Handler = Handler, Slots = Slots, Owner = Owner });
    }
}

/** Transient handle to a multicast event carrying three reflected arguments. */
[NativeSlotView]
public readonly unsafe struct ScriptDelegate<T1, T2, T3>
{
    private readonly void* Address;
    private readonly IntPtr Property;

    public ScriptDelegate(void* Address, IntPtr Property)
    {
        this.Address = Address;
        this.Property = Property;
    }

    public bool IsValid => Address != null;

    public DelegateBinding Bind(Action<T1, T2, T3> Handler)
    {
        if (Address == null || Handler == null)
        {
            return default;
        }

        FrameMarshal.FSlot[]? Slots = ScriptDelegateArgs.Resolve(
            Property, new[] { typeof(T1), typeof(T2), typeof(T3) }, "ScriptDelegate<T1,T2,T3>.Bind");
        if (Slots == null)
        {
            return default;
        }

        EntityScript? Owner = Game.ActiveScript;
        return DelegateBindings.Bind(Address, new ArgsInvoker3<T1, T2, T3> { Handler = Handler, Slots = Slots, Owner = Owner });
    }
}

/** Transient handle to a multicast event carrying four reflected arguments. */
[NativeSlotView]
public readonly unsafe struct ScriptDelegate<T1, T2, T3, T4>
{
    private readonly void* Address;
    private readonly IntPtr Property;

    public ScriptDelegate(void* Address, IntPtr Property)
    {
        this.Address = Address;
        this.Property = Property;
    }

    public bool IsValid => Address != null;

    public DelegateBinding Bind(Action<T1, T2, T3, T4> Handler)
    {
        if (Address == null || Handler == null)
        {
            return default;
        }

        FrameMarshal.FSlot[]? Slots = ScriptDelegateArgs.Resolve(
            Property, new[] { typeof(T1), typeof(T2), typeof(T3), typeof(T4) }, "ScriptDelegate<T1,T2,T3,T4>.Bind");
        if (Slots == null)
        {
            return default;
        }

        EntityScript? Owner = Game.ActiveScript;
        return DelegateBindings.Bind(Address, new ArgsInvoker4<T1, T2, T3, T4> { Handler = Handler, Slots = Slots, Owner = Owner });
    }
}
