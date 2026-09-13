using System;
using Lumina;

namespace LuminaSharp;

/// <summary>
/// Constructs CObjects from script, the C# face of native NewObject.
/// </summary>
/// <remarks>
/// Nothing holds the result on your behalf. Lifetime here is refcounting rather than a garbage collector, so
/// an object you make and never store is leaked rather than collected: put it somewhere that owns it, which
/// for a script usually means a [Property] field, whose native side is a strong TObjectPtr.
///
/// A wrapper is a weak handle, so one whose object is later destroyed reads back invalid rather than dangling.
/// </remarks>
public static class ObjectFactory
{
    /// <summary>Constructs T in the transient package under a generated name.</summary>
    public static T? New<T>() where T : NativeObject
    {
        return New<T>(TSubclassOf<T>.FromName(NativeTypeName.Of<T>()));
    }

    /// <summary>Constructs T under Name, empty meaning the class generates a unique one.</summary>
    public static T? New<T>(string Name) where T : NativeObject
    {
        return New<T>(TSubclassOf<T>.FromName(NativeTypeName.Of<T>()), Name);
    }

    /// <summary>Constructs the class Class names, which may be any subclass of T.</summary>
    public static T? New<T>(TSubclassOf<T> Class, string Name = "") where T : NativeObject
    {
        if (!Class.IsValid)
        {
            Debug.LogError($"ObjectFactory.New<{typeof(T).Name}>: no class to construct; is the type reflected?");
            return null;
        }

        IntPtr Object = Native.NewObject(Class.ClassPtr, IntPtr.Zero, Name ?? string.Empty);
        return Object == IntPtr.Zero ? null : NativeObjectMarshal.FromHandle<T>(Object);
    }
}
