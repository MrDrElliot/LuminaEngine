using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// Managed entry points native calls that belong to no particular subsystem; everything else reaches managed through a generated [ManagedExport] or the Scriptable bridge.
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
