using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// <summary>
/// Hosts live managed instances of C# subclasses of <c>REFLECT(Scriptable)</c> native CObjects. Generic over
/// any Scriptable type, with no per-type hosting code: native creates the CObject (a minted CClass), this
/// runtime instantiates the matching C# subclass and pairs it to the native object via a GCHandle.
///
/// It keeps no handle list of its own. Each handle lives in its native object's managed-instance slot
/// (Core/Object/ManagedInstance.h), which frees it when the object dies and drains the whole table before the
/// collectible ALC unloads -- so there is exactly one owner and no second bookkeeping to keep in step.
/// Per-instance dispatch is driven directly by the Reflector-generated native shim, not from here.
/// </summary>
internal sealed class ScriptableRuntime
{
    private readonly TypeLibrary Library;

    // EntityScripts are created here but resolved and destroyed through EntityScriptRuntime's handle set.
    private readonly EntityScriptRuntime EntityScripts;

    // Cached per user-type override bitmask (which ScriptEvents the subclass overrides); keyed by the user type.
    private readonly Dictionary<Type, int> OverrideFlagsByType = new();

    public ScriptableRuntime(TypeLibrary Library, EntityScriptRuntime EntityScripts)
    {
        this.Library = Library;
        this.EntityScripts = EntityScripts;
    }

    public IReadOnlyCollection<string> TypeNames => Library.ScriptableTypeNames;

    /// <summary>Reports each discovered Scriptable C# type as (full name, native base class name, override
    /// mask) to a native sink, so the host can mint a CClass deriving from that native base. The native base
    /// name is the <c>[NativeType]</c> name of the nearest <c>[ScriptableType]</c> wrapper in the base chain.
    /// The mask is type-uniform, so it rides here and is stamped on the minted CClass once, rather than being
    /// recomputed and stored per instance.</summary>
    public unsafe void Enumerate(IntPtr Sink, IntPtr Context)
    {
        if (Sink == IntPtr.Zero)
        {
            return;
        }

        var Add = (delegate* unmanaged[Stdcall]<IntPtr, byte*, int, byte*, int, ulong, void>)Sink;
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
                Add(Context, Name.Pointer, Name.Length, Base.Pointer, Base.Length, (ulong)GetOverrideFlags(Type));
            }
            finally
            {
                Name.Free();
                Base.Free();
            }
        }
    }

    /// <summary>Reports each (prior name, current name) pair from the <c>[Alias]</c> attributes on script
    /// classes, so the host can record where a renamed class went. Separate from <see cref="Enumerate"/>
    /// rather than folded into it because that sink's arity is part of the native ABI.</summary>
    public unsafe void EnumerateAliases(IntPtr Sink, IntPtr Context)
    {
        if (Sink == IntPtr.Zero)
        {
            return;
        }

        var Add = (delegate* unmanaged[Stdcall]<IntPtr, byte*, int, byte*, int, void>)Sink;
        Span<byte> OldScratch = stackalloc byte[256];
        Span<byte> NewScratch = stackalloc byte[256];
        foreach (KeyValuePair<string, string> Pair in Library.ClassAliases)
        {
            Interop.FInteropString Old = new(Pair.Key, OldScratch);
            Interop.FInteropString New = new(Pair.Value, NewScratch);
            try
            {
                Add(Context, Old.Pointer, Old.Length, New.Pointer, New.Length);
            }
            finally
            {
                Old.Free();
                New.Free();
            }
        }
    }

    /// <summary>Instantiates the named Scriptable subclass, pairs it to the already-created native object, and
    /// returns a STRONG GCHandle (IntPtr.Zero on failure).
    ///
    /// The caller stores the handle in the native object's managed-instance slot
    /// (Core/Object/ManagedInstance.h), which is its only owner: the object's destructor frees it, and the
    /// script teardown contract drains the whole table before the collectible ALC unloads. Strong rather than
    /// weak because this instance holds user state for as long as the native object lives -- letting the GC
    /// take it between dispatches would silently reset the script's fields.</summary>
    public IntPtr Create(string TypeName, ulong NativePtr)
    {
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

        GCHandle Allocated = GCHandle.Alloc(Instance);

        // The one point every script instance passes through, and it runs before the dispatch that created it.
        if (Instance is EntityScript Script)
        {
            PrepareEntityScript(Script, TypeName, Type);
            EntityScripts.Adopt(Allocated);
        }
        return GCHandle.ToIntPtr(Allocated);
    }

    // Description must be set before any dispatch: PollInput and the profiler labels both read it.
    private void PrepareEntityScript(EntityScript Script, string TypeName, Type Type)
    {
        Script.Description = Library.GetEntityScript(TypeName) ?? Library.Describe(Type);

        // Defensive: the driver always SetOwner's before the first dispatch that creates this instance.
        if (Script.Entity.IsNull)
        {
            return;
        }

        try
        {
            Script.Description.EnsureInputComponent(Script);
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"EntityScript component injection threw: {Exception}");
        }
    }

    /// <summary>
    /// Runs a type's declared [Property] initializers into its class default object, once, at mint time.
    ///
    /// The CDO deliberately never keeps a managed instance -- it is never dispatched to -- so one is created
    /// here, bound just long enough for the generated <c>__ApplyScriptDefaults</c> to write through its
    /// accessors into the CDO's native block, and then dropped. Every later instance is copied from that
    /// block by CClass::ConstructScriptProperties, which is how a script gets defaults at all: a minted class
    /// has no C++ constructor to carry them.
    /// </summary>
    public void ApplyDefaults(string TypeName, ulong NativeDefaultObject)
    {
        Type? Type = Library.GetScriptable(TypeName);
        if (Type == null || Activator.CreateInstance(Type) is not NativeObject Instance)
        {
            return;
        }

        Instance.BindNativeHandle(new IntPtr(unchecked((long)NativeDefaultObject)));
        try
        {
            Instance.__ApplyScriptDefaults();
        }
        catch (Exception Exception)
        {
            Native.Log(ELogLevel.Error, $"Script defaults for '{TypeName}' threw: {Exception.Message}");
        }
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
