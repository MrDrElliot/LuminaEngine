using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

/// <summary>
/// The single entry point every script-declared reflected function is dispatched through.
/// </summary>
/// <remarks>
/// One entry point serves all of them, which is why native hands it the function being called. Arguments are
/// read back out of the call frame through the function's own parameters: a frame is a container like an
/// object is, so the property accessors already exported address it without any marshalling of its own.
/// </remarks>
public static unsafe class ScriptFunctionDispatch
{
    // Keyed by the FFunction, which is minted once per class and lives as long as it does.
    private static readonly Dictionary<IntPtr, FBound> BoundByFunction = new();

    private readonly struct FBound
    {
        public FBound(MethodInfo Method, IntPtr[] Parameters, IntPtr Return)
        {
            this.Method = Method;
            this.Parameters = Parameters;
            this.Return = Return;
        }

        public readonly MethodInfo Method;
        public readonly IntPtr[]   Parameters;
        public readonly IntPtr     Return;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void InvokeScriptFunction(IntPtr Instance, IntPtr Function, IntPtr Frame)
    {
        try
        {
            if (Instance == IntPtr.Zero || Function == IntPtr.Zero)
            {
                return;
            }

            if (GCHandle.FromIntPtr(Instance).Target is not object Target)
            {
                return;
            }

            if (!TryBind(Target.GetType(), Function, out FBound Bound))
            {
                return;
            }

            ParameterInfo[] Signature = Bound.Method.GetParameters();
            object?[] Arguments = Signature.Length == 0 ? Array.Empty<object>() : new object?[Signature.Length];

            for (int Index = 0; Index < Signature.Length; ++Index)
            {
                Arguments[Index] = FrameMarshal.Read(Frame, Bound.Parameters[Index], Signature[Index].ParameterType);
            }

            object? Result = Bound.Method.Invoke(Target, Arguments);

            if (Bound.Return != IntPtr.Zero)
            {
                FrameMarshal.Write(Frame, Bound.Return, Bound.Method.ReturnType, Result);
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    // Resolved once per function. A signature the frame and the method disagree about is reported here rather
    // than read as whatever happened to be at the offset.
    private static bool TryBind(Type Type, IntPtr Function, out FBound Bound)
    {
        if (BoundByFunction.TryGetValue(Function, out Bound))
        {
            return Bound.Method != null;
        }

        Bound = default;

        string Name = Native.FunctionGetName(Function);
        MethodInfo? Method = Type.GetMethod(Name,
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);

        if (Method == null)
        {
            Debug.LogError($"Script function '{Name}' is reflected on {Type.Name} but the method is gone; the call is dropped.");
            BoundByFunction[Function] = default;
            return false;
        }

        int Count = Native.FunctionParamCount(Function);
        if (Count != Method.GetParameters().Length)
        {
            Debug.LogError($"Script function '{Name}' on {Type.Name} takes {Method.GetParameters().Length} arguments but its frame describes {Count}; the call is dropped.");
            BoundByFunction[Function] = default;
            return false;
        }

        IntPtr[] Parameters = new IntPtr[Count];
        for (int Index = 0; Index < Count; ++Index)
        {
            Parameters[Index] = Native.FunctionParamAt(Function, Index);
        }

        Bound = new FBound(Method, Parameters, Native.FunctionReturnParam(Function));
        BoundByFunction[Function] = Bound;
        return true;
    }

    /// Dropped on hot reload, since the methods a bound entry points at belong to the old assembly.
    internal static void Reset()
    {
        BoundByFunction.Clear();
    }
}
