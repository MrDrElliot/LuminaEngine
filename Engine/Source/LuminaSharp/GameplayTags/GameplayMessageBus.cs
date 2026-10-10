using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

public enum GameplayTagMatch
{
    // The channel and every descendant, so a listener on "Damage" hears "Damage.Fire".
    Partial,
    Exact,
}

// A view over the native FGameplayMessageBus the world owns, so C++ and C# send to and hear each other.
public readonly unsafe struct GameplayMessageBus
{
    private readonly CWorld? World;

    internal GameplayMessageBus(CWorld World)
    {
        this.World = World;
    }

    public IDisposable Subscribe<T>(GameplayTag Channel, Action<T> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
        => Listen<T>(Entity.Null, Channel, Message => { Handler(Message); return false; }, Match);

    public IDisposable Subscribe<T>(string Channel, Action<T> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
        => Subscribe(GameplayTag.Request(Channel), Handler, Match);

    public IDisposable SubscribeOnce<T>(GameplayTag Channel, Action<T> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
    {
        IDisposable? Subscription = null;
        Subscription = Subscribe<T>(Channel, Message => { Subscription?.Dispose(); Handler(Message); }, Match);
        return Subscription;
    }

    public IDisposable SubscribeOnce<T>(string Channel, Action<T> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
        => SubscribeOnce(GameplayTag.Request(Channel), Handler, Match);

    // Hears routed sends that reach Owner, and the bus drops it when the entity dies.
    public IDisposable Subscribe<T>(Entity Owner, GameplayTag Channel, Action<T> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
        => Owner.IsNull ? GameplayMessageSubscription.Empty : Listen<T>(Owner, Channel, Message => { Handler(Message); return false; }, Match);

    public IDisposable Subscribe<T>(Entity Owner, string Channel, Action<T> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
        => Subscribe(Owner, GameplayTag.Request(Channel), Handler, Match);

    // Returning true marks the message handled, so no further ancestor or descendant receives it.
    public IDisposable Subscribe<T>(Entity Owner, GameplayTag Channel, Func<T, bool> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
        => Owner.IsNull ? GameplayMessageSubscription.Empty : Listen(Owner, Channel, Handler, Match);

    public IDisposable Subscribe<T>(Entity Owner, string Channel, Func<T, bool> Handler, GameplayTagMatch Match = GameplayTagMatch.Partial)
        => Subscribe(Owner, GameplayTag.Request(Channel), Handler, Match);

    public void UnsubscribeAll(Entity Owner)
    {
        if (!Owner.IsNull && IsLive)
        {
            Native.MessageBusUnsubscribeAll(World!.WorldHandle, Owner.Id);
        }
    }

    // Partial listeners on the channel's ancestors hear it too.
    public void Broadcast<T>(GameplayTag Channel, T Message) => Send(Route.Broadcast, Entity.Null, Channel, Message, true);

    public void Broadcast<T>(string Channel, T Message) => Broadcast(GameplayTag.Request(Channel), Message);

    public void SendUp<T>(Entity Source, GameplayTag Channel, T Message, bool IncludeSelf = true)
        => Send(Route.Up, Source, Channel, Message, IncludeSelf);

    public void SendUp<T>(Entity Source, string Channel, T Message, bool IncludeSelf = true)
        => SendUp(Source, GameplayTag.Request(Channel), Message, IncludeSelf);

    public void SendDown<T>(Entity Source, GameplayTag Channel, T Message, bool IncludeSelf = true)
        => Send(Route.Down, Source, Channel, Message, IncludeSelf);

    public void SendDown<T>(Entity Source, string Channel, T Message, bool IncludeSelf = true)
        => SendDown(Source, GameplayTag.Request(Channel), Message, IncludeSelf);

    public void SendTo<T>(Entity Target, GameplayTag Channel, T Message) => Send(Route.To, Target, Channel, Message, true);

    public void SendTo<T>(Entity Target, string Channel, T Message) => SendTo(Target, GameplayTag.Request(Channel), Message);

    private bool IsLive => World is { IsValid: true };

    private IDisposable Listen<T>(Entity Owner, GameplayTag Channel, Func<T, bool> Handler, GameplayTagMatch Match)
    {
        if (!Channel.IsValid || !IsLive)
        {
            return GameplayMessageSubscription.Empty;
        }

        // Handed to the native bus, which frees it when the listener goes.
        GCHandle Handle = GCHandle.Alloc(new MessageInvoker<T>(Handler));
        ulong Id = Native.MessageBusSubscribe(World!.WorldHandle, Channel.Id, Owner.Id, MessageType<T>.Key, (uint)Match,
                                              MessageInvoker.ThunkPtr, GCHandle.ToIntPtr(Handle));
        return Id == 0 ? GameplayMessageSubscription.Empty : new GameplayMessageSubscription(World, Id);
    }

    private void Send<T>(Route Route, Entity Target, GameplayTag Channel, T Message, bool IncludeSelf)
    {
        if (!Channel.IsValid || (Route != Route.Broadcast && Target.IsNull) || Message is null || !IsLive)
        {
            return;
        }

        ulong Handle = World!.WorldHandle;
        uint Include = IncludeSelf ? 1u : 0u;
        switch (MessageType<T>.Kind)
        {
            case PayloadKind.Bytes:
            {
                T Copy = Message;
                Native.MessageBusSend(Handle, (uint)Route, Target.Id, Channel.Id, (uint)PayloadKind.Bytes, MessageType<T>.Key,
                                      (IntPtr)Unsafe.AsPointer(ref Copy), (uint)Unsafe.SizeOf<T>(), IntPtr.Zero, Include);
                break;
            }
            case PayloadKind.Struct:
            {
                NativeStruct Struct = (NativeStruct)(object)Message;
                Native.MessageBusSend(Handle, (uint)Route, Target.Id, Channel.Id, (uint)PayloadKind.Struct, MessageType<T>.Key,
                                      Struct.Handle, 0, MessageType<T>.Struct, Include);
                break;
            }
            default:
            {
                // The native bus frees the handle once the message is delivered.
                GCHandle Payload = GCHandle.Alloc(Message);
                Native.MessageBusSend(Handle, (uint)Route, Target.Id, Channel.Id, (uint)PayloadKind.Managed, MessageType.ManagedKey,
                                      GCHandle.ToIntPtr(Payload), 0, IntPtr.Zero, Include);
                break;
            }
        }
    }

    private enum Route : uint
    {
        Broadcast,
        Up,
        Down,
        To,
    }
}

// Mirrors EGameplayMessagePayload.
internal enum PayloadKind : uint
{
    None,
    Struct,
    Bytes,
    Managed,
}

// Mirrors FGameplayMessage.
[StructLayout(LayoutKind.Sequential)]
internal struct GameplayMessageData
{
    public ulong Type;
    public IntPtr Payload;
    public uint Size;
    public PayloadKind Kind;
    public uint Channel;
    public uint Source;
    public uint Target;
    public uint Pad;
}

internal static class MessageType
{
    // Matches kManagedGameplayMessageType.
    public const ulong ManagedKey = 1;
}

// How T crosses to the native bus, worked out once per type.
internal static class MessageType<T>
{
    public static readonly PayloadKind Kind;
    public static readonly ulong Key;
    public static readonly IntPtr Struct;
    public static readonly Func<IntPtr, T>? Wrap;

    static MessageType()
    {
        Type Type = typeof(T);
        string? NativeName = Type.GetCustomAttribute<NativeTypeAttribute>()?.Name;

        if (Type.IsValueType && !RuntimeHelpers.IsReferenceOrContainsReferences<T>())
        {
            // A generated mirror keys on its native name, so C++ listeners of that struct hear it.
            Kind = PayloadKind.Bytes;
            Key = Native.MessageBusTypeKey(NativeName ?? "cs:" + Type.FullName);
        }
        else if (NativeName != null && typeof(NativeStruct).IsAssignableFrom(Type))
        {
            Kind = PayloadKind.Struct;
            Key = Native.MessageBusTypeKey(NativeName);
            Struct = Native.FindStructByName(NativeName);
            Wrap = Pointer => (T)Activator.CreateInstance(Type, Pointer)!;
        }
        else
        {
            Kind = PayloadKind.Managed;
            Key = MessageType.ManagedKey;
        }
    }
}

internal abstract unsafe class MessageInvoker
{
    public abstract bool Invoke(GameplayMessageData* Message);

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static uint Thunk(IntPtr Context, GameplayMessageData* Message)
    {
        // One throwing listener must not starve the rest, nor surface inside the sender.
        try
        {
            return GCHandle.FromIntPtr(Context).Target is MessageInvoker Invoker && Invoker.Invoke(Message) ? 1u : 0u;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception, "message bus listener");
            return 0;
        }
    }

    public static readonly IntPtr ThunkPtr = (IntPtr)(delegate* unmanaged[Cdecl]<IntPtr, GameplayMessageData*, uint>)&Thunk;
}

internal sealed unsafe class MessageInvoker<T> : MessageInvoker
{
    private readonly Func<T, bool> Handler;

    public MessageInvoker(Func<T, bool> Handler)
    {
        this.Handler = Handler;
    }

    public override bool Invoke(GameplayMessageData* Message)
    {
        if (Message->Payload == IntPtr.Zero)
        {
            return false;
        }

        switch (MessageType<T>.Kind)
        {
            case PayloadKind.Bytes:
                return Message->Size >= Unsafe.SizeOf<T>() && Handler(Unsafe.ReadUnaligned<T>((void*)Message->Payload));
            case PayloadKind.Struct:
                return Handler(MessageType<T>.Wrap!(Message->Payload));
            default:
                return GCHandle.FromIntPtr(Message->Payload).Target is T Value && Handler(Value);
        }
    }
}

public sealed class GameplayMessageSubscription : IDisposable
{
    internal static readonly GameplayMessageSubscription Empty = new(null, 0);

    private readonly CWorld? World;
    private ulong Id;

    internal GameplayMessageSubscription(CWorld? World, ulong Id)
    {
        this.World = World;
        this.Id = Id;
    }

    public bool IsActive => Id != 0 && World is { IsValid: true };

    public void Dispose()
    {
        // A world that is already gone took its listeners with it.
        if (Id != 0 && World is { IsValid: true })
        {
            Native.MessageBusUnsubscribe(World.WorldHandle, Id);
        }
        Id = 0;
    }
}
