using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// The entry point every script-declared function dispatches through, reading its arguments out of the frame through the parameters.
public static unsafe class ScriptFunctionDispatch
{
    // Keyed by function and type, since a re-mint can reuse a freed function's address.
    private static readonly Dictionary<(IntPtr Function, Type Type), FBound> BoundByFunction = new();

    // A direct-mapped cache answers the repeat case, since hashing the tuple costs more than the call.
    private const int CacheLines = 256;
    private static readonly FCacheLine?[] Cache = new FCacheLine?[CacheLines];

    // The offset arrays this dispatcher allocated, so a reset frees them rather than leaking a generation.
    private static readonly List<nint> OwnedOffsets = new();

    // Immutable and published by one reference write, so a call on a worker never reads half of a line.
    private sealed class FCacheLine
    {
        public FCacheLine(IntPtr Function, Type Type, FBound Bound)
        {
            this.Function = Function;
            this.Type = Type;
            this.Bound = Bound;
        }

        public readonly IntPtr Function;
        public readonly Type   Type;
        public readonly FBound Bound;
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

            // Straight off the cache line when an invoker is bound, without copying the whole entry.
            FCacheLine? Line = Cache[LineOf(Function)];
            if (Line != null && Line.Function == Function && ReferenceEquals(Line.Type, Owner) && Line.Bound.Invoker != 0)
            {
                ((delegate* unmanaged<nint, nint, int*, void>)Line.Bound.Invoker)(
                    Instance, Frame, Line.Bound.Offsets);
                return;
            }

            if (!TryBind(Owner, Function, out FBound Bound))
            {
                return;
            }

            if (Bound.Invoker != 0)
            {
                ((delegate* unmanaged<nint, nint, int*, void>)Bound.Invoker)(Instance, Frame, Bound.Offsets);
                return;
            }

            using Engine.Scope Scope = EnterFunction(Target);
            try
            {
                InvokeBound(Target, Bound, Frame);
            }
            catch (TargetInvocationException Thrown) when (Thrown.InnerException != null)
            {
                ReportFunctionException(Target, Bound.Method.Name, Thrown.InnerException);
            }
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
        }
    }

    // Shared by the reflected path and every generated invoker, which native calls directly once it is published.
    [System.ComponentModel.EditorBrowsable(System.ComponentModel.EditorBrowsableState.Never)]
    public static Engine.Scope EnterFunction(object Target)
    {
        return Target is Lumina.CEntityScript Script && !Script.Entity.IsNull
            ? Engine.Push(Script.World, Script.Entity, Script)
            : Engine.Snapshot();
    }

    [System.ComponentModel.EditorBrowsable(System.ComponentModel.EditorBrowsableState.Never)]
    public static void ReportFunctionException(object Target, string Function, Exception Exception)
    {
        NativeBindings.ScriptEventException(Target, Function, Exception);
    }

    private static void InvokeBound(object Target, in FBound Bound, IntPtr Frame)
    {
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

    // Resolved once per function, a signature mismatch reported here rather than read from the wrong offset.
    private static bool TryBind(Type Type, IntPtr Function, out FBound Bound)
    {
        FCacheLine? Line = Cache[LineOf(Function)];
        if (Line != null && Line.Function == Function && ReferenceEquals(Line.Type, Type))
        {
            Bound = Line.Bound;
            return Bound.Method != null;
        }

        // A [ParallelUpdate] script can call into native that calls back here from several workers at once.
        lock (BoundByFunction)
        {
            return TryBindLocked(Type, Function, out Bound);
        }
    }

    private static bool TryBindLocked(Type Type, IntPtr Function, out FBound Bound)
    {
        var Key = (Function, Type);
        if (BoundByFunction.TryGetValue(Key, out Bound))
        {
            Cache[LineOf(Function)] = new FCacheLine(Function, Type, Bound);
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

        // Registered under the declaring type so a subclass inheriting the function shares its invoker, and only after TryBind validated the signature.
        nint Invoker = ScriptInvokerRegistry.Find(Method.DeclaringType!, Name);
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

            // Published so the native thunk calls the generated entry point directly from here on.
            Native.FunctionPublishInvoker(Function, Invoker, (IntPtr)Offsets);
        }

        Bound = new FBound(Method, Parameters, WriteBack, Return, bHasReturn, bAsync, Invoker, Offsets);
        BoundByFunction[Key] = Bound;
        Cache[LineOf(Function)] = new FCacheLine(Function, Type, Bound);
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

    // Dropped on reload, since a bound entry's MethodInfo would pin the context being unloaded.
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
