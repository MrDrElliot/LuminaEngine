using System;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// Base for generated CObject wrappers, identified by object-array index and generation; the canonical wrapper holds a strong reference.
public unsafe class NativeObject : IDisposable
{
    private IntPtr RawHandle;            // pointer captured at construction; the fallback when untracked
    private int ObjectIndex = -1;
    private int ObjectGeneration;        // slot generation at capture; a free/reuse bumps it -> stale
    private byte* EntryPtr;              // the slot's array entry, which outlives every object in it
    private ObjectReference? References; // set on the canonical wrapper, absent on a natively owned Scriptable

    // Read once. The entry outlives its objects, so revalidating through it needs no crossing at all.
    private static readonly int GenerationOffset = Native.ObjectLayoutOffset(0);
    private static readonly int ObjectOffset = Native.ObjectLayoutOffset(1);
    private static readonly int FlagsOffset = Native.ObjectLayoutOffset(2);
    private static readonly uint MarkedDestroy = (uint)Native.ObjectLayoutOffset(3);
    private static readonly bool bFastPathUsable =
        GenerationOffset >= 0 && ObjectOffset >= 0 && FlagsOffset >= 0 && MarkedDestroy != 0;

    protected internal NativeObject(IntPtr Handle)
    {
        BindNativeHandle(Handle);
    }

    // A C# subclass is created first and bound to its already constructed native object afterwards, invalid until then.
    protected NativeObject()
    {
    }

    // The Scriptable hosting path creates the native object first, then binds the managed instance here.
    internal void BindNativeHandle(IntPtr Handle)
    {
        RawHandle = Handle;
        long Packed = Native.ObjectGetHandle(Handle);
        ObjectIndex = unchecked((int)Packed);
        ObjectGeneration = (int)(Packed >> 32);
        EntryPtr = bFastPathUsable && ObjectIndex >= 0 ? (byte*)Native.ObjectGetEntry(Handle) : null;
    }

    // Runs once when the bridge pairs a script instance with its native object, before anything dispatches to it.
    internal virtual void OnScriptInstanceCreated(ScriptableRuntime Runtime, System.Runtime.InteropServices.GCHandle Handle)
    {
    }

    // Called before the wrapper is published to the instance cache, so no other thread can see it yet.
    internal void StartCountingReferences()
    {
        References = new ObjectReference(RawHandle);
        References.Add();
    }

    // A handout from the instance cache. A twin of an object nothing native owns becomes C#'s, held like any wrapper.
    internal void Acquire(IntPtr Pointer)
    {
        if (References != null)
        {
            References.Add();
            return;
        }

        GCHandle Weak = GCHandle.Alloc(this, GCHandleType.Weak);
        if (Native.ObjectAdoptScriptTwin(Pointer, GCHandle.ToIntPtr(Weak)) != 0)
        {
            StartCountingReferences();
            return;
        }
        Weak.Free();
    }

    // For an object whose owner tears it down explicitly, such as a world, so held wrappers stop pinning its shell.
    internal void ReleaseAllReferences()
    {
        References?.ReleaseAll();
    }

    // Drops one handed-out reference, as releasing a TStrongObjectPtr would; undisposed ones go when the wrapper is collected.
    public void Dispose()
    {
        References?.Release();
    }

    public bool IsValid
    {
        get
        {
            if (ObjectIndex < 0)
            {
                return RawHandle != IntPtr.Zero;
            }
            if (EntryPtr != null && *(int*)(EntryPtr + GenerationOffset) == ObjectGeneration)
            {
                byte* Object = *(byte**)(EntryPtr + ObjectOffset);
                return Object != null && (*(uint*)(Object + FlagsOffset) & MarkedDestroy) == 0;
            }
            return Native.ObjectResolve(ObjectIndex, ObjectGeneration) != IntPtr.Zero;
        }
    }

    // False during the schema pass, whose unbound instances must read defaults instead of a null handle.
    protected internal bool HasNativeStorage => ObjectIndex >= 0 || RawHandle != IntPtr.Zero;

    // Not a constructor because the wrapper is created lazily, after a loaded object already holds its authored values.
    protected virtual void __ApplyScriptDefaults()
    {
    }

    // The virtual stays plain protected so any script assembly can override it; the engine calls through here.
    internal void ApplyScriptDefaults() => __ApplyScriptDefaults();

    // The names generated accessors read through, shared with struct handles so one emitter serves both.
    protected internal IntPtr Self => Handle;

    protected internal NativeRef SelfRef => new(Handle);

    protected internal IntPtr Handle
    {
        // Four loads and no crossing, inlined into every accessor; anything unusual goes out of line.
        [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.AggressiveInlining)]
        get
        {
            if (EntryPtr != null && *(int*)(EntryPtr + GenerationOffset) == ObjectGeneration)
            {
                byte* Object = *(byte**)(EntryPtr + ObjectOffset);
                if (Object != null && (*(uint*)(Object + FlagsOffset) & MarkedDestroy) == 0)
                {
                    return (IntPtr)Object;
                }
            }
            return HandleSlow();
        }
    }

    // A recycled slot only a full resolve can tell from a reinstance, a destroyed object, or a wrapper with no fast path.
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    private IntPtr HandleSlow()
    {
        if (ObjectIndex < 0)
        {
            return RawHandle;
        }

        if (EntryPtr != null)
        {
            if (*(int*)(EntryPtr + GenerationOffset) == ObjectGeneration)
            {
                throw Destroyed();
            }
            return ResolveRedirected();
        }

        IntPtr Pointer = Native.ObjectResolve(ObjectIndex, ObjectGeneration);
        if (Pointer == IntPtr.Zero)
        {
            throw Destroyed();
        }
        return Pointer;
    }

    // A generation bump means the slot was recycled or the object reinstanced, and only native can tell which.
    private IntPtr ResolveRedirected()
    {
        IntPtr Pointer = Native.ObjectResolve(ObjectIndex, ObjectGeneration);
        if (Pointer == IntPtr.Zero)
        {
            throw Destroyed();
        }

        RawHandle = Pointer;
        long Packed = Native.ObjectGetHandle(Pointer);
        ObjectIndex = unchecked((int)Packed);
        ObjectGeneration = (int)(Packed >> 32);
        EntryPtr = ObjectIndex >= 0 ? (byte*)Native.ObjectGetEntry(Pointer) : null;
        References?.Redirect(Pointer);
        return Pointer;
    }

    private static InvalidOperationException Destroyed()
    {
        return new InvalidOperationException(
            "Use of a destroyed native object: the CObject this wrapper referenced was destroyed by its owner, " +
            "for example a deleted asset or a torn-down world.");
    }

    public void ThrowIfInvalid()
    {
        _ = Handle;
    }
}
