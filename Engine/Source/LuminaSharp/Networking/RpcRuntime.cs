using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// Called by the code the script compiler writes into every [Rpc] method. Native routes and serializes the call exactly as it does a C++ NET_RPC.
[EditorBrowsable(EditorBrowsableState.Never)]
public static unsafe class RpcRuntime
{
    internal sealed class FBinding
    {
        public required IntPtr Function;
        public required FrameMarshal.FSlot[] Slots;
    }

    // Keyed weakly by type, so a hot reload's old types are not pinned. A null entry is a method that failed to bind.
    private static readonly ConditionalWeakTable<Type, Dictionary<string, FBinding?>> Bindings = new();

    internal static bool IsReceiving => Lumina.CEntityScript.IsReceivingRpc();

    internal static Connection CurrentCaller()
    {
        return IsReceiving ? new Connection(Lumina.CEntityScript.GetReceivingCaller()) : Connection.Local;
    }

    // False runs the body here and only here. True means the call goes out once Call has its arguments.
    public static bool Begin(Lumina.CEntityScript Script, string Method, out RpcCall Call)
    {
        Call = default;
        FBinding? Binding = Bind(Script, Method);
        if (Binding == null)
        {
            return false;
        }

        Lumina.ERpcRoute Route = Script.PlanRpcCall(Binding.Function);
        if (Route == Lumina.ERpcRoute.RunHere)
        {
            return false;
        }

        Call = new RpcCall(Script, Binding, Lumina.CEntityScript.MakeRpcFrame(Binding.Function), Route == Lumina.ERpcRoute.SendOnly);
        return true;
    }

    private static FBinding? Bind(Lumina.CEntityScript Script, string Method)
    {
        Dictionary<string, FBinding?> ByName = Bindings.GetValue(Script.GetType(), _ => new Dictionary<string, FBinding?>(StringComparer.Ordinal));
        lock (ByName)
        {
            if (!ByName.TryGetValue(Method, out FBinding? Binding))
            {
                Binding = Build(Script, Method);
                ByName[Method] = Binding;
            }
            return Binding;
        }
    }

    private static FBinding? Build(Lumina.CEntityScript Script, string Method)
    {
        Type Type = Script.GetType();
        IntPtr Function = Script.FindRpcFunction(Method);
        MethodInfo? Info = Type.GetMethod(Method, BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.FlattenHierarchy);
        if (Function == IntPtr.Zero || Info == null)
        {
            Native.Log(ELogLevel.Error, $"[Net] {Type.Name}.{Method} has no reflected RPC behind it, so calls to it only run here.");
            return null;
        }

        ParameterInfo[] Parameters = Info.GetParameters();
        if (Parameters.Length != Native.FunctionParamCount(Function))
        {
            Native.Log(ELogLevel.Error, $"[Net] {Type.Name}.{Method} takes {Parameters.Length} arguments but its reflected frame describes {Native.FunctionParamCount(Function)}.");
            return null;
        }

        var Slots = new FrameMarshal.FSlot[Parameters.Length];
        for (int Index = 0; Index < Parameters.Length; ++Index)
        {
            string Where = $"[Rpc] {Type.Name}.{Method}, argument '{Parameters[Index].Name}'";
            if (!FrameMarshal.TryBind(Native.FunctionParamAt(Function, Index), Parameters[Index].ParameterType, Where, out Slots[Index]))
            {
                return null;
            }
        }
        return new FBinding { Function = Function, Slots = Slots };
    }

    // Native brackets each replicated update with this, so a change handler sees the old value and then the new one.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvStdcall) })]
    public static void NetDispatchSync(IntPtr Handle, byte* Names, int NamesSize, int After)
    {
        if (GCHandle.FromIntPtr(Handle).Target is not Lumina.CEntityScript Script)
        {
            return;
        }

        using var Scope = Engine.Push(Script.World, Script.Entity, Script);
        try
        {
            SyncChanges.Dispatch(Script, new ReadOnlySpan<byte>(Names, NamesSize), After != 0);
        }
        catch (Exception Exception)
        {
            NativeBindings.ScriptEventException(Script, "Change", Exception);
        }
    }
}

// One outgoing call, whose arguments go straight into the method's native call frame.
[EditorBrowsable(EditorBrowsableState.Never)]
public ref struct RpcCall
{
    private readonly Lumina.CEntityScript Script;
    private readonly RpcRuntime.FBinding Binding;
    private readonly IntPtr Frame;
    private readonly bool bSendOnly;
    private int Next;

    internal RpcCall(Lumina.CEntityScript Script, RpcRuntime.FBinding Binding, IntPtr Frame, bool bSendOnly)
    {
        this.Script = Script;
        this.Binding = Binding;
        this.Frame = Frame;
        this.bSendOnly = bSendOnly;
        Next = 0;
    }

    public void Add<T>(T Value)
    {
        if (Frame != IntPtr.Zero && Next < Binding.Slots.Length)
        {
            try
            {
                FrameMarshal.Write(Frame, Binding.Slots[Next], Value);
            }
            catch
            {
                // Send never runs once this throws, so the arguments already written are torn down here.
                Lumina.CEntityScript.DiscardRpcFrame(Binding.Function, Frame);
                throw;
            }
        }
        ++Next;
    }

    // Sends the call and frees the frame. True when the call does not also run here.
    public bool Send()
    {
        Script.SendRpcCall(Binding.Function, Frame);
        return bSendOnly;
    }
}
