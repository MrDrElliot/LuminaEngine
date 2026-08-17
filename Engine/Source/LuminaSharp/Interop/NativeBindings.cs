using System;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// <summary>Resolves native engine exports into raw function pointers for the generated bindings.</summary>
public static unsafe class NativeBindings
{
    /// <summary>Resolves an export from a module (by name) to a function pointer; null on miss.</summary>
    public static void* Resolve(string Module, string EntryPoint)
    {
        IntPtr Handle = Host.ModuleHandle(Module);
        if (Handle == IntPtr.Zero)
        {
            Native.Log(ELogLevel.Error, $"NativeBindings: module '{Module}' not loaded for '{EntryPoint}'.");
            return null;
        }
        return ResolveFrom(Handle, EntryPoint);
    }

    /// <summary>Resolves an export from a known module handle (the bootstrap binds).</summary>
    public static void* ResolveFrom(IntPtr ModuleHandle, string EntryPoint)
    {
        if (ModuleHandle != IntPtr.Zero && NativeLibrary.TryGetExport(ModuleHandle, EntryPoint, out IntPtr Export))
        {
            return (void*)Export;
        }
        Native.Log(ELogLevel.Error, $"NativeBindings: export '{EntryPoint}' not found.");
        return null;
    }

    // Property resolve helpers for the generated bindings: a blittable property caches a byte offset
    // (PropertyOffset), a non-blittable one an FProperty* token (FindProperty), resolved once per property.

    /// <summary>Byte offset of Type.Prop within its container.</summary>
    /// <remarks>
    /// A failed resolve returns -1 natively, and the generated accessors add the offset to the container
    /// pointer unconditionally -- so an unresolved property silently reads and writes the byte *before* the
    /// component instead of the field, leaving the field at its default and corrupting whatever sits there.
    /// That is invisible at the call site, so say so loudly and clamp to 0 rather than let it scribble.
    /// </remarks>
    public static int PropertyOffset(string Type, string Prop)
    {
        int Offset = CallWithStringArgs(Type, Prop, PropertyOffsetByName);
        if (Offset < 0)
        {
            Native.Log(ELogLevel.Error,
                $"NativeBindings: property '{Type}.{Prop}' did not resolve; its C# accessor will not reach the "
                + "native field. The reflected type or property name is missing or does not match.");
            return 0;
        }
        return Offset;
    }

    /// <summary>FProperty* token for Type.Prop (opaque); IntPtr.Zero on miss.</summary>
    public static IntPtr FindProperty(string Type, string Prop) => CallWithStringArgs(Type, Prop, FindPropertyExport);

    // Encodes two strings to UTF-8 stack buffers and invokes a (ptr,len,ptr,len)->T native export.
    private static T CallWithStringArgs<T>(string Type, string Prop, delegate* unmanaged[Cdecl]<byte*, int, byte*, int, T> Fn) where T : unmanaged
    {
        if (Fn == null)
        {
            Native.Log(ELogLevel.Error, "NativeBindings: property-resolve export unresolved.");
            return default;
        }

        Span<byte> TypeScratch = stackalloc byte[256];
        Span<byte> PropScratch = stackalloc byte[256];
        Interop.FInteropString TypeUtf8 = new(Type, TypeScratch);
        Interop.FInteropString PropUtf8 = new(Prop, PropScratch);
        try
        {
            return Fn(TypeUtf8.Pointer, TypeUtf8.Length, PropUtf8.Pointer, PropUtf8.Length);
        }
        finally
        {
            TypeUtf8.Free();
            PropUtf8.Free();
        }
    }

    // Parameter-block memory for an animation graph component, name-checked against the graph's struct.
    public static IntPtr AnimGraphParameterMemory(IntPtr Component, string TypeName)
    {
        if (AnimGraphParameterMemoryExport == null || Component == IntPtr.Zero)
        {
            return IntPtr.Zero;
        }

        Span<byte> Scratch = stackalloc byte[256];
        Interop.FInteropString Utf8 = new(TypeName, Scratch);
        try
        {
            return AnimGraphParameterMemoryExport((void*)Component, Utf8.Pointer, Utf8.Length);
        }
        finally
        {
            Utf8.Free();
        }
    }

    private static readonly delegate* unmanaged[Cdecl]<void*, byte*, int, IntPtr> AnimGraphParameterMemoryExport =
        (delegate* unmanaged[Cdecl]<void*, byte*, int, IntPtr>)Resolve(Host.NativeLibrary, "LuminaSharp_AnimGraph_GetParameterMemory");

    private static readonly delegate* unmanaged[Cdecl]<byte*, int, byte*, int, int> PropertyOffsetByName =
        (delegate* unmanaged[Cdecl]<byte*, int, byte*, int, int>)Resolve(Host.NativeLibrary, "LuminaSharp_PropertyOffsetByName");

    private static readonly delegate* unmanaged[Cdecl]<byte*, int, byte*, int, IntPtr> FindPropertyExport =
        (delegate* unmanaged[Cdecl]<byte*, int, byte*, int, IntPtr>)Resolve(Host.NativeLibrary, "LuminaSharp_FindProperty");
}
