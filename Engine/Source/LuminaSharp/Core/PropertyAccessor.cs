using System;
using System.Collections.Concurrent;
using System.Reflection;

namespace LuminaSharp;

/// Cached open-instance property get/set delegates; far cheaper than repeated PropertyInfo.GetValue/SetValue.
public static class PropertyAccessor
{
    private static readonly ConcurrentDictionary<PropertyInfo, Func<object, object?>> Getters = new();
    private static readonly ConcurrentDictionary<PropertyInfo, Action<object, object?>?> Setters = new();

    /// A cached getter; throws if the property has no getter.
    public static Func<object, object?> Getter(PropertyInfo Property)
    {
        return Getters.GetOrAdd(Property, static P =>
        {
            MethodInfo? Get = P.GetGetMethod(true);
            if (Get == null)
            {
                throw new ArgumentException($"Property '{P.DeclaringType?.Name}.{P.Name}' has no getter.");
            }
            MethodInfo Builder = typeof(PropertyAccessor)
                .GetMethod(nameof(BuildGetter), BindingFlags.NonPublic | BindingFlags.Static)!
                .MakeGenericMethod(P.DeclaringType!, P.PropertyType);
            return (Func<object, object?>)Builder.Invoke(null, new object[] { Get })!;
        });
    }

    /// A cached setter, or null if the property is read-only.
    public static Action<object, object?>? Setter(PropertyInfo Property)
    {
        return Setters.GetOrAdd(Property, static P =>
        {
            MethodInfo? Set = P.GetSetMethod(true);
            if (Set == null)
            {
                return null;
            }
            MethodInfo Builder = typeof(PropertyAccessor)
                .GetMethod(nameof(BuildSetter), BindingFlags.NonPublic | BindingFlags.Static)!
                .MakeGenericMethod(P.DeclaringType!, P.PropertyType);
            return (Action<object, object?>?)Builder.Invoke(null, new object[] { Set });
        });
    }

    /// Drops every cached accessor. Called on a script ALC unload: the cache keys (PropertyInfo) and the
    /// closed-generic delegate values reference user types in the collectible ALC, so leaving them would
    /// pin that generation (and accumulate one entry set per reload). The next generation rebuilds lazily.
    public static void ClearScriptCaches()
    {
        Getters.Clear();
        Setters.Clear();
    }

    private static Func<object, object?> BuildGetter<TTarget, TValue>(MethodInfo Get)
    {
        var Typed = (Func<TTarget, TValue>)Delegate.CreateDelegate(typeof(Func<TTarget, TValue>), Get);
        return Target => Typed((TTarget)Target);
    }

    private static Action<object, object?> BuildSetter<TTarget, TValue>(MethodInfo Set)
    {
        var Typed = (Action<TTarget, TValue>)Delegate.CreateDelegate(typeof(Action<TTarget, TValue>), Set);
        return (Target, Value) => Typed((TTarget)Target, (TValue)Value!);
    }
}
