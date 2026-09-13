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

    // The same wrapper for a type only known at runtime, which is what a reflective call has. The generic
    // instantiation is cached, since resolving it per call would cost more than the call.
    private static readonly global::System.Collections.Generic.Dictionary<Type, global::System.Reflection.MethodInfo> FromHandleByType = new();

    /// <summary>The canonical wrapper, for a type resolved at runtime rather than named in source.</summary>
    public static NativeObject? FromHandleOfType(IntPtr Object, Type Wanted)
    {
        if (Object == IntPtr.Zero)
        {
            return null;
        }

        global::System.Reflection.MethodInfo? Made;
        lock (FromHandleByType)
        {
            if (!FromHandleByType.TryGetValue(Wanted, out Made))
            {
                Made = typeof(NativeObjectMarshal)
                    .GetMethod(nameof(FromHandle))!
                    .MakeGenericMethod(Wanted);
                FromHandleByType[Wanted] = Made;
            }
        }

        return Made.Invoke(null, new object[] { Object }) as NativeObject;
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
