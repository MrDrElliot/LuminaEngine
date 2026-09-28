using System;
using System.Buffers;
using System.Collections.Generic;
using System.Threading;

namespace LuminaSharp;

/// <summary>How a listener's channel is matched against a broadcast tag.</summary>
public enum GameplayTagMatch
{
    /// <summary>Receive this exact tag AND any descendant (a listener on "Damage" hears "Damage.Fire"). Default.</summary>
    Partial,

    /// <summary>Receive only broadcasts on this exact tag.</summary>
    Exact,
}

/// <summary>
/// A world's gameplay event bus (<c>World.Messages</c>): typed messages on hierarchical <see cref="GameplayTag"/>
/// channels, per-world isolated, game thread only. Four delivery directions share one channel space:
/// <list type="bullet">
/// <item><b>Global</b>, <see cref="Broadcast{T}(GameplayTag, T)"/> reaches every listener on the channel (and its
/// ancestor channels for Partial listeners), anywhere in the world. Fire-and-forget.</item>
/// <item><b>Up / Down / To</b>, <see cref="SendUp{T}"/> / <see cref="SendDown{T}"/> route along a source entity's
/// scene-graph chain; <see cref="SendTo{T}"/> targets one entity. Entity-scoped listeners receive these; a listener
/// subscribed with a <c>Func&lt;T, bool&gt;</c> handler returns <c>true</c> to halt propagation along the route.</item>
/// </list>
/// Dispatch is allocation-free (pooled snapshot), reentrancy-safe (handlers may (un)subscribe mid-dispatch), and
/// non-recursive (one batched native crossing fetches the chain/subtree). Subscriptions are cleared on script
/// reload, but a script should still Dispose its subscriptions in OnDetach.
/// </summary>
public readonly unsafe partial struct GameplayMessageBus
{
    // Tag chains beyond this aren't real; ancestors past it are simply not matched.
    private const int MaxTagDepth = 32;

    private readonly ulong World;

    internal GameplayMessageBus(ulong World)
    {
        this.World = World;
    }

    /// <summary>Listen for <typeparamref name="T"/> on <paramref name="Channel"/> globally. Dispose the handle to stop.</summary>
    public IDisposable Subscribe<T>(GameplayTag Channel, Action<T> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
    {
        if (!Channel.IsValid)
        {
            return GameplayMessageSubscription.Empty;
        }
        Listener Entry = new(typeof(T), Message => Handler((T)Message), Match);
        BusRegistry.Of(World).Add(Channel.Id, Entry);
        return new GameplayMessageSubscription(World, Channel.Id, Entry);
    }

    /// <summary>Subscribe by tag name (interns the channel first).</summary>
    public IDisposable Subscribe<T>(string Channel, Action<T> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
        => Subscribe(GameplayTag.Request(Channel), Handler, Match);

    /// <summary>Like <see cref="Subscribe{T}(GameplayTag, Action{T}, GameplayTagMatch)"/>, but auto-unsubscribes after the first matching message.</summary>
    public IDisposable SubscribeOnce<T>(GameplayTag Channel, Action<T> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
    {
        IDisposable? Sub = null;
        Sub = Subscribe<T>(Channel, Message => { Sub?.Dispose(); Handler(Message); }, Match);
        return Sub;
    }

    /// <summary>Subscribe-once by tag name (interns the channel first).</summary>
    public IDisposable SubscribeOnce<T>(string Channel, Action<T> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
        => SubscribeOnce(GameplayTag.Request(Channel), Handler, Match);

    /// <summary>Broadcast globally to listeners on <paramref name="Channel"/> and (for Partial listeners) its ancestors.</summary>
    public void Broadcast<T>(GameplayTag Channel, T Message)
    {
        if (!Channel.IsValid)
        {
            return;
        }
        BusState? State = BusRegistry.Find(World);
        if (State == null)
        {
            return;
        }

        object Boxed = Message!;
        Type MessageType = typeof(T);
        for (GameplayTag Cur = Channel; Cur.IsValid; Cur = Cur.Parent)
        {
            State.Dispatch(Cur.Id, Cur.Id == Channel.Id, MessageType, Boxed);
        }
    }

    /// <summary>Broadcast by tag name (interns the channel first).</summary>
    public void Broadcast<T>(string Channel, T Message) => Broadcast(GameplayTag.Request(Channel), Message);

    /// <summary>Listen for directional messages that reach <paramref name="Owner"/>'s entity. Dispose to stop (e.g. in OnDetach).</summary>
    public IDisposable Subscribe<T>(Entity Owner, GameplayTag Channel, Action<T> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
        => SubscribeEntity<T>(Owner, Channel, Message => { Handler((T)Message); return false; }, Match);

    /// <summary>Entity-scoped subscribe by tag name (interns the channel first).</summary>
    public IDisposable Subscribe<T>(Entity Owner, string Channel, Action<T> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
        => Subscribe(Owner, GameplayTag.Request(Channel), Handler, Match);

    /// <summary>Listen for directional messages on <paramref name="Owner"/> with a handler that returns <c>true</c> to
    /// mark the message handled and halt propagation along the route (no further ancestor / descendant receives it).</summary>
    public IDisposable Subscribe<T>(Entity Owner, GameplayTag Channel, Func<T, bool> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
        => SubscribeEntity<T>(Owner, Channel, Message => Handler((T)Message), Match);

    /// <summary>Routed entity-scoped subscribe by tag name (interns the channel first).</summary>
    public IDisposable Subscribe<T>(Entity Owner, string Channel, Func<T, bool> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
        => Subscribe(Owner, GameplayTag.Request(Channel), Handler, Match);

    private IDisposable SubscribeEntity<T>(Entity Owner, GameplayTag Channel, Func<object, bool> Invoke, GameplayTagMatch Match)
    {
        if (Owner.IsNull || !Channel.IsValid)
        {
            return GameplayMessageSubscription.Empty;
        }
        EntityListener Entry = new(typeof(T), Invoke, Channel.Id, Match);
        BusRegistry.Of(World).AddEntity(Owner.Id, Entry);
        return new GameplayMessageSubscription(World, Owner.Id, Entry);
    }

    /// <summary>Drop all of <paramref name="Owner"/>'s entity-scoped subscriptions in one call (teardown helper).</summary>
    public void UnsubscribeAll(Entity Owner)
    {
        if (!Owner.IsNull)
        {
            BusRegistry.Find(World)?.ClearEntity(Owner.Id);
        }
    }

    /// <summary>Send <paramref name="Message"/> UP the scene graph: <paramref name="Source"/> (when
    /// <paramref name="IncludeSelf"/>) then each ancestor to the root, halting if a handler returns true.</summary>
    public void SendUp<T>(Entity Source, GameplayTag Channel, T Message, bool IncludeSelf = true)
        => SendChain(GetAncestorChainFn, Source, Channel, Message, IncludeSelf);

    /// <summary>Send up the scene graph by tag name (interns the channel first).</summary>
    public void SendUp<T>(Entity Source, string Channel, T Message, bool IncludeSelf = true)
        => SendUp(Source, GameplayTag.Request(Channel), Message, IncludeSelf);

    /// <summary>Send <paramref name="Message"/> DOWN the scene graph: <paramref name="Source"/> (when
    /// <paramref name="IncludeSelf"/>) then every descendant, halting if a handler returns true.</summary>
    public void SendDown<T>(Entity Source, GameplayTag Channel, T Message, bool IncludeSelf = true)
        => SendChain(GetSubtreeFn, Source, Channel, Message, IncludeSelf);

    /// <summary>Send down the scene graph by tag name (interns the channel first).</summary>
    public void SendDown<T>(Entity Source, string Channel, T Message, bool IncludeSelf = true)
        => SendDown(Source, GameplayTag.Request(Channel), Message, IncludeSelf);

    /// <summary>Send <paramref name="Message"/> directly to a single <paramref name="Target"/> entity (point-to-point).</summary>
    public void SendTo<T>(Entity Target, GameplayTag Channel, T Message)
    {
        if (Target.IsNull || !Channel.IsValid)
        {
            return;
        }
        BusState? State = BusRegistry.Find(World);
        if (State == null)
        {
            return;
        }

        Span<uint> AncestorBuf = stackalloc uint[MaxTagDepth];
        ReadOnlySpan<uint> Ancestors = AncestorBuf[..WriteAncestors(Channel, AncestorBuf)];
        State.DispatchEntity(Target.Id, Channel.Id, Ancestors, typeof(T), Message!);
    }

    /// <summary>Point-to-point send by tag name (interns the channel first).</summary>
    public void SendTo<T>(Entity Target, string Channel, T Message) => SendTo(Target, GameplayTag.Request(Channel), Message);

    // Shared body for SendUp/SendDown: one batched native crossing flattens the ancestor chain / subtree into a
    // pooled buffer ([Source, ...] in walk order), then dispatch each in managed code, halting when a handler returns true.
    private void SendChain<T>(delegate* unmanaged[Cdecl]<ulong, uint, uint*, int, int> Fetch, Entity Source, GameplayTag Channel, T Message, bool IncludeSelf)
    {
        if (Source.IsNull || !Channel.IsValid || Fetch == null)
        {
            return;
        }
        BusState? State = BusRegistry.Find(World);
        if (State == null)
        {
            return;
        }

        Span<uint> AncestorBuf = stackalloc uint[MaxTagDepth];
        ReadOnlySpan<uint> Ancestors = AncestorBuf[..WriteAncestors(Channel, AncestorBuf)];
        object Boxed = Message!;
        Type MessageType = typeof(T);

        uint[] Nodes = ArrayPool<uint>.Shared.Rent(64);
        try
        {
            int Count = FetchHierarchy(Fetch, World, Source.Id, ref Nodes);
            for (int Index = IncludeSelf ? 0 : 1; Index < Count; Index++)
            {
                if (!State.DispatchEntity(Nodes[Index], Channel.Id, Ancestors, MessageType, Boxed))
                {
                    break;
                }
            }
        }
        finally
        {
            ArrayPool<uint>.Shared.Return(Nodes);
        }
    }

    // Fills Buffer with the native walk result, growing once if the subtree is larger than the rented buffer.
    private static int FetchHierarchy(delegate* unmanaged[Cdecl]<ulong, uint, uint*, int, int> Fetch, ulong World, uint Entity, ref uint[] Buffer)
    {
        int Count;
        fixed (uint* Ptr = Buffer)
        {
            Count = Fetch(World, Entity, Ptr, Buffer.Length);
        }
        if (Count > Buffer.Length)
        {
            ArrayPool<uint>.Shared.Return(Buffer);
            Buffer = ArrayPool<uint>.Shared.Rent(Count);
            fixed (uint* Ptr = Buffer)
            {
                Count = Fetch(World, Entity, Ptr, Buffer.Length);
            }
        }
        return Count;
    }

    // The sent channel's id followed by its ancestor ids (closest first); returns the count written.
    private static int WriteAncestors(GameplayTag Channel, Span<uint> Buffer)
    {
        int Count = 0;
        for (GameplayTag Cur = Channel; Cur.IsValid && Count < Buffer.Length; Cur = Cur.Parent)
        {
            Buffer[Count++] = Cur.Id;
        }
        return Count;
    }

    private static readonly delegate* unmanaged[Cdecl]<ulong, uint, uint*, int, int> GetAncestorChainFn =
        (delegate* unmanaged[Cdecl]<ulong, uint, uint*, int, int>)NativeBindings.Resolve("Runtime", "LuminaSharp_Call_Lumina_CEntityLibrary_GetAncestorChain");

    private static readonly delegate* unmanaged[Cdecl]<ulong, uint, uint*, int, int> GetSubtreeFn =
        (delegate* unmanaged[Cdecl]<ulong, uint, uint*, int, int>)NativeBindings.Resolve("Runtime", "LuminaSharp_Call_Lumina_CEntityLibrary_GetSubtree");
}

/// <summary>One registered global listener: its payload type, the boxed-invoke shim, and its match mode.</summary>
internal sealed class Listener
{
    public readonly Type MessageType;
    public readonly Action<object> Invoke;
    public readonly GameplayTagMatch Match;

    public Listener(Type MessageType, Action<object> Invoke, GameplayTagMatch Match)
    {
        this.MessageType = MessageType;
        this.Invoke = Invoke;
        this.Match = Match;
    }
}

/// <summary>An entity-scoped listener for directional delivery: like <see cref="Listener"/> but it also carries the
/// channel tag (directional sends index by entity, not channel); its invoke returns true to halt the route.</summary>
internal sealed class EntityListener
{
    public readonly Type MessageType;
    public readonly Func<object, bool> Invoke;
    public readonly uint ChannelId;
    public readonly GameplayTagMatch Match;

    public EntityListener(Type MessageType, Func<object, bool> Invoke, uint ChannelId, GameplayTagMatch Match)
    {
        this.MessageType = MessageType;
        this.Invoke = Invoke;
        this.ChannelId = ChannelId;
        this.Match = Match;
    }
}

/// <summary>Per-world listener table, keyed by tag id (global) and entity id (directional). Self-locking; dispatch
/// snapshots into a pooled buffer so a handler may safely (un)subscribe mid-dispatch with no per-dispatch heap alloc.</summary>
internal sealed class BusState
{
    private readonly Lock Gate = new();
    private readonly Dictionary<uint, List<Listener>> ByTag = new();
    private readonly Dictionary<uint, List<EntityListener>> ByEntity = new();

    public void Add(uint TagId, Listener Entry)
    {
        lock (Gate)
        {
            if (!ByTag.TryGetValue(TagId, out List<Listener>? List))
            {
                List = new List<Listener>();
                ByTag[TagId] = List;
            }
            List.Add(Entry);
        }
    }

    public void Remove(uint TagId, Listener Entry)
    {
        lock (Gate)
        {
            if (ByTag.TryGetValue(TagId, out List<Listener>? List) && List.Remove(Entry) && List.Count == 0)
            {
                ByTag.Remove(TagId);
            }
        }
    }

    public void AddEntity(uint EntityId, EntityListener Entry)
    {
        lock (Gate)
        {
            if (!ByEntity.TryGetValue(EntityId, out List<EntityListener>? List))
            {
                List = new List<EntityListener>();
                ByEntity[EntityId] = List;
            }
            List.Add(Entry);
        }
    }

    public void RemoveEntity(uint EntityId, EntityListener Entry)
    {
        lock (Gate)
        {
            if (ByEntity.TryGetValue(EntityId, out List<EntityListener>? List) && List.Remove(Entry) && List.Count == 0)
            {
                ByEntity.Remove(EntityId);
            }
        }
    }

    public void ClearEntity(uint EntityId)
    {
        lock (Gate)
        {
            ByEntity.Remove(EntityId);
        }
    }

    public void Clear()
    {
        lock (Gate)
        {
            ByTag.Clear();
            ByEntity.Clear();
        }
    }

    public void Dispatch(uint TagId, bool IsChannelLevel, Type MessageType, object Boxed)
    {
        int Count;
        Listener[] Buffer;
        lock (Gate)
        {
            if (!ByTag.TryGetValue(TagId, out List<Listener>? List) || List.Count == 0)
            {
                return;
            }
            Count = List.Count;
            Buffer = ArrayPool<Listener>.Shared.Rent(Count);
            List.CopyTo(Buffer);
        }

        try
        {
            for (int Index = 0; Index < Count; Index++)
            {
                Listener Entry = Buffer[Index];
                // IsChannelLevel is true only when this dispatch is on the listener's exact tag (not an ancestor):
                // Exact listeners require it, Partial listeners always fire on the channel or any ancestor.
                bool Reaches = Entry.Match == GameplayTagMatch.Partial || IsChannelLevel;
                if (Reaches && Entry.MessageType.IsAssignableFrom(MessageType))
                {
                    // One throwing listener must not starve the rest, nor surface inside the sender.
                    try
                    {
                        Entry.Invoke(Boxed);
                    }
                    catch (Exception Exception)
                    {
                        Interop.LogException(Exception, "message bus listener");
                    }
                }
            }
        }
        finally
        {
            Array.Clear(Buffer, 0, Count);
            ArrayPool<Listener>.Shared.Return(Buffer);
        }
    }

    // A handler that throws is logged and treated as not having consumed the message.
    private static bool InvokeGuarded(EntityListener Entry, object Boxed)
    {
        try
        {
            return Entry.Invoke(Boxed);
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception, "message bus listener");
            return false;
        }
    }

    /// <summary>Deliver a directional send that reached <paramref name="EntityId"/> to that entity's matching
    /// listeners. Returns false (halt the route) as soon as a handler returns true; true to continue.</summary>
    public bool DispatchEntity(uint EntityId, uint ExactId, ReadOnlySpan<uint> Ancestors, Type MessageType, object Boxed)
    {
        int Count;
        EntityListener[] Buffer;
        lock (Gate)
        {
            if (!ByEntity.TryGetValue(EntityId, out List<EntityListener>? List) || List.Count == 0)
            {
                return true;
            }
            Count = List.Count;
            Buffer = ArrayPool<EntityListener>.Shared.Rent(Count);
            List.CopyTo(Buffer);
        }

        bool Continue = true;
        try
        {
            for (int Index = 0; Index < Count; Index++)
            {
                EntityListener Entry = Buffer[Index];
                bool Reaches = Entry.Match == GameplayTagMatch.Exact ? Entry.ChannelId == ExactId : Ancestors.Contains(Entry.ChannelId);
                if (Reaches && Entry.MessageType.IsAssignableFrom(MessageType) && InvokeGuarded(Entry, Boxed))
                {
                    Continue = false;
                    break;
                }
            }
        }
        finally
        {
            Array.Clear(Buffer, 0, Count);
            ArrayPool<EntityListener>.Shared.Return(Buffer);
        }
        return Continue;
    }
}

/// <summary>Holds one <see cref="BusState"/> per world handle (world-isolated event buses).</summary>
internal static class BusRegistry
{
    private static readonly Lock Gate = new();
    private static readonly Dictionary<ulong, BusState> Worlds = new();

    public static BusState Of(ulong World)
    {
        lock (Gate)
        {
            if (!Worlds.TryGetValue(World, out BusState? State))
            {
                State = new BusState();
                Worlds[World] = State;
            }
            return State;
        }
    }

    public static BusState? Find(ulong World)
    {
        lock (Gate)
        {
            return Worlds.TryGetValue(World, out BusState? State) ? State : null;
        }
    }

    /// <summary>Drop a world's bus when it is torn down (PIE stop), releasing its listener delegates.</summary>
    public static void Remove(ulong World)
    {
        lock (Gate)
        {
            if (Worlds.Remove(World, out BusState? State))
            {
                State.Clear();
            }
        }
    }

    /// <summary>Drop every world's bus. Called before a script ALC unload so no captured script delegate survives
    /// (a strong reference from this non-collectible static would otherwise pin the collectible ALC).</summary>
    public static void ClearAll()
    {
        lock (Gate)
        {
            foreach (BusState State in Worlds.Values)
            {
                State.Clear();
            }
            Worlds.Clear();
        }
    }
}

/// <summary>A live message-bus subscription; dispose to stop listening.</summary>
public sealed class GameplayMessageSubscription : IDisposable
{
    internal static readonly GameplayMessageSubscription Empty = new();

    private readonly ulong World;
    private readonly uint Key; // tag id for a global listener, entity id for an entity-scoped one
    private Listener? Entry;
    private EntityListener? EntityEntry;

    private GameplayMessageSubscription()
    {
    }

    internal GameplayMessageSubscription(ulong World, uint TagId, Listener Entry)
    {
        this.World = World;
        this.Key = TagId;
        this.Entry = Entry;
    }

    internal GameplayMessageSubscription(ulong World, uint EntityId, EntityListener Entry)
    {
        this.World = World;
        this.Key = EntityId;
        this.EntityEntry = Entry;
    }

    public bool IsActive => Entry != null || EntityEntry != null;

    public void Dispose()
    {
        if (Entry != null)
        {
            BusRegistry.Find(World)?.Remove(Key, Entry);
            Entry = null;
        }
        if (EntityEntry != null)
        {
            BusRegistry.Find(World)?.RemoveEntity(Key, EntityEntry);
            EntityEntry = null;
        }
    }
}
