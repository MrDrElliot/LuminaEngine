using System;
using System.Reflection;

namespace LuminaSharp;

/// <summary>Carries a native reflected type's simple name so the mirror can be classified and resolved.</summary>
[AttributeUsage(AttributeTargets.Struct | AttributeTargets.Class, Inherited = false)]
public sealed class NativeTypeAttribute : Attribute
{
    public string Name { get; }

    public NativeTypeAttribute(string Name)
    {
        this.Name = Name;
    }
}

// The one rule for the native name of any C# type, so minting, lookups and accessors can never disagree.
internal static class NativeTypeName
{
    public static string Of<T>() => Cache<T>.Name;

    // A wrapper names its native type, a data struct keeps the short name its assets store, and a script class its full name.
    public static string Of(Type Type)
    {
        if (Type.GetCustomAttribute<NativeTypeAttribute>(inherit: false) is { } Native)
        {
            return Native.Name;
        }
        if (Type.GetCustomAttribute<ScriptStructBaseAttribute>(inherit: false) != null)
        {
            return Type.Name;
        }
        return Type.FullName ?? Type.Name;
    }

    private static class Cache<T>
    {
        public static readonly string Name = Of(typeof(T));
    }
}
