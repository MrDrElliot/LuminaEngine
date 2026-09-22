using System;

namespace LuminaSharp;

/// Declares a type whose managed value is the address of a native slot rather than a copy of its bytes, so a binder views the slot in place instead of copying it.
[AttributeUsage(AttributeTargets.Struct, Inherited = false)]
public sealed class NativeSlotViewAttribute : Attribute
{
}
