using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using LuminaSharp;

namespace Lumina;

// Typed sending and listening over the world's native message bus, so C++ and C# send to and hear each other.
public unsafe partial class CMessageSubsystem
{
    public IDisposable Subscribe<T>(HierarchicalTag Channel, Action<T> Handler, HierarchicalTagMatch Match = HierarchicalTagMatch.Partial)
        => Listen<T>(FEntity.Null, Channel, Message => { Handler(Message); return false; }, Match);

    public IDisposable Subscribe<T>(string Channel, Action<T> Handler, HierarchicalTagMatch Match = HierarchicalTagMatch.Partial)
        => Subscribe(HierarchicalTag.Request(Channel), Handler, Match);

    public IDisposable SubscribeOnce<T>(HierarchicalTag Channel, Action<T> Handler, HierarchicalTagMatch Match = HierarchicalTagMatch.Partial)
    {
        IDisposable? Subscription = null;
        Subscription = Subscribe<T>(Channel, Message => { Subscription?.Dispose(); Handler(Message); }, Match);
        return Subscription;
    }

    public IDisposable SubscribeOnce<T>(string Channel, Action<T> Handler, HierarchicalTagMatch Match = HierarchicalTagMatch.Partial)
        => SubscribeOnce(HierarchicalTag.Request(Channel), Handler, Match);

    // Hears routed sends that reach Owner, and the bus drops it when the entity dies.
    public IDisposable Subscribe<T>(FEntity Owner, HierarchicalTag Channel, Action<T> Handler, HierarchicalTagMatch Match = HierarchicalTagMatch.Partial)
        => Owner.IsNull ? GameplayMessageSubscription.Empty : Listen<T>(Owner, Channel, Message => { Handler(Message); return false; }, Match);

    public IDisposable Subscribe<T>(FEntity Owner, string Channel, Action<T> Handler, HierarchicalTagMatch Match = HierarchicalTagMatch.Partial)
        => Subscribe(Owner, HierarchicalTag.Request(Channel), Handler, Match);

    // Returning true marks the message handled, so no further ancestor or descendant receives it.
    public IDisposable Subscribe<T>(FEntity Owner, HierarchicalTag Channel, Func<T, bool> Handler, HierarchicalTagMatch Match = HierarchicalTagMatch.Partial)
        => Owner.IsNull ? GameplayMessageSubscription.Empty : Listen(Owner, Channel, Handler, Match);

    public IDisposable Subscribe<T>(FEntity Owner, string Channel, Func<T, bool> Handler, HierarchicalTagMatch Match = HierarchicalTagMatch.Partial)
        => Subscribe(Owner, HierarchicalTag.Request(Channel), Handler, Match);

    // Partial listeners on the channel's ancestors hear it too.
    public void Broadcast<T>(HierarchicalTag Channel, T Message) => SendMessage(MessageRoute.Broadcast, FEntity.Null, Channel, Message, true);

    public void Broadcast<T>(string Channel, T Message) => Broadcast(HierarchicalTag.Request(Channel), Message);

    public void SendUp<T>(FEntity Source, HierarchicalTag Channel, T Message, bool IncludeSelf = true)
        => SendMessage(MessageRoute.Up, Source, Channel, Message, IncludeSelf);

    public void SendUp<T>(FEntity Source, string Channel, T Message, bool IncludeSelf = true)
        => SendUp(Source, HierarchicalTag.Request(Channel), Message, IncludeSelf);

    public void SendDown<T>(FEntity Source, HierarchicalTag Channel, T Message, bool IncludeSelf = true)
        => SendMessage(MessageRoute.Down, Source, Channel, Message, IncludeSelf);

    public void SendDown<T>(FEntity Source, string Channel, T Message, bool IncludeSelf = true)
        => SendDown(Source, HierarchicalTag.Request(Channel), Message, IncludeSelf);

    public void SendTo<T>(FEntity Target, HierarchicalTag Channel, T Message) => SendMessage(MessageRoute.To, Target, Channel, Message, true);

    public void SendTo<T>(FEntity Target, string Channel, T Message) => SendTo(Target, HierarchicalTag.Request(Channel), Message);

    private IDisposable Listen<T>(FEntity Owner, HierarchicalTag Channel, Func<T, bool> Handler, HierarchicalTagMatch Match)
    {
        if (!Channel.IsValid || !IsValid)
        {
            return GameplayMessageSubscription.Empty;
        }

        // Handed to the native bus, which releases it when the listener goes.
        ulong Id = Subscribe(Channel.Id, Owner, MessageType<T>.Key, Match == HierarchicalTagMatch.Exact,
                                             ScriptCallback.Bind(new MessageInvoker<T>(Handler)));
        return Id == 0 ? GameplayMessageSubscription.Empty : new GameplayMessageSubscription(GetWorld(), Id);
    }

    private void SendMessage<T>(MessageRoute Route, FEntity Target, HierarchicalTag Channel, T Message, bool IncludeSelf)
    {
        if (!Channel.IsValid || (Route != MessageRoute.Broadcast && Target.IsNull) || Message is null || !IsValid)
        {
            return;
        }

        switch (MessageType<T>.Kind)
        {
            case PayloadKind.Bytes:
            {
                T Copy = Message;
                Send((uint)Route, Target, Channel.Id, (uint)PayloadKind.Bytes, MessageType<T>.Key,
                                    (IntPtr)Unsafe.AsPointer(ref Copy), (uint)Unsafe.SizeOf<T>(), IntPtr.Zero, IncludeSelf);
                break;
            }
            case PayloadKind.Struct:
            {
                T Copy = Message;
                Send((uint)Route, Target, Channel.Id, (uint)PayloadKind.Struct, MessageType<T>.Key,
                                    Unsafe.As<T, NativeRef>(ref Copy).Address, 0, MessageType<T>.Struct, IncludeSelf);
                break;
            }
            default:
            {
                // The native bus frees the handle once the message is delivered.
                GCHandle Payload = GCHandle.Alloc(Message);
                Send((uint)Route, Target, Channel.Id, (uint)PayloadKind.Managed, MessageType.ManagedKey,
                                    GCHandle.ToIntPtr(Payload), 0, IntPtr.Zero, IncludeSelf);
                break;
            }
        }
    }
}
