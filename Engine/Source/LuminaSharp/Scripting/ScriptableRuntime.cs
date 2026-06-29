using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// <summary>
/// Hosts live managed instances of C# subclasses of <c>REFLECT(Scriptable)</c> native CObjects. Generic over
/// any Scriptable type, with no per-type hosting code: native creates the CObject (a minted CClass), this
/// runtime instantiates the matching C# subclass and pairs it to the native object via a GCHandle. Mirrors
/// <see cref="EntityScriptRuntime"/> - <see cref="LiveHandles"/> exists only to free every strong handle before
/// the collectible ALC unloads (a live handle pins the generation). Per-instance dispatch is driven directly
/// by the Reflector-generated native shim, not from here.
/// </summary>
internal sealed class ScriptableRuntime
{
    private readonly TypeLibrary Library;
    private readonly HashSet<GCHandle> LiveHandles = new();

    // Cached per user-type override bitmask (which ScriptEvents the subclass overrides); keyed by the user type.
    private readonly Dictionary<Type, int> OverrideFlagsByType = new();

    public ScriptableRuntime(TypeLibrary Library)
    {
        this.Library = Library;
    }

    public IReadOnlyCollection<string> TypeNames => Library.ScriptableTypeNames;

    /// <summary>Reports each discovered Scriptable C# type as (full name, native base class name) to a native
    /// sink, so the host can mint a CClass deriving from that native base. The native base name is the
    /// <c>[NativeType]</c> name of the nearest <c>[ScriptableType]</c> wrapper in the base chain.</summary>
    public unsafe void Enumerate(IntPtr Sink, IntPtr Context)
    {
        if (Sink == IntPtr.Zero)
        {
            return;
        }

        var Add = (delegate* unmanaged[Stdcall]<IntPtr, byte*, int, byte*, int, void>)Sink;
        Span<byte> NameScratch = stackalloc byte[256];
        Span<byte> BaseScratch = stackalloc byte[256];
        foreach (Type Type in Library.ScriptableTypes)
        {
            if (Type.FullName is not { } FullName)
            {
                continue;
            }
            string? NativeBase = ScriptableNativeBaseName(Type);
            if (NativeBase == null)
            {
                continue;
            }

            Interop.FInteropString Name = new(FullName, NameScratch);
            Interop.FInteropString Base = new(NativeBase, BaseScratch);
            try
            {
                Add(Context, Name.Pointer, Name.Length, Base.Pointer, Base.Length);
            }
            finally
            {
                Name.Free();
                Base.Free();
            }
        }
    }

    /// <summary>Instantiates the named Scriptable subclass, pairs it to the already-created native object, and
    /// returns a strong GCHandle (IntPtr.Zero on failure). <paramref name="OverrideFlags"/> reports which
    /// ScriptEvents the subclass overrides so native can skip the boundary for the rest.</summary>
    public IntPtr Create(string TypeName, ulong NativePtr, out int OverrideFlags)
    {
        OverrideFlags = 0;
        Type? Type = Library.GetScriptable(TypeName);
        if (Type == null)
        {
            Native.Log(ELogLevel.Warn, $"Scriptable type not found: '{TypeName}'.");
            return IntPtr.Zero;
        }

        if (Activator.CreateInstance(Type) is not NativeObject Instance)
        {
            Native.Log(ELogLevel.Error, $"Failed to create Scriptable '{TypeName}'.");
            return IntPtr.Zero;
        }

        Instance.BindNativeHandle(new IntPtr(unchecked((long)NativePtr)));
        OverrideFlags = GetOverrideFlags(Type);

        GCHandle Handle = GCHandle.Alloc(Instance);
        LiveHandles.Add(Handle);
        return GCHandle.ToIntPtr(Handle);
    }

    public void Destroy(IntPtr Pointer)
    {
        if (Pointer == IntPtr.Zero)
        {
            return;
        }
        GCHandle Handle = GCHandle.FromIntPtr(Pointer);
        if (LiveHandles.Remove(Handle) && Handle.IsAllocated)
        {
            Handle.Free();
        }
    }

    /// <summary>Frees every live handle so the collectible ALC can unload. Called on reload/shutdown.</summary>
    public void FreeAll()
    {
        foreach (GCHandle Handle in new List<GCHandle>(LiveHandles))
        {
            if (Handle.IsAllocated)
            {
                Handle.Free();
            }
        }
        LiveHandles.Clear();
    }

    // Bit i is set when the user subclass overrides the ScriptEvent the wrapper declared with [ScriptEvent(i)].
    // An override moves the method's DeclaringType out of the engine assembly (LuminaSharp.dll) into user code.
    private int GetOverrideFlags(Type Type)
    {
        if (OverrideFlagsByType.TryGetValue(Type, out int Cached))
        {
            return Cached;
        }

        int Flags = 0;
        Assembly Engine = typeof(ScriptableRuntime).Assembly;
        foreach (MethodInfo Method in Type.GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
        {
            ScriptEventAttribute? Event = Method.GetCustomAttribute<ScriptEventAttribute>(inherit: true);
            if (Event != null && Method.DeclaringType is { } Decl && Decl.Assembly != Engine)
            {
                Flags |= 1 << Event.Index;
            }
        }

        OverrideFlagsByType[Type] = Flags;
        return Flags;
    }

    // The [NativeType] name of the nearest [ScriptableType]-marked wrapper above Type, or null if none.
    private static string? ScriptableNativeBaseName(Type Type)
    {
        for (Type? Base = Type.BaseType; Base != null; Base = Base.BaseType)
        {
            if (Base.GetCustomAttribute<ScriptableTypeAttribute>(inherit: false) != null)
            {
                NativeTypeAttribute? Native = Base.GetCustomAttribute<NativeTypeAttribute>(inherit: false);
                return Native?.Name ?? Base.Name;
            }
        }
        return null;
    }
}
