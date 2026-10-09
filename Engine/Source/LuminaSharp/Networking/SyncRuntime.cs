using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;

namespace LuminaSharp;

[EditorBrowsable(EditorBrowsableState.Never)]
public static class SyncRuntime
{
    // Emitted into a [Sync] setter after the value actually changed. Native ignores it everywhere but on the host.
    public static void MarkDirty(IntPtr Script)
    {
        NetNative.MarkScriptDirty(Script);
    }

    // Emitted into a [Sync] setter carrying [Change], so the writer's own change runs the handler the same as an arriving one.
    public static void Changed<T>(object Owner, string Member, T Old, T New)
    {
        if (Owner is EntityScript Script && !Script.Entity.IsNull)
        {
            SyncChanges.DispatchLocal(Script, Member, Old, New);
        }
    }

    // A FromOwner write goes to the host only from the owning client, and never while an arriving value is being applied.
    public static bool ShouldForward(EntityScript Script)
    {
        return !Script.Entity.IsNull && Script.World.IsClient && Script.Network.IsNetworked && Script.Network.IsOwner && !RpcRuntime.IsReceiving;
    }

    // Native sends the stored value through the field's FProperty, then validates, applies or corrects it on the host, as for C++.
    public static void Forward(EntityScript Script, string Field)
    {
        IntPtr Handle = NativeObjectMarshal.ToHandle(Script);
        IntPtr Property = FieldOf(Script.GetType(), Handle, Field);
        if (Property != IntPtr.Zero)
        {
            NetNative.SendSync(Handle, Property);
        }
    }

    private static readonly ConditionalWeakTable<Type, Dictionary<string, IntPtr>> Fields = new();

    private static IntPtr FieldOf(Type Type, IntPtr Handle, string Field)
    {
        Dictionary<string, IntPtr> ByName = Fields.GetValue(Type, _ => new Dictionary<string, IntPtr>(StringComparer.Ordinal));
        lock (ByName)
        {
            if (!ByName.TryGetValue(Field, out IntPtr Property))
            {
                Property = NetNative.FindSyncField(Handle, Field);
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

// Runs [Change] handlers. Built per type on first use and keyed weakly, so a hot reload's old types are not pinned.
internal static class SyncChanges
{
    private sealed class FHandler
    {
        public required PropertyInfo Property;
        public required MethodInfo Method;
        public required bool bTakesValues;
    }

    private sealed class FTypeHandlers
    {
        public readonly Dictionary<string, FHandler> ByName = new(StringComparer.Ordinal);

        // The C# member name, which is what a generated setter knows when a [Property] renames the native one.
        public readonly Dictionary<string, FHandler> ByMember = new(StringComparer.Ordinal);
    }

    public static void DispatchLocal<T>(EntityScript Script, string Member, T Old, T New)
    {
        FTypeHandlers Handlers = Cache.GetValue(Script.GetType(), Build);
        if (!Handlers.ByMember.TryGetValue(Member, out FHandler? Handler))
        {
            return;
        }
        Handler.Method.Invoke(Script, Handler.bTakesValues ? new object?[] { Old, New } : null);
    }

    private static readonly ConditionalWeakTable<Type, FTypeHandlers> Cache = new();

    [ThreadStatic] private static List<(FHandler Handler, object? Old)>? Pending;

    public static void Dispatch(EntityScript Script, ReadOnlySpan<byte> Names, bool bAfter)
    {
        FTypeHandlers Handlers = Cache.GetValue(Script.GetType(), Build);
        if (Handlers.ByName.Count == 0)
        {
            return;
        }

        if (!bAfter)
        {
            Pending ??= new List<(FHandler, object?)>();
            Pending.Clear();
            foreach (string Name in SplitNames(Names))
            {
                if (Handlers.ByName.TryGetValue(Name, out FHandler? Handler))
                {
                    Pending.Add((Handler, Handler.bTakesValues ? Handler.Property.GetValue(Script) : null));
                }
            }
            return;
        }

        if (Pending == null || Pending.Count == 0)
        {
            return;
        }

        // Copied out, since a handler may trigger another replicated update that refills the list.
        var Ready = Pending.ToArray();
        Pending.Clear();
        foreach ((FHandler Handler, object? Old) in Ready)
        {
            object? New = Handler.bTakesValues ? Handler.Property.GetValue(Script) : null;
            if (Handler.bTakesValues && Equals(Old, New))
            {
                continue;
            }
            Handler.Method.Invoke(Script, Handler.bTakesValues ? new[] { Old, New } : null);
        }
    }

    private static IEnumerable<string> SplitNames(ReadOnlySpan<byte> Names)
    {
        var Result = new List<string>();
        int Start = 0;
        for (int Index = 0; Index < Names.Length; ++Index)
        {
            if (Names[Index] == 0)
            {
                if (Index > Start)
                {
                    Result.Add(Encoding.UTF8.GetString(Names.Slice(Start, Index - Start)));
                }
                Start = Index + 1;
            }
        }
        return Result;
    }

    private static FTypeHandlers Build(Type Type)
    {
        var Result = new FTypeHandlers();
        const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy;
        foreach (PropertyInfo Property in Type.GetProperties(Flags))
        {
            ChangeAttribute? Change = Property.GetCustomAttribute<ChangeAttribute>();
            if (Change == null)
            {
                continue;
            }

            MethodInfo? Method = FindHandler(Type, Change.Method, Property.PropertyType, out bool bTakesValues);
            if (Method == null)
            {
                Native.Log(ELogLevel.Warn, $"[Net] {Type.Name}.{Property.Name} names change handler '{Change.Method}', which must take nothing or ({Property.PropertyType.Name} Old, {Property.PropertyType.Name} New).");
                continue;
            }

            string NativeName = Property.Name;
            var Handler = new FHandler { Property = Property, Method = Method, bTakesValues = bTakesValues };
            Result.ByName[NativeName] = Handler;
            Result.ByMember[Property.Name] = Handler;
        }
        return Result;
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
