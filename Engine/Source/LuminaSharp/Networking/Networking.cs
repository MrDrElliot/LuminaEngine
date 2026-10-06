using System;
using System.Collections.Generic;

namespace LuminaSharp;

// The host is always connection 0, which is also what a standalone world answers as.
public readonly struct Connection : IEquatable<Connection>
{
    public readonly uint Id;

    public Connection(uint Id)
    {
        this.Id = Id;
    }

    public static Connection Host => default;

    public static Connection Local => new(Networking.LocalConnectionId(Game.World));

    public bool IsHost => Id == 0;

    public bool IsLocal => Id == Networking.LocalConnectionId(Game.World);

    public bool Equals(Connection Other) => Id == Other.Id;
    public override bool Equals(object? Obj) => Obj is Connection Other && Equals(Other);
    public override int GetHashCode() => (int)Id;
    public static bool operator ==(Connection A, Connection B) => A.Id == B.Id;
    public static bool operator !=(Connection A, Connection B) => A.Id != B.Id;
    public override string ToString() => IsHost ? "Host" : $"Connection {Id}";
}

// The session of the current world. A standalone world counts as its own host.
public static class Networking
{
    public static bool IsActive => Game.World.IsNetworked;

    public static bool IsHost => !Game.World.IsClient;

    public static bool IsClient => Game.World.IsClient;

    public static bool IsDedicatedServer => Game.World.GetNetMode() == Lumina.ENetMode.DedicatedServer;

    // A load-test client from -bots, which gameplay should drive itself and never draw for.
    public static bool IsBot => Game.World.IsBotWorld();

    // Remote connections that finished joining, which a client has no view of.
    public static IReadOnlyList<Connection> Connections => GetConnections(Game.World);

    internal static uint LocalConnectionId(Lumina.CWorld World)
    {
        return World.IsClient ? NetNative.GetLocalConnection(World.WorldHandle) : 0u;
    }

    internal static unsafe IReadOnlyList<Connection> GetConnections(Lumina.CWorld World)
    {
        if (!World.IsServer)
        {
            return Array.Empty<Connection>();
        }

        Span<uint> Scratch = stackalloc uint[64];
        int Total;
        fixed (uint* Ids = Scratch)
        {
            Total = NetNative.GetConnections(World.WorldHandle, (IntPtr)Ids, Scratch.Length);
        }

        uint[] Buffer;
        if (Total > Scratch.Length)
        {
            Buffer = new uint[Total];
            fixed (uint* Ids = Buffer)
            {
                Total = Math.Min(Total, NetNative.GetConnections(World.WorldHandle, (IntPtr)Ids, Buffer.Length));
            }
        }
        else
        {
            Buffer = Scratch.Slice(0, Total).ToArray();
        }

        var Result = new Connection[Total];
        for (int Index = 0; Index < Total; ++Index)
        {
            Result[Index] = new Connection(Buffer[Index]);
        }
        return Result;
    }
}

// Who controls an entity, and whether this peer does.
public readonly struct EntityNetwork
{
    private readonly Lumina.CWorld World;
    private readonly Entity Entity;

    public EntityNetwork(Lumina.CWorld World, Entity Entity)
    {
        this.World = World;
        this.Entity = Entity;
    }

    public static EntityNetwork Of(Entity Entity) => new(Game.World, Entity);

    // Needs an SNetworkComponent on the entity.
    public bool IsNetworked => World.IsNetworked && NetNative.IsEntityNetworked(World.WorldHandle, Entity.Id) != 0;

    public Connection Owner => new(NetNative.GetOwner(World.WorldHandle, Entity.Id));

    // Always true in a standalone world, where nothing else could control it.
    public bool IsOwner => !IsNetworked || Owner.Id == Networking.LocalConnectionId(World);

    // Another peer controls the entity, so this one only mirrors it.
    public bool IsProxy => IsNetworked && !IsOwner;

    // Host only, a client asking is refused.
    public bool AssignOwnership(Connection Connection) => NetNative.SetOwner(World.WorldHandle, Entity.Id, Connection.Id) != 0;

    public bool DropOwnership() => AssignOwnership(Connection.Host);

    public uint NetId => NetNative.EntityToNetId(World.WorldHandle, Entity.Id);

    // Resends replicated component fields. A [Sync] field marks itself when written.
    public void MarkDirty() => NetNative.MarkEntityDirty(World.WorldHandle, Entity.Id);
}

// Called on the host as remote connections join and leave.
public interface INetworkListener
{
    void OnConnected(Connection Connection) { }

    void OnDisconnected(Connection Connection) { }
}

// Replicated from the host to every client. A client's own write stays local until the host's next value lands.
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property, AllowMultiple = false)]
public sealed class SyncAttribute : Attribute
{
}

// The named method runs when a [Sync] field arrives with a new value, taking nothing or the old and new values.
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property, AllowMultiple = false)]
public sealed class ChangeAttribute : Attribute
{
    public ChangeAttribute(string Method)
    {
        this.Method = Method;
    }

    public string Method { get; }
}

[Flags]
public enum NetFlags : uint
{
    None = 0,

    // May be dropped, for frequent cosmetic calls.
    Unreliable = 1u << 0,

    // Ignored on arrival unless the caller owns the entity or is the host.
    OwnerOnly = 1u << 1,
}

public enum ERpcTarget : uint
{
    Broadcast = 0,
    Host = 1,
    Owner = 2,
}

// Mark a void method on an EntityScript with one of these and calling it routes the call.
public static class Rpc
{
    public abstract class RpcAttribute : Attribute
    {
        protected RpcAttribute(NetFlags Flags)
        {
            this.Flags = Flags;
        }

        public NetFlags Flags { get; }
    }

    // Runs on the host and every client.
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false)]
    public sealed class BroadcastAttribute : RpcAttribute
    {
        public BroadcastAttribute(NetFlags Flags = NetFlags.None) : base(Flags) { }
    }

    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false)]
    public sealed class HostAttribute : RpcAttribute
    {
        public HostAttribute(NetFlags Flags = NetFlags.None) : base(Flags) { }
    }

    // Runs on whichever peer owns the entity.
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false)]
    public sealed class OwnerAttribute : RpcAttribute
    {
        public OwnerAttribute(NetFlags Flags = NetFlags.None) : base(Flags) { }
    }

    // The local connection when the call did not arrive from the network.
    public static Connection Caller => RpcRuntime.CurrentCaller();

    public static bool IsRemote => RpcRuntime.RemoteDepth > 0;
}
