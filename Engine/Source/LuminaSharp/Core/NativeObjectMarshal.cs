using System;

namespace LuminaSharp;

/// <summary>
/// Converts between a native CObject pointer and its managed wrapper, for the accessors
/// ScriptPropertyRewriter emits for an object <c>[Property]</c>.
///
/// It exists because the two pieces those accessors need are not reachable from a user's assembly:
/// <c>Wrapper&lt;T&gt;</c> is internal to this assembly, and <see cref="NativeObject.Handle"/> is protected,
/// so a script can read its OWN handle but not another object's. Routing through here keeps both inside
/// LuminaSharp and leaves the generated code with nothing to reach for.
/// </summary>
public static class NativeObjectMarshal
{
    /// <summary>The canonical wrapper for a native object, or null. Canonical, so reading the same reference
    /// twice returns the same managed instance and reference equality means what a script author expects.</summary>
    public static T? FromHandle<T>(IntPtr Object) where T : NativeObject
    {
        return Object == IntPtr.Zero ? null : Wrapper<T>.ForObject(Object);
    }

    /// <summary>The native pointer behind a wrapper, or zero for null.</summary>
    public static IntPtr ToHandle(NativeObject? Value)
    {
        return Value is null ? IntPtr.Zero : Value.Handle;
    }

    // The same for an opaque struct wrapper, which is a NativeStruct rather than a NativeObject.
    public static IntPtr ToHandle(NativeStruct? Value)
    {
        return Value is null ? IntPtr.Zero : Value.Handle;
    }
}
