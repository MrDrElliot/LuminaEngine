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
    // Keyed by the function AND the type, because a re-mint frees the old FFunction and the arena can hand
    // its address straight back to a new one; on the pointer alone that would silently dispatch the old method.
    private static readonly Dictionary<(IntPtr Function, Type Type), FBound> BoundByFunction = new();

    // Hashing that tuple costs more than the call it guards, so a direct-mapped line answers the repeat case
    // and the dictionary stays the authority behind it. Power of two, masked rather than divided.
    private const int CacheLines = 256;
    private static readonly FCacheLine[] Cache = new FCacheLine[CacheLines];

    // The offset arrays this dispatcher allocated, so a reset frees them rather than leaking a generation.
    private static readonly List<nint> OwnedOffsets = new();

    private struct FCacheLine
    {
        public IntPtr Function;
        public Type?  Type;
        public FBound Bound;
    }

    // An FFunction is arena allocated, so the low bits carry no entropy and the shift is what spreads it.
    private static int LineOf(IntPtr Function)
    {
        return (int)(((ulong)Function >> 4) & (CacheLines - 1));
    }

    private readonly struct FBound
    {
        public FBound(MethodInfo Method, FrameMarshal.FSlot[] Parameters, bool[]? WriteBack,
            FrameMarshal.FSlot Return, bool bHasReturn, bool bAsync, nint Invoker, int* Offsets)
        {
            this.Method = Method;
            this.Parameters = Parameters;
            this.WriteBack = WriteBack;
            this.Return = Return;
            this.bHasReturn = bHasReturn;
            this.bAsync = bAsync;
            this.Invoker = Invoker;
            this.Offsets = Offsets;
        }

        public readonly MethodInfo            Method;
        public readonly FrameMarshal.FSlot[]  Parameters;

        // Null when nothing is by-ref, which is the ordinary signature and skips the write-back pass.
        public readonly bool[]?               WriteBack;
        public readonly FrameMarshal.FSlot    Return;
        public readonly bool                  bHasReturn;
        public readonly bool                  bAsync;

        // A managed function pointer to the generated entry point, zero when the generator declined.
        public readonly nint                  Invoker;
        public readonly int*                  Offsets;
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

            Type Owner = Target.GetType();

            // Straight off the cache line when a generated invoker is bound, so the whole FBound is never
            // copied out just to reach two of its fields.
            ref FCacheLine Line = ref Cache[LineOf(Function)];
            if (Line.Function == Function && ReferenceEquals(Line.Type, Owner) && Line.Bound.Invoker != 0)
            {
                ((delegate* managed<nint, nint, int*, void>)Line.Bound.Invoker)(
                    Instance, Frame, Line.Bound.Offsets);
                return;
            }

            if (!TryBind(Owner, Function, out FBound Bound))
            {
                return;
            }

            if (Bound.Invoker != 0)
            {
                ((delegate* managed<nint, nint, int*, void>)Bound.Invoker)(Instance, Frame, Bound.Offsets);
                return;
            }

            int Count = Bound.Parameters.Length;
            object?[] Arguments = Count == 0 ? Array.Empty<object>() : new object?[Count];

            for (int Index = 0; Index < Count; ++Index)
            {
                Arguments[Index] = FrameMarshal.Read(Frame, Bound.Parameters[Index]);
            }

            object? Result = Bound.Method.Invoke(Target, Arguments);
            if (Bound.bAsync)
            {
                Result = ScriptAsync.Track(Result);
            }

            // Invoke assigns an out or ref argument back into the array, which the caller reads off the frame.
            if (Bound.WriteBack != null)
            {
                for (int Index = 0; Index < Count; ++Index)
                {
                    if (Bound.WriteBack[Index])
                    {
                        FrameMarshal.Write(Frame, Bound.Parameters[Index], Arguments[Index]);
                    }
                }
            }

            if (Bound.bHasReturn)
            {
                FrameMarshal.Write(Frame, Bound.Return, Result);
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
        ref FCacheLine Line = ref Cache[LineOf(Function)];
        if (Line.Function == Function && ReferenceEquals(Line.Type, Type))
        {
            Bound = Line.Bound;
            return Bound.Method != null;
        }

        var Key = (Function, Type);
        if (BoundByFunction.TryGetValue(Key, out Bound))
        {
            Line.Function = Function;
            Line.Type = Type;
            Line.Bound = Bound;
            return Bound.Method != null;
        }

        Bound = default;

        string Name = Native.FunctionGetName(Function);
        MethodInfo? Method = FindMethod(Type, Name, out int Overloads);

        if (Method == null)
        {
            Debug.LogError(Overloads > 1
                ? $"Script function '{Name}' names {Overloads} methods on {Type.Name}, which a name cannot tell apart; the call is dropped. Give each one its own name."
                : $"Script function '{Name}' is reflected on {Type.Name} but the method is gone; the call is dropped.");
            BoundByFunction[Key] = default;
            return false;
        }

        ParameterInfo[] Signature = Method.GetParameters();
        int Count = Native.FunctionParamCount(Function);

        IntPtr ReturnParam = Native.FunctionReturnParam(Function);
        bool bHasReturn = ReturnParam != IntPtr.Zero;

        if (Count != Signature.Length)
        {
            Debug.LogError($"Script function '{Name}' on {Type.Name} takes {Signature.Length} arguments but its frame describes {Count}; the call is dropped.");
            BoundByFunction[Key] = default;
            return false;
        }

        bool bAsync = ScriptAsync.IsFireAndForget(Method.ReturnType);
        bool bMethodReturns = Method.ReturnType != typeof(void);
        if (bHasReturn != bMethodReturns)
        {
            Debug.LogError($"Script function '{Name}' on {Type.Name} returns {Method.ReturnType.Name} but its frame {(bHasReturn ? "describes a return value" : "describes none")}; the call is dropped.");
            BoundByFunction[Key] = default;
            return false;
        }

        FrameMarshal.FSlot[] Parameters = new FrameMarshal.FSlot[Signature.Length];
        bool[]? WriteBack = null;

        for (int Index = 0; Index < Signature.Length; ++Index)
        {
            string Where = $"Script function '{Name}' on {Type.Name}, argument '{Signature[Index].Name}'";
            if (!FrameMarshal.TryBind(Native.FunctionParamAt(Function, Index), Signature[Index].ParameterType,
                    Where, out Parameters[Index]))
            {
                BoundByFunction[Key] = default;
                return false;
            }

            // A view writes through to the slot as the callee edits it, so a ref one needs no copy back.
            if (FrameMarshal.DirectionOf(Signature[Index]) != EScriptParamFlags.None
                && !FrameMarshal.IsViewOnly(Parameters[Index]))
            {
                WriteBack ??= new bool[Signature.Length];
                WriteBack[Index] = true;
            }
        }

        // An async function's slot holds the token it is tracked under, not the Task it declared.
        Type ReturnDeclared = bAsync ? typeof(ulong) : Method.ReturnType;

        FrameMarshal.FSlot Return = default;
        if (bHasReturn && !FrameMarshal.TryBind(ReturnParam, ReturnDeclared,
                $"Script function '{Name}' on {Type.Name}, return value", out Return))
        {
            BoundByFunction[Key] = default;
            return false;
        }

        // A view borrows storage it does not own, so there is nothing for it to hand back by value.
        if (bHasReturn && FrameMarshal.IsViewOnly(Return))
        {
            Debug.LogError($"Script function '{Name}' on {Type.Name} returns {Method.ReturnType.Name}, a view over storage it does not own; the call is dropped. Take it as a parameter and fill it in place instead.");
            BoundByFunction[Key] = default;
            return false;
        }

        // Only after TryBind validated the same signature, so the invoker can never widen what is accepted.
        nint Invoker = ScriptInvokerRegistry.Find(Type, Name);
        int* Offsets = null;
        if (Invoker != 0)
        {
            // Unmanaged, so the invoker indexes it with no bounds check and the GC never has to pin it.
            int Count2 = Parameters.Length + (bHasReturn ? 1 : 0);
            Offsets = (int*)NativeMemory.Alloc((nuint)Count2 * sizeof(int));
            OwnedOffsets.Add((nint)Offsets);
            for (int Index = 0; Index < Parameters.Length; ++Index)
            {
                Offsets[Index] = Parameters[Index].Offset;
            }
            if (bHasReturn)
            {
                Offsets[Parameters.Length] = Return.Offset;
            }

            // Published so the native thunk calls the generated entry point directly from here on, which
            // skips this dispatcher's handle lookup, type read and cache probe entirely.
            Native.FunctionPublishInvoker(Function, Invoker, (IntPtr)Offsets);
        }

        Bound = new FBound(Method, Parameters, WriteBack, Return, bHasReturn, bAsync, Invoker, Offsets);
        BoundByFunction[Key] = Bound;
        Line = ref Cache[LineOf(Function)];
        Line.Function = Function;
        Line.Type = Type;
        Line.Bound = Bound;
        return true;
    }

    // Walked rather than looked up with GetMethod, which throws on an overload instead of reporting one.
    private static MethodInfo? FindMethod(Type Type, string Name, out int Overloads)
    {
        MethodInfo? Match = null;
        Overloads = 0;

        foreach (MethodInfo Candidate in Type.GetMethods(
                     BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
        {
            if (Candidate.Name != Name)
            {
                continue;
            }

            // A shadowed base declaration loses to the most derived one, the way a C# call site resolves it.
            if (Match != null && Match.DeclaringType != Candidate.DeclaringType)
            {
                if (Candidate.DeclaringType!.IsSubclassOf(Match.DeclaringType!))
                {
                    Match = Candidate;
                    Overloads = 1;
                }
                continue;
            }

            Match = Candidate;
            ++Overloads;
        }

        return Overloads == 1 ? Match : null;
    }

    /// <summary>
    /// Dropped on hot reload. A bound entry holds a MethodInfo from the generation being unloaded, which
    /// roots its declaring type and so pins the collectible load context: left in place this does not just
    /// go stale, it stops the generation unloading at all.
    /// </summary>
    internal static void Reset()
    {
        BoundByFunction.Clear();
        Array.Clear(Cache);
        // Cleared natively first, since a published pointer outlives this table and would dangle.
        Native.FunctionClearInvokers();
        foreach (nint Offsets in OwnedOffsets)
        {
            NativeMemory.Free((void*)Offsets);
        }
        OwnedOffsets.Clear();
        ScriptInvokerRegistry.Clear();
        FrameMarshal.Reset();
    }
}
