using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// <summary>
/// Managed entry points the native side calls that are not tied to a particular subsystem.
///
/// This used to be the inbound half of a Coral-style reflective interop layer: resolve a type by name,
/// construct it, and invoke methods / get / set fields by name, marshalling every value through a
/// self-describing blob codec. Almost none of it was reachable -- native-to-managed dispatch goes through
/// generated <c>[ManagedExport]</c> entries, and C# subclasses of native types go through the Scriptable
/// bridge. Its one real consumer was the inspector's <c>[Button]</c>, which only ever needed "call this
/// parameterless method on this instance". That is now <see cref="InvokeScriptButton"/> directly, and the
/// generic machinery (ClassFind / ObjectNew / Invoke / FieldGet / FieldSet, the value-blob codec, and the
/// C++ FManagedClass / FManagedObject RAII wrappers) is gone.
/// </summary>
public static unsafe class ManagedCalls
{
    /// <summary>Releases a GCHandle held by native code. Used by the per-CObject managed-instance cache
    /// (Core/Object/ManagedInstance.h) to free a wrapper's weak handle when the object dies, when the cached
    /// wrapper is replaced, and when the table is drained on hot reload or shutdown.</summary>
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void FreeHandle(IntPtr Handle)
    {
        try
        {
            if (Handle != IntPtr.Zero)
            {
                GCHandle.FromIntPtr(Handle).Free();
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    /// <summary>Invokes an inspector <c>[Button]</c>: a parameterless instance method, by name, on the script
    /// instance behind <paramref name="Instance"/> (a GCHandle). <c>[Button]</c> methods are contractually
    /// parameterless (TypeLibrary.ComputeButtons rejects anything else), which is why this takes no arguments
    /// and returns nothing -- it is the whole reason the old general-purpose reflective invoke existed.
    /// Returns 0 on success, non-zero on failure (1 null handle, 2 dead target, 3 no such method, 4 threw).</summary>
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int InvokeScriptButton(IntPtr Instance, byte* Name, int NameLength)
    {
        try
        {
            if (Instance == IntPtr.Zero)
            {
                return 1;
            }

            object? Target = GCHandle.FromIntPtr(Instance).Target;
            if (Target == null)
            {
                return 2;
            }

            string MethodName = Interop.GetString(Name, NameLength);
            MethodInfo? Method = Target.GetType().GetMethod(
                MethodName,
                BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.FlattenHierarchy,
                null, Type.EmptyTypes, null);

            if (Method == null)
            {
                Native.Log(ELogLevel.Error,
                    $"[Button]: no parameterless method '{Target.GetType().FullName}.{MethodName}'.");
                return 3;
            }

            Method.Invoke(Target, null);
            return 0;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return 4;
        }
    }
}
