using System;

namespace LuminaSharp;

/// <summary>
/// Base for the generated opaque wrappers around native CObjects. Holds a WEAK handle (the CObject's
/// object-array index + generation) rather than a bare pointer.
/// </summary>
public class NativeObject
{
    private IntPtr RawHandle;            // pointer captured at construction; the fallback when untracked
    private int ObjectIndex = -1;        // GObjectArray slot, or -1 if the object isn't array-tracked
    private int ObjectGeneration;        // slot generation at capture; a free/reuse bumps it -> stale

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
    }

    /// <summary>True while the native CObject this wraps is still alive. </summary>
    public bool IsValid => ObjectIndex < 0
        ? RawHandle != IntPtr.Zero
        : Native.ObjectResolve(ObjectIndex, ObjectGeneration) != IntPtr.Zero;

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
    protected internal virtual void __ApplyScriptDefaults()
    {
    }

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
            IntPtr Pointer = Native.ObjectResolve(ObjectIndex, ObjectGeneration);
            if (Pointer == IntPtr.Zero)
            {
                throw new InvalidOperationException(
                    "Use of a destroyed native object: the CObject this wrapper referenced has been freed. " +
                    "Don't cache wrappers across frames or structural changes, re-fetch it (Asset.Load, the property, ...).");
            }
            return Pointer;
        }
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
