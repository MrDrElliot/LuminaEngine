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

    internal static bool IsReceiving => NetNative.IsReceivingRpc() != 0;

    internal static Connection CurrentCaller()
    {
        return IsReceiving ? new Connection(NetNative.RpcCaller(IntPtr.Zero)) : Connection.Local;
    }

    // False runs the body here and only here. True means the call goes out once Call has its arguments.
    public static bool Begin(EntityScript Script, string Method, out RpcCall Call)
    {
        Call = default;
        IntPtr Handle = NativeObjectMarshal.ToHandle(Script);
        FBinding? Binding = Bind(Script.GetType(), Handle, Method);
        if (Binding == null)
        {
            return false;
        }

        var Route = (ERpcRoute)NetNative.RpcPlan(Handle, Binding.Function);
        if (Route == ERpcRoute.RunHere)
        {
            return false;
        }

        Call = new RpcCall(Handle, Binding, NetNative.RpcFrame(Binding.Function), Route == ERpcRoute.SendOnly);
        return true;
    }

    private static FBinding? Bind(Type Type, IntPtr Handle, string Method)
    {
        Dictionary<string, FBinding?> ByName = Bindings.GetValue(Type, _ => new Dictionary<string, FBinding?>(StringComparer.Ordinal));
        lock (ByName)
        {
            if (!ByName.TryGetValue(Method, out FBinding? Binding))
            {
                Binding = Build(Type, Handle, Method);
                ByName[Method] = Binding;
            }
            return Binding;
        }
    }

    private static FBinding? Build(Type Type, IntPtr Handle, string Method)
    {
        IntPtr Function = NetNative.FindRpc(Handle, Method);
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

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvStdcall) })]
    public static void NetDispatchSession(IntPtr Handle, int Event, uint ConnectionId, int Reason)
    {
        if (GCHandle.FromIntPtr(Handle).Target is not EntityScript Script || Script is not INetworkListener Listener)
        {
            return;
        }

        using var Scope = Engine.Push(Script.World, Script.Entity, Script);
        string Name = Event switch { 0 => "OnConnected", 1 => "OnDisconnected", 2 => "OnJoinedHost", _ => "OnLeftHost" };
        try
        {
            switch (Event)
            {
                case 0:
                    Listener.OnConnected(new Connection(ConnectionId));
                    break;
                case 1:
                    Listener.OnDisconnected(new Connection(ConnectionId));
                    break;
                case 2:
                    Listener.OnJoinedHost();
                    break;
                default:
                    Listener.OnLeftHost((ENetLeaveReason)Reason);
                    break;
            }
        }
        catch (Exception Exception)
        {
            NativeBindings.ScriptEventException(Script, Name, Exception);
        }
    }

    // Native brackets each replicated update with this, so a change handler sees the old value and then the new one.
    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvStdcall) })]
    public static void NetDispatchSync(IntPtr Handle, byte* Names, int NamesSize, int After)
    {
        if (GCHandle.FromIntPtr(Handle).Target is not EntityScript Script)
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

// Mirrors the native ERpcRoute.
internal enum ERpcRoute
{
    RunHere = 0,
    SendAndRunHere = 1,
    SendOnly = 2,
}

// One outgoing call, whose arguments go straight into the method's native call frame.
[EditorBrowsable(EditorBrowsableState.Never)]
public ref struct RpcCall
{
    private readonly IntPtr Script;
    private readonly RpcRuntime.FBinding Binding;
    private readonly IntPtr Frame;
    private readonly bool bSendOnly;
    private int Next;

    internal RpcCall(IntPtr Script, RpcRuntime.FBinding Binding, IntPtr Frame, bool bSendOnly)
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
            FrameMarshal.Write(Frame, Binding.Slots[Next], Value);
        }
        ++Next;
    }

    // Sends the call and frees the frame. True when the call does not also run here.
    public bool Send()
    {
        NetNative.RpcSend(Script, Binding.Function, Frame);
        return bSendOnly;
    }
}
