using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

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

    // A blittable slot is the argument's own bytes, so reading it as T skips the box FrameMarshal.Read returns.
    // IsReferenceOrContainsReferences is folded by the JIT, so the branch costs nothing per instantiation.
    internal static unsafe T Read<T>(IntPtr Payload, in FrameMarshal.FSlot Slot)
    {
        if (Slot.Access == EElementKind.Blittable && !RuntimeHelpers.IsReferenceOrContainsReferences<T>())
        {
            return Unsafe.ReadUnaligned<T>((void*)((nint)Payload + Slot.Offset));
        }
        return (T)FrameMarshal.Read(Payload, Slot)!;
    }

    // The first argument of a pack sits at offset zero, which is what a view with no property token reads.
    internal static unsafe T ReadUntyped<T>(IntPtr Payload) => Unsafe.ReadUnaligned<T>((void*)Payload);

    /** Null slots when the view carries no property and T is blittable, which the arity-one fast path allows. */
    internal static FrameMarshal.FSlot[]? ResolveOrPack<T>(IntPtr Property, string Where)
    {
        if (Property != IntPtr.Zero)
        {
            return Resolve(Property, new[] { typeof(T) }, Where);
        }
        if (RuntimeHelpers.IsReferenceOrContainsReferences<T>())
        {
            Debug.LogError($"{Where}: {typeof(T).Name} has to be marshalled but this view carries no delegate property.");
            return null;
        }
        return Array.Empty<FrameMarshal.FSlot>();
    }
}

internal sealed class ArgsInvoker1<T1> : IDelegateInvoker
{
    public Action<T1> Handler = null!;
    public FrameMarshal.FSlot[] Slots = null!;
    public EntityScript? Owner;

    public void Invoke(IntPtr Payload)
    {
        T1 A0 = Slots.Length == 0
            ? ScriptDelegateArgs.ReadUntyped<T1>(Payload)
            : ScriptDelegateArgs.Read<T1>(Payload, Slots[0]);

        if (Owner is { } Script)
        {
            using (Game.Push(Script.World, Script.Entity))
            {
                Handler(A0);
            }
        }
        else
        {
            Handler(A0);
        }
    }
}

internal sealed class ArgsInvoker2<T1, T2> : IDelegateInvoker
{
    public Action<T1, T2> Handler = null!;
    public FrameMarshal.FSlot[] Slots = null!;
    public EntityScript? Owner;

    public void Invoke(IntPtr Payload)
    {
        T1 A0 = ScriptDelegateArgs.Read<T1>(Payload, Slots[0]);
        T2 A1 = ScriptDelegateArgs.Read<T2>(Payload, Slots[1]);

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
        T1 A0 = ScriptDelegateArgs.Read<T1>(Payload, Slots[0]);
        T2 A1 = ScriptDelegateArgs.Read<T2>(Payload, Slots[1]);
        T3 A2 = ScriptDelegateArgs.Read<T3>(Payload, Slots[2]);

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
        T1 A0 = ScriptDelegateArgs.Read<T1>(Payload, Slots[0]);
        T2 A1 = ScriptDelegateArgs.Read<T2>(Payload, Slots[1]);
        T3 A2 = ScriptDelegateArgs.Read<T3>(Payload, Slots[2]);
        T4 A3 = ScriptDelegateArgs.Read<T4>(Payload, Slots[3]);

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

internal sealed class ArgsInvoker5<T1, T2, T3, T4, T5> : IDelegateInvoker
{
    public Action<T1, T2, T3, T4, T5> Handler = null!;
    public FrameMarshal.FSlot[] Slots = null!;
    public EntityScript? Owner;

    public void Invoke(IntPtr Payload)
    {
        T1 A0 = ScriptDelegateArgs.Read<T1>(Payload, Slots[0]);
        T2 A1 = ScriptDelegateArgs.Read<T2>(Payload, Slots[1]);
        T3 A2 = ScriptDelegateArgs.Read<T3>(Payload, Slots[2]);
        T4 A3 = ScriptDelegateArgs.Read<T4>(Payload, Slots[3]);
        T5 A4 = ScriptDelegateArgs.Read<T5>(Payload, Slots[4]);

        if (Owner is { } Script)
        {
            using (Game.Push(Script.World, Script.Entity))
            {
                Handler(A0, A1, A2, A3, A4);
            }
        }
        else
        {
            Handler(A0, A1, A2, A3, A4);
        }
    }
}

internal sealed class ArgsInvoker6<T1, T2, T3, T4, T5, T6> : IDelegateInvoker
{
    public Action<T1, T2, T3, T4, T5, T6> Handler = null!;
    public FrameMarshal.FSlot[] Slots = null!;
    public EntityScript? Owner;

    public void Invoke(IntPtr Payload)
    {
        T1 A0 = ScriptDelegateArgs.Read<T1>(Payload, Slots[0]);
        T2 A1 = ScriptDelegateArgs.Read<T2>(Payload, Slots[1]);
        T3 A2 = ScriptDelegateArgs.Read<T3>(Payload, Slots[2]);
        T4 A3 = ScriptDelegateArgs.Read<T4>(Payload, Slots[3]);
        T5 A4 = ScriptDelegateArgs.Read<T5>(Payload, Slots[4]);
        T6 A5 = ScriptDelegateArgs.Read<T6>(Payload, Slots[5]);

        if (Owner is { } Script)
        {
            using (Game.Push(Script.World, Script.Entity))
            {
                Handler(A0, A1, A2, A3, A4, A5);
            }
        }
        else
        {
            Handler(A0, A1, A2, A3, A4, A5);
        }
    }
}

/** Transient handle to a multicast event carrying one reflected argument. */
[NativeSlotView]
public readonly unsafe struct ScriptDelegate<T1>
{
    private readonly void* Address;
    private readonly IntPtr Property;

    public ScriptDelegate(void* Address, IntPtr Property)
    {
        this.Address = Address;
        this.Property = Property;
    }

    public ScriptDelegate(void* Address)
    {
        this.Address = Address;
        this.Property = IntPtr.Zero;
    }

    // The slot address as an integer, which is the shape a binder building this view by reflection has.
    public ScriptDelegate(nint Address)
    {
        this.Address = (void*)Address;
        this.Property = IntPtr.Zero;
    }

    public bool IsValid => Address != null;

    public DelegateBinding Bind(Action<T1> Handler)
    {
        if (Address == null || Handler == null)
        {
            return default;
        }

        FrameMarshal.FSlot[]? Slots = ScriptDelegateArgs.ResolveOrPack<T1>(Property, "ScriptDelegate<T1>.Bind");
        if (Slots == null)
        {
            return default;
        }

        EntityScript? Owner = Game.ActiveScript;
        return DelegateBindings.Bind(Address, new ArgsInvoker1<T1> { Handler = Handler, Slots = Slots, Owner = Owner });
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

/** Transient handle to a multicast event carrying five reflected arguments. */
[NativeSlotView]
public readonly unsafe struct ScriptDelegate<T1, T2, T3, T4, T5>
{
    private readonly void* Address;
    private readonly IntPtr Property;

    public ScriptDelegate(void* Address, IntPtr Property)
    {
        this.Address = Address;
        this.Property = Property;
    }

    public bool IsValid => Address != null;

    public DelegateBinding Bind(Action<T1, T2, T3, T4, T5> Handler)
    {
        if (Address == null || Handler == null)
        {
            return default;
        }

        FrameMarshal.FSlot[]? Slots = ScriptDelegateArgs.Resolve(
            Property, new[] { typeof(T1), typeof(T2), typeof(T3), typeof(T4), typeof(T5) }, "ScriptDelegate<T1,T2,T3,T4,T5>.Bind");
        if (Slots == null)
        {
            return default;
        }

        EntityScript? Owner = Game.ActiveScript;
        return DelegateBindings.Bind(Address, new ArgsInvoker5<T1, T2, T3, T4, T5> { Handler = Handler, Slots = Slots, Owner = Owner });
    }
}

/** Transient handle to a multicast event carrying six reflected arguments. */
[NativeSlotView]
public readonly unsafe struct ScriptDelegate<T1, T2, T3, T4, T5, T6>
{
    private readonly void* Address;
    private readonly IntPtr Property;

    public ScriptDelegate(void* Address, IntPtr Property)
    {
        this.Address = Address;
        this.Property = Property;
    }

    public bool IsValid => Address != null;

    public DelegateBinding Bind(Action<T1, T2, T3, T4, T5, T6> Handler)
    {
        if (Address == null || Handler == null)
        {
            return default;
        }

        FrameMarshal.FSlot[]? Slots = ScriptDelegateArgs.Resolve(
            Property, new[] { typeof(T1), typeof(T2), typeof(T3), typeof(T4), typeof(T5), typeof(T6) }, "ScriptDelegate<T1,T2,T3,T4,T5,T6>.Bind");
        if (Slots == null)
        {
            return default;
        }

        EntityScript? Owner = Game.ActiveScript;
        return DelegateBindings.Bind(Address, new ArgsInvoker6<T1, T2, T3, T4, T5, T6> { Handler = Handler, Slots = Slots, Owner = Owner });
    }
}
