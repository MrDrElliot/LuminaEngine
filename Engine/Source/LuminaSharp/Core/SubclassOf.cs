using System;
using LuminaSharp;

namespace Lumina;

// A reflected class handle that is T or derived from it, mirroring native TSubclassOf.
public readonly struct TSubclassOf<T> : IEquatable<TSubclassOf<T>> where T : NativeObject
{
    // A class is a permanent singleton, so this pointer needs no lifetime handling.
    internal readonly IntPtr ClassPtr;

    internal TSubclassOf(IntPtr Class)
    {
        ClassPtr = Class;
    }

    public bool IsValid => ClassPtr != IntPtr.Zero;

    // Empty when unset.
    public string Name => IsValid ? Native.ClassGetName(ClassPtr) : string.Empty;

    // Null when unset; reads defaults without spawning an instance.
    public T? GetDefaultObject()
    {
        if (!IsValid)
        {
            return null;
        }
        IntPtr Object = Native.ClassGetDefaultObject(ClassPtr);
        return Object == IntPtr.Zero ? null : Wrapper<T>.ForObject(Object);
    }

    // Native re-checks the base, so a wrong widen arrives as invalid.
    public TSubclassOf<TBase> AsBase<TBase>() where TBase : NativeObject => new(ClassPtr);

    // Invalid when nothing is registered under the name; the assignment still checks the base.
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

// Mirrors native TSubStructOf; T is unconstrained since a reflected struct reaches C# as a value struct or a wrapper class.
public readonly struct TSubStructOf<T> : IEquatable<TSubStructOf<T>>
{
    // A struct type is a permanent singleton, so this pointer needs no lifetime handling.
    internal readonly IntPtr StructPtr;

    internal TSubStructOf(IntPtr Struct)
    {
        StructPtr = Struct;
    }

    public bool IsValid => StructPtr != IntPtr.Zero;

    // Empty when unset; pass it to FInstancedStruct.InitializeAs to build one.
    public string Name => IsValid ? Native.StructGetName(StructPtr) : string.Empty;

    // Script-declared or native; the assignment still checks the base.
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
