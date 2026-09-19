using System;

namespace LuminaSharp;

/// <summary>
/// Base for the generated opaque wrappers around native CObjects. Holds a WEAK handle (the CObject's
/// object-array index + generation) rather than a bare pointer.
/// </summary>
public unsafe class NativeObject
{
    private IntPtr RawHandle;            // pointer captured at construction; the fallback when untracked
    private int ObjectIndex = -1;        // GObjectArray slot, or -1 if the object isn't array-tracked
    private int ObjectGeneration;        // slot generation at capture; a free/reuse bumps it -> stale
    private byte* EntryPtr;              // the slot's array entry, which outlives every object in it

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

    /// <summary>Parameterless ctor for a managed-first subclass: a C# subclass of a <c>REFLECT(Scriptable)</c>
    /// native class is Activator-created, then paired to its (already-constructed) native object via
    /// <see cref="BindNativeHandle"/>. Until bound the wrapper is invalid.</summary>
    protected NativeObject()
    {
    }

    /// <summary>Pairs this wrapper with its native CObject after construction. Used by the Scriptable hosting
    /// path, which creates the native object first, then binds the managed instance to it.</summary>
    internal void BindNativeHandle(IntPtr Handle)
    {
        RawHandle = Handle;
        long Packed = Native.ObjectGetHandle(Handle);
        ObjectIndex = unchecked((int)Packed);
        ObjectGeneration = (int)(Packed >> 32);
        EntryPtr = bFastPathUsable && ObjectIndex >= 0 ? (byte*)Native.ObjectGetEntry(Handle) : null;
    }

    /// <summary>True while the native CObject this wraps is still alive. </summary>
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

    /// <summary>True once this wrapper has been paired with a native object at all.
    /// <para>A <c>[Property]</c> accessor is a view over native bytes, so it has nothing to read
    /// before the pairing happens -- and there IS a moment before it: the schema pass Activator-creates one
    /// unbound instance per script type purely to describe the type to the engine. The generated accessors
    /// gate on this so that pass reads defaults instead of dereferencing a null handle.</para></summary>
    protected internal bool HasNativeStorage => ObjectIndex >= 0 || RawHandle != IntPtr.Zero;

    /// <summary>
    /// Writes this type's declared <c>[Property]</c> initializers into whatever native object this wrapper is
    /// bound to. Overridden by generated code (see ScriptPropertyRewriter) and called by the engine exactly
    /// once, against the class default object; every instance is then copied from that.
    ///
    /// It is not a constructor for a reason: the managed wrapper is created lazily, AFTER a loaded object
    /// already holds its authored values, so assigning there would overwrite them with the declared default.
    /// </summary>
    protected virtual void __ApplyScriptDefaults()
    {
    }

    // Plain protected on the virtual, so a script assembly overrides it the same way whether or not it is
    // a friend of this one; the engine reaches it through this wrapper.
    internal void ApplyScriptDefaults() => __ApplyScriptDefaults();

    /// <summary>The live native pointer. Throws <see cref="InvalidOperationException"/> if the object has
    /// been destroyed; every generated accessor reads through here, so touching a dead reference fails
    /// loudly rather than corrupting memory. (An object the array doesn't track falls back to the raw
    /// pointer, preserving the old behavior.)
    /// <para>protected, not internal: a user script is a subclass in ANOTHER assembly, and the accessors
    /// ScriptPropertyRewriter emits for it read through here.</para></summary>
    protected internal IntPtr Handle
    {
        get
        {
            if (ObjectIndex < 0)
            {
                return RawHandle;
            }

            // Four loads and no crossing, which is the whole point; a recycled slot falls to the slow path
            // because only a full resolve knows whether the handle was redirected by a reinstance.
            if (EntryPtr != null)
            {
                if (*(int*)(EntryPtr + GenerationOffset) == ObjectGeneration)
                {
                    byte* Object = *(byte**)(EntryPtr + ObjectOffset);
                    if (Object != null && (*(uint*)(Object + FlagsOffset) & MarkedDestroy) == 0)
                    {
                        return (IntPtr)Object;
                    }
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
        return Pointer;
    }

    private static InvalidOperationException Destroyed()
    {
        return new InvalidOperationException(
            "Use of a destroyed native object: the CObject this wrapper referenced has been freed. " +
            "Don't cache wrappers across frames or structural changes, re-fetch it (Asset.Load, the property, ...).");
    }

    /// <summary>Throws <see cref="InvalidOperationException"/> if the object has been destroyed.</summary>
    public void ThrowIfInvalid()
    {
        _ = Handle;
    }
}

/// <summary>
/// Base for the generated opaque wrappers around native structs that are NOT blittable.
/// </summary>
public class NativeStruct
{
    private IntPtr RawHandle;

    protected internal NativeStruct(IntPtr Handle)
    {
        RawHandle = Handle;
    }

    // A borrow, not a reference. Fetch through Registry.Get where you use it and never store one.
    protected internal IntPtr Handle
    {
        get => RawHandle;
        set => RawHandle = value;
    }
}
