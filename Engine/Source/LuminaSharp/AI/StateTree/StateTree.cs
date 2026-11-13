using System;
using System.Collections.Concurrent;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

// A state tree task written in C#, a [Reflect] struct extending SStateTreeTaskBase whose [Property] fields are its instance data. One without a Tick lets its instance sleep.
public interface IStateTreeTask
{
    // A status other than Running completes the state at once.
    EStateTreeRunStatus EnterState(FStateTreeExecutionContext Context) => EStateTreeRunStatus.Running;

    EStateTreeRunStatus Tick(FStateTreeExecutionContext Context) => EStateTreeRunStatus.Running;

    void ExitState(FStateTreeExecutionContext Context) { }

    void OnEvent(FStateTreeExecutionContext Context, HierarchicalTag EventTag) { }
}

// A state tree condition written in C#, a [Reflect] struct extending SStateTreeConditionBase.
public interface IStateTreeCondition
{
    bool TestCondition(FStateTreeExecutionContext Context);
}

// A state tree evaluator written in C#, a [Reflect] struct extending SStateTreeEvaluatorBase whose [Property] fields are its outputs.
public interface IStateTreeEvaluator
{
    void Tick(FStateTreeExecutionContext Context);
}

// What a C# node sees while it runs, valid only during the call it was handed to.
public readonly unsafe struct FStateTreeExecutionContext
{
    private readonly FStateTreeScriptCall* Call;

    internal FStateTreeExecutionContext(FStateTreeScriptCall* Call)
    {
        this.Call = Call;
    }

    internal IntPtr NodeData => Call->NodeData;

    internal uint EventTag => Call->EventTag;

    public CWorld? GetWorld() => NativeObjectMarshal.FromHandle<CWorld>(Call->World);

    public FEntity GetOwner() => new(Call->Owner);

    public double GetTime() => Call->Time;

    public float GetDeltaTime() => Call->DeltaTime;

    // Wakes a sleeping instance at WakeTime, for a task that does not need every tick.
    public void RequestWakeAt(double WakeTime) => StateTreeNative.StateTreeRequestWakeAt(Call->Context, WakeTime);

    // Queued for the next tick; false when the queue is full.
    public bool SendEvent(HierarchicalTag Tag) => StateTreeNative.StateTreeSendEvent(Call->Context, Tag.Id);
}

// Numbered as EStateTreeScriptOp in DotNetStateTree.h, and named as the interface methods each one runs.
internal enum EStateTreeScriptOp
{
    EnterState,
    Tick,
    ExitState,
    OnEvent,
    TestCondition,
    NeedsTick,
}

// Mirrors StateTreeScript::FCall in DotNetStateTree.h field for field.
[StructLayout(LayoutKind.Sequential)]
internal struct FStateTreeScriptCall
{
    public IntPtr Context;
    public IntPtr World;
    public IntPtr NodeData;
    public double Time;
    public uint Owner;
    public float DeltaTime;
    public uint EventTag;
    public int Default;
}

internal static unsafe partial class StateTreeNative
{
    [NativeCall] public static partial void StateTreeRequestWakeAt(IntPtr Context, double WakeTime);
    [NativeCall] public static partial bool StateTreeSendEvent(IntPtr Context, uint EventTag);

    // Every C# node of every tree runs through here, from whichever worker ticks its instance.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static int StateTreeDispatch(IntPtr Struct, int Op, FStateTreeScriptCall* Call)
    {
        try
        {
            StateTreeNodeInvoker? Invoker = Host.Library?.StateTreeNodes.Find(Struct);
            return Invoker != null ? Invoker.Invoke((EStateTreeScriptOp)Op, Call) : Call->Default;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception);
            return Call->Default;
        }
    }
}

// Runs one C# node type, skipping the interface methods it leaves to their defaults, which a struct could only reach boxed.
internal abstract unsafe class StateTreeNodeInvoker
{
    private readonly uint Defined;

    protected StateTreeNodeInvoker(Type Node, Type Interface)
    {
        InterfaceMapping Map = Node.GetInterfaceMap(Interface);
        for (int Index = 0; Index < Map.TargetMethods.Length; ++Index)
        {
            if (Map.TargetMethods[Index].DeclaringType == Node && Enum.TryParse(Map.InterfaceMethods[Index].Name, out EStateTreeScriptOp Op))
            {
                Defined |= 1u << (int)Op;
            }
        }
    }

    public int Invoke(EStateTreeScriptOp Op, FStateTreeScriptCall* Call)
    {
        if (Op == EStateTreeScriptOp.NeedsTick)
        {
            return Defines(EStateTreeScriptOp.Tick) ? 1 : 0;
        }
        return Defines(Op) ? Run(Op, new FStateTreeExecutionContext(Call)) : Call->Default;
    }

    private bool Defines(EStateTreeScriptOp Op) => (Defined & (1u << (int)Op)) != 0;

    protected abstract int Run(EStateTreeScriptOp Op, FStateTreeExecutionContext Context);
}

internal sealed class StateTreeTaskInvoker<T> : StateTreeNodeInvoker where T : struct, INativeStruct, IStateTreeTask
{
    public StateTreeTaskInvoker() : base(typeof(T), typeof(IStateTreeTask)) { }

    protected override int Run(EStateTreeScriptOp Op, FStateTreeExecutionContext Context)
    {
        T Node = NativeStructs.FromAddress<T>(Context.NodeData);
        switch (Op)
        {
            case EStateTreeScriptOp.EnterState:
                return (int)Node.EnterState(Context);
            case EStateTreeScriptOp.Tick:
                return (int)Node.Tick(Context);
            case EStateTreeScriptOp.ExitState:
                Node.ExitState(Context);
                return 0;
            default:
                Node.OnEvent(Context, new HierarchicalTag(Context.EventTag));
                return 0;
        }
    }
}

internal sealed class StateTreeConditionInvoker<T> : StateTreeNodeInvoker where T : struct, INativeStruct, IStateTreeCondition
{
    public StateTreeConditionInvoker() : base(typeof(T), typeof(IStateTreeCondition)) { }

    protected override int Run(EStateTreeScriptOp Op, FStateTreeExecutionContext Context)
    {
        return NativeStructs.FromAddress<T>(Context.NodeData).TestCondition(Context) ? 1 : 0;
    }
}

internal sealed class StateTreeEvaluatorInvoker<T> : StateTreeNodeInvoker where T : struct, INativeStruct, IStateTreeEvaluator
{
    public StateTreeEvaluatorInvoker() : base(typeof(T), typeof(IStateTreeEvaluator)) { }

    protected override int Run(EStateTreeScriptOp Op, FStateTreeExecutionContext Context)
    {
        NativeStructs.FromAddress<T>(Context.NodeData).Tick(Context);
        return 0;
    }
}

// Each C# node type's invoker by its native struct, owned by the generation whose types it holds.
internal sealed class StateTreeNodeInvokers
{
    private readonly TypeLibrary Library;
    private readonly ConcurrentDictionary<IntPtr, StateTreeNodeInvoker?> Invokers = new();
    private readonly Func<IntPtr, StateTreeNodeInvoker?> Create;

    public StateTreeNodeInvokers(TypeLibrary Library)
    {
        this.Library = Library;
        Create = CreateInvoker;
    }

    // Null for a struct this generation does not define, such as one a reload replaced, which then runs as a native default.
    public StateTreeNodeInvoker? Find(IntPtr Struct) => Invokers.GetOrAdd(Struct, Create);

    private StateTreeNodeInvoker? CreateInvoker(IntPtr Struct)
    {
        Type? Node = Library.GetScriptStruct(Native.StructGetName(Struct))?.Type;
        if (Node == null)
        {
            return null;
        }
        Type? Invoker = typeof(IStateTreeTask).IsAssignableFrom(Node) ? typeof(StateTreeTaskInvoker<>)
            : typeof(IStateTreeCondition).IsAssignableFrom(Node) ? typeof(StateTreeConditionInvoker<>)
            : typeof(IStateTreeEvaluator).IsAssignableFrom(Node) ? typeof(StateTreeEvaluatorInvoker<>)
            : null;
        if (Invoker == null)
        {
            Native.Log(ELogLevel.Warn, $"'{Node.FullName}' is in a state tree but implements none of IStateTreeTask, IStateTreeCondition or IStateTreeEvaluator, so it does nothing.");
            return null;
        }
        return (StateTreeNodeInvoker)Activator.CreateInstance(Invoker.MakeGenericType(Node))!;
    }
}
