using System;
using System.Buffers;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq.Expressions;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;

namespace LuminaSharp;

[EditorBrowsable(EditorBrowsableState.Never)]
public static class SyncRuntime
{
    // Emitted into a Sync setter after the value actually changed. Native ignores it everywhere but on the host.
    public static void MarkDirty(Lumina.CEntityScript Script)
    {
        Script.World.Net.MarkEntityDirty(Script.Entity);
    }

    // Emitted into a Sync setter that names a Change handler, so the writer's own change runs the handler the same as an arriving one.
    public static void Changed<T>(object Owner, string Member, T Old, T New)
    {
        if (Owner is Lumina.CEntityScript Script && !Script.Entity.IsNull)
        {
            SyncChanges.DispatchLocal(Script, Member, Old, New);
        }
    }

    // A FromOwner write goes to the host only from the owning client, and never while an arriving value is being applied.
    public static bool ShouldForward(Lumina.CEntityScript Script)
    {
        return !Script.Entity.IsNull && Script.World.IsClient && Script.World.Net.IsEntityNetworked(Script.Entity) && Script.World.Net.IsOwner(Script.Entity) && !RpcRuntime.IsReceiving;
    }

    // Native sends the stored value through the field's FProperty, then validates, applies or corrects it on the host, as for C++.
    public static void Forward(Lumina.CEntityScript Script, string Field)
    {
        IntPtr Property = FieldOf(Script, Field);
        if (Property != IntPtr.Zero)
        {
            Lumina.CScriptNetLibrary.ForwardSyncField(Script, Property);
        }
    }

    private static readonly ConditionalWeakTable<Type, Dictionary<string, IntPtr>> Fields = new();

    private static IntPtr FieldOf(Lumina.CEntityScript Script, string Field)
    {
        Type Type = Script.GetType();
        Dictionary<string, IntPtr> ByName = Fields.GetValue(Type, _ => new Dictionary<string, IntPtr>(StringComparer.Ordinal));
        lock (ByName)
        {
            if (!ByName.TryGetValue(Field, out IntPtr Property))
            {
                Property = Lumina.CScriptNetLibrary.FindSyncField(Script, Field);
                if (Property == IntPtr.Zero)
                {
                    Native.Log(ELogLevel.Error, $"[Net] {Type.Name}.{Field} is not a native Sync FromOwner field, so the owner's writes stay local.");
                }
                ByName[Field] = Property;
            }
            return Property;
        }
    }
}

// Runs Change handlers. Built per type on first use and keyed weakly, so a hot reload's old types are not pinned.
internal static class SyncChanges
{
    // Compiled once per field, so an arriving update reads and calls without reflection.
    private sealed class FHandler
    {
        public required Func<Lumina.CEntityScript, object?> Read;
        public required Action<Lumina.CEntityScript, object?, object?> Call;

        // Action<CEntityScript, T, T> for the field's own T, so a local write reaches its handler without boxing.
        public required Delegate TypedCall;
        public required bool bTakesValues;
    }

    private sealed class FTypeHandlers
    {
        public readonly Dictionary<string, FHandler> ByName = new(StringComparer.Ordinal);

        // The C# member name, which is what a generated setter knows when a [Property] renames the native one.
        public readonly Dictionary<string, FHandler> ByMember = new(StringComparer.Ordinal);
    }

    public static void DispatchLocal<T>(Lumina.CEntityScript Script, string Member, T Old, T New)
    {
        FTypeHandlers Handlers = Cache.GetValue(Script.GetType(), Build);
        if (Handlers.ByMember.TryGetValue(Member, out FHandler? Handler))
        {
            ((Action<Lumina.CEntityScript, T, T>)Handler.TypedCall)(Script, Old, New);
        }
    }

    private static readonly ConditionalWeakTable<Type, FTypeHandlers> Cache = new();

    // Per script, since arrivals are captured inside the network system and their handlers run later, at the sync point.
    private static readonly ConditionalWeakTable<Lumina.CEntityScript, List<(FHandler Handler, object? Old)>> Pending = new();

    public static void Dispatch(Lumina.CEntityScript Script, ReadOnlySpan<byte> Names, bool bAfter)
    {
        FTypeHandlers Handlers = Cache.GetValue(Script.GetType(), Build);
        if (Handlers.ByName.Count == 0)
        {
            return;
        }

        if (!bAfter)
        {
            List<(FHandler, object?)> Captured = Pending.GetOrCreateValue(Script);
            lock (Captured)
            {
                CollectNamed(Handlers, Script, Names, Captured);
            }
            return;
        }

        if (!Pending.TryGetValue(Script, out List<(FHandler Handler, object? Old)>? Waiting))
        {
            return;
        }

        // Copied out, since a handler may trigger another replicated update that refills the list.
        int Count;
        (FHandler Handler, object? Old)[] Ready;
        lock (Waiting)
        {
            Count = Waiting.Count;
            if (Count == 0)
            {
                return;
            }
            Ready = ArrayPool<(FHandler, object?)>.Shared.Rent(Count);
            Waiting.CopyTo(Ready);
            Waiting.Clear();
        }
        try
        {
            for (int Index = 0; Index < Count; ++Index)
            {
                (FHandler Handler, object? Old) = Ready[Index];
                object? New = Handler.bTakesValues ? Handler.Read(Script) : null;
                if (Handler.bTakesValues && Equals(Old, New))
                {
                    continue;
                }
                Handler.Call(Script, Old, New);
            }
        }
        finally
        {
            ArrayPool<(FHandler, object?)>.Shared.Return(Ready, clearArray: true);
        }
    }

    // Names arrive as UTF-8 separated by zero bytes, and are matched without making a string of each.
    private static void CollectNamed(FTypeHandlers Handlers, Lumina.CEntityScript Script, ReadOnlySpan<byte> Names, List<(FHandler, object?)> Into)
    {
        Dictionary<string, FHandler>.AlternateLookup<ReadOnlySpan<char>> Lookup = Handlers.ByName.GetAlternateLookup<ReadOnlySpan<char>>();
        Span<char> Scratch = stackalloc char[256];
        while (!Names.IsEmpty)
        {
            int End = Names.IndexOf((byte)0);
            ReadOnlySpan<byte> Name = End < 0 ? Names : Names.Slice(0, End);
            Names = End < 0 ? ReadOnlySpan<byte>.Empty : Names.Slice(End + 1);
            if (Name.IsEmpty || Encoding.UTF8.GetMaxCharCount(Name.Length) > Scratch.Length)
            {
                continue;
            }

            int Length = Encoding.UTF8.GetChars(Name, Scratch);
            // The first capture in a frame holds the value the handler last saw, so a later one for the same field is dropped.
            if (Lookup.TryGetValue(Scratch.Slice(0, Length), out FHandler? Handler) && !Into.Exists(Entry => ReferenceEquals(Entry.Item1, Handler)))
            {
                Into.Add((Handler, Handler.bTakesValues ? Handler.Read(Script) : null));
            }
        }
    }

    private static FTypeHandlers Build(Type Type)
    {
        var Result = new FTypeHandlers();
        const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy;
        foreach (PropertyInfo Property in Type.GetProperties(Flags))
        {
            string? Change = Property.GetCustomAttribute<PropertyAttribute>()?.Change;
            if (Change == null)
            {
                continue;
            }

            MethodInfo? Method = FindHandler(Type, Change, Property.PropertyType, out bool bTakesValues);
            if (Method == null)
            {
                Native.Log(ELogLevel.Warn, $"[Net] {Type.Name}.{Property.Name} names change handler '{Change}', which must take nothing or ({Property.PropertyType.Name} Old, {Property.PropertyType.Name} New).");
                continue;
            }

            FHandler Handler = Compile(Type, Property, Method, bTakesValues);
            Result.ByName[Property.Name] = Handler;
            Result.ByMember[Property.Name] = Handler;
        }
        return Result;
    }

    private static FHandler Compile(Type Type, PropertyInfo Property, MethodInfo Method, bool bTakesValues)
    {
        Type Value = Property.PropertyType;
        ParameterExpression Script = Expression.Parameter(typeof(Lumina.CEntityScript), "Script");
        Expression Typed = Expression.Convert(Script, Type);

        ParameterExpression BoxedOld = Expression.Parameter(typeof(object), "Old");
        ParameterExpression BoxedNew = Expression.Parameter(typeof(object), "New");
        ParameterExpression TypedOld = Expression.Parameter(Value, "Old");
        ParameterExpression TypedNew = Expression.Parameter(Value, "New");

        Expression BoxedCall = bTakesValues
            ? Expression.Call(Typed, Method, Expression.Convert(BoxedOld, Value), Expression.Convert(BoxedNew, Value))
            : Expression.Call(Typed, Method);
        Expression TypedCall = bTakesValues
            ? Expression.Call(Typed, Method, TypedOld, TypedNew)
            : Expression.Call(Typed, Method);

        return new FHandler
        {
            Read = Expression.Lambda<Func<Lumina.CEntityScript, object?>>(
                Expression.Convert(Expression.Property(Typed, Property), typeof(object)), Script).Compile(),
            Call = Expression.Lambda<Action<Lumina.CEntityScript, object?, object?>>(BoxedCall, Script, BoxedOld, BoxedNew).Compile(),
            TypedCall = Expression.Lambda(typeof(Action<,,>).MakeGenericType(typeof(Lumina.CEntityScript), Value, Value),
                TypedCall, Script, TypedOld, TypedNew).Compile(),
            bTakesValues = bTakesValues,
        };
    }

    private static MethodInfo? FindHandler(Type Type, string Name, Type ValueType, out bool bTakesValues)
    {
        const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy;
        bTakesValues = true;
        MethodInfo? WithValues = Type.GetMethod(Name, Flags, null, new[] { ValueType, ValueType }, null);
        if (WithValues != null)
        {
            return WithValues;
        }
        bTakesValues = false;
        return Type.GetMethod(Name, Flags, null, Type.EmptyTypes, null);
    }
}
