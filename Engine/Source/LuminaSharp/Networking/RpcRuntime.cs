using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace LuminaSharp;

// Called by the code the script compiler writes into every [Rpc] method and [Sync] setter.
[EditorBrowsable(EditorBrowsableState.Never)]
public static unsafe class RpcRuntime
{
    [ThreadStatic] private static bool bArrivingCall;
    [ThreadStatic] private static uint CallerId;
    [ThreadStatic] internal static int RemoteDepth;

    internal static Connection CurrentCaller()
    {
        return RemoteDepth > 0 ? new Connection(CallerId) : Connection.Local;
    }

    // False runs the body here and only here. True means the call goes out, and RunLocal says whether it also runs here.
    public static bool ShouldSend(EntityScript Script, ERpcTarget Target, out bool RunLocal)
    {
        RunLocal = true;

        // The flag belongs to exactly one call, so an RPC made from inside a received one still routes.
        if (bArrivingCall)
        {
            bArrivingCall = false;
            return false;
        }

        Lumina.CWorld World = Script.World;
        Lumina.ENetMode Mode = World.GetNetMode();
        if (Mode == Lumina.ENetMode.Standalone)
        {
            return false;
        }
        bool bHost = Mode != Lumina.ENetMode.Client;

        switch (Target)
        {
            case ERpcTarget.Broadcast:
                return true;

            case ERpcTarget.Host:
                if (bHost)
                {
                    return false;
                }
                RunLocal = false;
                return true;

            case ERpcTarget.Owner:
                uint Owner = NetNative.GetOwner(World.WorldHandle, Script.Entity.Id);
                if (Owner == Networking.LocalConnectionId(World))
                {
                    return false;
                }
                RunLocal = false;
                return true;
        }
        return false;
    }

    public static NetWriter BeginWrite(EntityScript Script) => new(Script.World.WorldHandle);

    public static void Send(EntityScript Script, uint RpcId, ERpcTarget Target, NetFlags Flags, ref NetWriter Writer)
    {
        ReadOnlySpan<byte> Payload = Writer.Written;
        fixed (byte* Bytes = Payload)
        {
            NetNative.SendRpc(NativeObjectMarshal.ToHandle(Script), RpcId, (uint)Target, (uint)(Flags & NetFlags.Unreliable),
                (IntPtr)Bytes, Payload.Length);
        }
    }

    // Owner-only calls are refused unless they came from the entity's owner or from the host.
    public static bool Permit(EntityScript Script, NetFlags Flags)
    {
        if ((Flags & NetFlags.OwnerOnly) == 0 || RemoteDepth == 0 || CallerId == 0)
        {
            return true;
        }
        return NetNative.GetOwner(Script.World.WorldHandle, Script.Entity.Id) == CallerId;
    }

    // The next routed method entered runs locally, since its arguments already came from the network.
    public static void MarkArriving()
    {
        bArrivingCall = true;
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvStdcall) })]
    public static void NetDispatchRpc(IntPtr Handle, uint RpcId, uint Caller, byte* Payload, int PayloadSize)
    {
        if (GCHandle.FromIntPtr(Handle).Target is not EntityScript Script)
        {
            return;
        }

        uint PriorCaller = CallerId;
        using var Scope = Game.Push(Script.World, Script.Entity, Script);
        try
        {
            ++RemoteDepth;
            CallerId = Caller;
            var Reader = new NetReader(new ReadOnlySpan<byte>(Payload, PayloadSize), Script.World.WorldHandle);
            if (!Script.InvokeRpc(RpcId, ref Reader))
            {
                Native.Log(ELogLevel.Warn, $"[Net] {Script.GetType().Name} has no RPC with id {RpcId}; the peers are running different script builds.");
            }
            else if (Reader.Failed)
            {
                Native.Log(ELogLevel.Warn, $"[Net] RPC {RpcId} on {Script.GetType().Name} arrived truncated.");
            }
        }
        catch (Exception Exception)
        {
            NativeBindings.ScriptEventException(Script, "RPC", Exception);
        }
        finally
        {
            bArrivingCall = false;
            CallerId = PriorCaller;
            --RemoteDepth;
        }
    }

    [ManagedExport]
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvStdcall) })]
    public static void NetDispatchConnection(IntPtr Handle, uint ConnectionId, int Joined)
    {
        if (GCHandle.FromIntPtr(Handle).Target is not EntityScript Script || Script is not INetworkListener Listener)
        {
            return;
        }

        using var Scope = Game.Push(Script.World, Script.Entity, Script);
        try
        {
            if (Joined != 0)
            {
                Listener.OnConnected(new Connection(ConnectionId));
            }
            else
            {
                Listener.OnDisconnected(new Connection(ConnectionId));
            }
        }
        catch (Exception Exception)
        {
            NativeBindings.ScriptEventException(Script, Joined != 0 ? "OnConnected" : "OnDisconnected", Exception);
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

        using var Scope = Game.Push(Script.World, Script.Entity, Script);
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
