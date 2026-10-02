using System;
using System.Linq.Expressions;
using System.Reflection;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// <summary>
/// Builds (once, then caches) a fast constructor for a generated wrapper type from its internal
/// <c>(IntPtr)</c> ctor, so the registry can turn a native component pointer into its typed wrapper
/// without per-call reflection.
/// </summary>
internal static class Wrapper<T> where T : class
{
    public static readonly Func<IntPtr, T?> Create = Build();

    /// <summary>
    /// The canonical wrapper for a native CObject: returns the managed instance this object already has if one
    /// is still alive, otherwise creates it and caches a WEAK handle on the object. Two calls for the same
    /// object therefore return the same instance, so reference identity (<c>==</c>, <c>is</c>, dictionary keys)
    /// works and repeated access stops allocating.
    ///
    /// Every hand-out counts as one reference to the object (see NativeObject.Dispose), so holding the wrapper
    /// keeps the object alive the way a TObjectPtr does. The cache itself stays weak, so a wrapper nothing references
    /// is collected normally (releasing its references) and the cache can never pin the collectible script ALC.
    ///
    /// Only valid for CObject-backed wrappers. Component views (NativeStruct) are not objects, have no slot,
    /// and keep using <see cref="Create"/>.
    /// </summary>
    public static T? ForObject(IntPtr Pointer)
    {
        if (Pointer == IntPtr.Zero)
        {
            return null;
        }

        IntPtr Existing = Native.ObjectGetManagedInstance(Pointer);
        if (Existing != IntPtr.Zero)
        {
            // Target is null when the wrapper was collected, or when it belonged to an unloaded script ALC.
            // A type mismatch means the object was previously wrapped as a different (e.g. base) type.
            if (GCHandle.FromIntPtr(Existing).Target is T Cached)
            {
                (Cached as NativeObject)?.AddReference();
                return Cached;
            }
        }

        T? Instance = Create(Pointer);
        if (Instance == null)
        {
            return null;
        }
        (Instance as NativeObject)?.StartCountingReferences();

        // Set frees the handle it replaces, so the stale one above is not leaked.
        GCHandle Weak = GCHandle.Alloc(Instance, GCHandleType.Weak);
        Native.ObjectSetManagedInstance(Pointer, GCHandle.ToIntPtr(Weak));
        return Instance;
    }

    private static Func<IntPtr, T?> Build()
    {
        ConstructorInfo? Constructor = typeof(T).GetConstructor(
            BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public,
            null, new[] { typeof(IntPtr) }, null);
        if (Constructor == null)
        {
            return Handle => null;
        }

        ParameterExpression Parameter = Expression.Parameter(typeof(IntPtr), "handle");
        return Expression.Lambda<Func<IntPtr, T?>>(Expression.New(Constructor, Parameter), Parameter).Compile();
    }
}
