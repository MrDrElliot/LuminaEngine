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

/// The native CStruct/CClass name a wrapper stands for, cached per type; falls back to the C# type name.
internal static class NativeTypeName
{
    public static string Of<T>() => Cache<T>.Name;

    private static class Cache<T>
    {
        public static readonly string Name =
            typeof(T).GetCustomAttribute<NativeTypeAttribute>(inherit: false)?.Name ?? typeof(T).Name;
    }
}
