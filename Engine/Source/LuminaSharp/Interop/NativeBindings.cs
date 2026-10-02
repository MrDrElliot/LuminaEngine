using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// <summary>Resolves native engine exports into raw function pointers for the generated bindings.</summary>
public static unsafe class NativeBindings
{
    // The boundary logger generated bindings call. Public because a routed binding compiles into the owning unit's own assembly, which cannot reach Interop.
    public static void LogException(Exception Exception)
    {
        Interop.LogException(Exception);
    }

    // Script event thunks land here, so a throwing OnAttach or OnReady disables its script instead of ticking it half-built.
    public static void ScriptEventException(object? Target, string Event, Exception Exception)
    {
        string Owner = Target?.GetType().FullName ?? "<released script>";
        Interop.LogException(Exception, Owner + "." + Event);

        if (Target is Lumina.CEntityScript Script && (Event == "OnAttach" || Event == "OnReady"))
        {
            try
            {
                Native.EntityScriptMarkFaulted(Script.Handle);
                Native.Log(ELogLevel.Warn, $"{Owner} stopped ticking because its {Event} threw. It still receives OnDetach.");
            }
            catch (Exception Secondary)
            {
                Interop.LogException(Secondary);
            }
        }
    }

    /// <summary>Resolves an export from a module (by name) to a function pointer; null on miss.</summary>
    public static void* Resolve(string Module, string EntryPoint)
    {
        return Resolve(Module, EntryPoint, 0);
    }

    public static void* Resolve(string Module, string EntryPoint, int ExpectedSignature)
    {
        IntPtr Handle = Host.ModuleHandle(Module);
        if (Handle == IntPtr.Zero)
        {
            Native.Log(ELogLevel.Error, $"NativeBindings: module '{Module}' not loaded for '{EntryPoint}'.");
            return null;
        }

        void* Export = ResolveFrom(Handle, EntryPoint);
        return Export != null && !SignatureAgrees(EntryPoint, ExpectedSignature) ? null : Export;
    }

    // An export that registered no width is left unchecked, so only a real disagreement drops the binding.
    private static bool SignatureAgrees(string EntryPoint, int Expected)
    {
        if (Expected <= 0 || ExportSignatureExport == null)
        {
            return true;
        }

        Span<byte> Scratch = stackalloc byte[256];
        Interop.FInteropString Utf8 = new(EntryPoint, Scratch);
        int Declared;
        try
        {
            Declared = ExportSignatureExport(Utf8.Pointer, Utf8.Length);
        }
        finally
        {
            Utf8.Free();
        }

        if (Declared < 0 || Declared == Expected)
        {
            return true;
        }

        Native.Log(ELogLevel.Error,
            $"NativeBindings: export '{EntryPoint}' takes {Declared} bytes of arguments natively but the managed "
            + $"binding declares {Expected}. The two sides have drifted, so the binding is dropped.");
        return false;
    }

    // What a call through a dropped or missing binding raises, naming the export rather than faulting at address zero.
    public static InvalidOperationException Unbound(string EntryPoint)
    {
        return new InvalidOperationException(
            $"Native export '{EntryPoint}' is not bound; the NativeBindings error logged when it resolved says why.");
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

    // Indexed by the negative offset PropertyOffset hands back, so FieldAt can name what failed.
    private static readonly List<string> UnresolvedProperties = new();

    // A miss returns a negative sentinel rather than a usable offset, and FieldAt throws on it at the access.
    public static int PropertyOffset(string Type, string Prop)
    {
        int Offset = CallWithStringArgs(Type, Prop, PropertyOffsetByName);
        if (Offset >= 0)
        {
            return Offset;
        }

        Native.Log(ELogLevel.Error,
            $"NativeBindings: property '{Type}.{Prop}' did not resolve, so its C# accessor throws. "
            + "The reflected type or property name is missing or does not match.");
        lock (UnresolvedProperties)
        {
            UnresolvedProperties.Add(Type + "." + Prop);
            return -UnresolvedProperties.Count;
        }
    }

    // Bytes a generated thunk stashed when its result overflowed Scratch, or -1 so the caller calls again.
    public static int TakeCallOverflow(void* Scratch, void* Dest, int Bytes)
    {
        return TakeCallOverflowExport != null ? TakeCallOverflowExport(Scratch, Dest, Bytes) : -1;
    }

    // Unlike PropertyOffset this neither logs nor records, for a caller that retries until the class is complete.
    internal static int TryPropertyOffset(string Type, string Prop) => CallWithStringArgs(Type, Prop, PropertyOffsetByName);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static nint FieldAt(nint Container, nint Offset)
    {
        if (Offset < 0)
        {
            ThrowUnresolvedProperty(Offset);
        }
        return Container + Offset;
    }

    [DoesNotReturn]
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void ThrowUnresolvedProperty(nint Offset)
    {
        string Name = "an unknown property";
        lock (UnresolvedProperties)
        {
            int Index = (int)(-Offset) - 1;
            if (Index >= 0 && Index < UnresolvedProperties.Count)
            {
                Name = UnresolvedProperties[Index];
            }
        }
        throw new InvalidOperationException($"{Name} did not resolve against native reflection, so its C# accessor has no field to reach.");
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

    // Resolved without a check of its own, since it is what every other check asks.
    private static readonly delegate* unmanaged[Cdecl]<byte*, int, int> ExportSignatureExport =
        (delegate* unmanaged[Cdecl]<byte*, int, int>)Resolve(Host.NativeLibrary, "LuminaSharp_ExportSignature");

    private static readonly delegate* unmanaged[Cdecl]<void*, void*, int, int> TakeCallOverflowExport =
        (delegate* unmanaged[Cdecl]<void*, void*, int, int>)Resolve(Host.NativeLibrary, "LuminaSharp_TakeCallOverflow");

    private static readonly delegate* unmanaged[Cdecl]<byte*, int, byte*, int, int> PropertyOffsetByName =
        (delegate* unmanaged[Cdecl]<byte*, int, byte*, int, int>)Resolve(Host.NativeLibrary, "LuminaSharp_PropertyOffsetByName");

    private static readonly delegate* unmanaged[Cdecl]<byte*, int, byte*, int, IntPtr> FindPropertyExport =
        (delegate* unmanaged[Cdecl]<byte*, int, byte*, int, IntPtr>)Resolve(Host.NativeLibrary, "LuminaSharp_FindProperty");
}
