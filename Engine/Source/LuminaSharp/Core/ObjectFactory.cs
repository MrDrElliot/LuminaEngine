using System;
using Lumina;

namespace LuminaSharp;

// The C# face of NewObject. The result lives while C# references it or native code stores it, like a TStrongObjectPtr.
public static class ObjectFactory
{
    /// <summary>Constructs T in the transient package under a generated name.</summary>
    public static T? New<T>() where T : NativeObject
    {
        return New<T>(NativeClass<T>.Of);
    }

    /// <summary>Constructs T under Name, empty meaning the class generates a unique one.</summary>
    public static T? New<T>(string Name) where T : NativeObject
    {
        return New<T>(NativeClass<T>.Of, Name);
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
