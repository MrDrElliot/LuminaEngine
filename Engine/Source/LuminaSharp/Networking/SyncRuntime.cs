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

            string NativeName = Property.GetCustomAttribute<PropertyAttribute>()?.Name ?? Property.Name;
            Result.ByName[NativeName] = new FHandler { Property = Property, Method = Method, bTakesValues = bTakesValues };
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
