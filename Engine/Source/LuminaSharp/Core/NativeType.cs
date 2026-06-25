using System;

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
