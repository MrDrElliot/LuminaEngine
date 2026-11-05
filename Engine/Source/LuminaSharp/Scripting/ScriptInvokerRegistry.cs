using System;
using System.Collections.Generic;

namespace LuminaSharp;

// Where those generated invokers register themselves, keyed by declaring type and function name.
public static class ScriptInvokerRegistry
{
    private static readonly Dictionary<(Type Owner, string Function), nint> Invokers = new();

    // A managed function pointer, kept as nint so the registry needs no delegate object per binding.
    public static void Register(Type Owner, string Function, nint Invoker)
    {
        lock (Invokers)
        {
            Invokers[(Owner, Function)] = Invoker;
        }
    }

    internal static nint Find(Type Owner, string Function)
    {
        lock (Invokers)
        {
            return Invokers.TryGetValue((Owner, Function), out nint Found) ? Found : 0;
        }
    }

    // Only script types go, since the engine assembly registered its own once at load and never will again.
    internal static void Clear()
    {
        lock (Invokers)
        {
            ClearCollectible();
        }
    }

    private static void ClearCollectible()
    {
        var Unloading = new List<(Type, string)>();
        foreach ((Type Owner, string Function) Key in Invokers.Keys)
        {
            if (Key.Owner.Assembly.IsCollectible)
            {
                Unloading.Add(Key);
            }
        }
        foreach ((Type, string) Key in Unloading)
        {
            Invokers.Remove(Key);
        }
    }
}
