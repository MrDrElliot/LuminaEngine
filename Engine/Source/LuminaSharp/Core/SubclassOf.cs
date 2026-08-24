using System;
using LuminaSharp;

namespace Lumina;

/// A reflected class handle guaranteed to be T or a class derived from it, mirroring native TSubclassOf.
public readonly struct TSubclassOf<T> : IEquatable<TSubclassOf<T>> where T : NativeObject
{
    /// The native CClass pointer. A class is a permanent singleton, so this needs no lifetime handling.
    internal readonly IntPtr ClassPtr;

    internal TSubclassOf(IntPtr Class)
    {
        ClassPtr = Class;
    }

    public bool IsValid => ClassPtr != IntPtr.Zero;

    /// The class's registered name, empty when unset.
    public string Name => IsValid ? Native.ClassGetName(ClassPtr) : string.Empty;

    /// The class default object, or null when unset. Reads defaults without spawning an instance.
    public T? GetDefaultObject()
    {
        if (!IsValid)
        {
            return null;
        }
        IntPtr Object = Native.ClassGetDefaultObject(ClassPtr);
        return Object == IntPtr.Zero ? null : Wrapper<T>.ForObject(Object);
    }

    /// Resolves a class by name. Invalid when nothing is registered under it; the assignment still checks the base.
    public static TSubclassOf<T> FromName(string ClassName)
    {
        return string.IsNullOrEmpty(ClassName)
            ? default
            : new TSubclassOf<T>(Native.FindClassByName(ClassName));
    }

    public bool Equals(TSubclassOf<T> Other) => ClassPtr == Other.ClassPtr;

    public override bool Equals(object? Obj) => Obj is TSubclassOf<T> Other && Equals(Other);

    public override int GetHashCode() => ClassPtr.GetHashCode();

    public static bool operator ==(TSubclassOf<T> Left, TSubclassOf<T> Right) => Left.Equals(Right);

    public static bool operator !=(TSubclassOf<T> Left, TSubclassOf<T> Right) => !Left.Equals(Right);

    public override string ToString() => IsValid ? Name : "None";
}

/// A struct handle that is T or derived, mirroring native TSubStructOf. T is unconstrained since a reflected struct reaches C# as either a value struct or a wrapper class.
public readonly struct TSubStructOf<T> : IEquatable<TSubStructOf<T>>
{
    /// The native CStruct pointer. A struct type is a permanent singleton, so this needs no lifetime handling.
    internal readonly IntPtr StructPtr;

    internal TSubStructOf(IntPtr Struct)
    {
        StructPtr = Struct;
    }

    public bool IsValid => StructPtr != IntPtr.Zero;

    /// The struct's registered name, empty when unset. Pass it to FInstancedStruct.InitializeAs to build one.
    public string Name => IsValid ? Native.StructGetName(StructPtr) : string.Empty;

    /// Resolves a struct by name, script-declared or native. The assignment still checks the base.
    public static TSubStructOf<T> FromName(string StructName)
    {
        return string.IsNullOrEmpty(StructName)
            ? default
            : new TSubStructOf<T>(Native.FindStructByName(StructName));
    }

    public bool Equals(TSubStructOf<T> Other) => StructPtr == Other.StructPtr;

    public override bool Equals(object? Obj) => Obj is TSubStructOf<T> Other && Equals(Other);

    public override int GetHashCode() => StructPtr.GetHashCode();

    public static bool operator ==(TSubStructOf<T> Left, TSubStructOf<T> Right) => Left.Equals(Right);

    public static bool operator !=(TSubStructOf<T> Left, TSubStructOf<T> Right) => !Left.Equals(Right);

    public override string ToString() => IsValid ? Name : "None";
}
