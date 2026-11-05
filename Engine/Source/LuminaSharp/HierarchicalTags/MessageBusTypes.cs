using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

public enum HierarchicalTagMatch
{
    // The channel and every descendant, so a listener on "Damage" hears "Damage.Fire".
    Partial,
    Exact,
}

// Which way a send travels, mirroring EGameplayMessageRoute.
internal enum MessageRoute : uint
{
    Broadcast,
    Up,
    Down,
    To,
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

        // Checked first, since a struct handle is also an unmanaged value and would otherwise send its own bytes.
        if (NativeName != null && Type.IsValueType && typeof(INativeStruct).IsAssignableFrom(Type))
        {
            Kind = PayloadKind.Struct;
            Key = CMessageSubsystem.GetMessageTypeKey(NativeName);
            Struct = Native.FindStructByName(NativeName);
            Wrap = Pointer =>
            {
                NativeRef Ref = new(Pointer);
                return Unsafe.As<NativeRef, T>(ref Ref);
            };
        }
        else if (Type.IsValueType && !RuntimeHelpers.IsReferenceOrContainsReferences<T>())
        {
            // A generated mirror keys on its native name, so C++ listeners of that struct hear it.
            Kind = PayloadKind.Bytes;
            Key = CMessageSubsystem.GetMessageTypeKey(NativeName ?? "cs:" + Type.FullName);
        }
        else
        {
            Kind = PayloadKind.Managed;
            Key = MessageType.ManagedKey;
        }
    }
}

internal abstract unsafe class MessageInvoker : ScriptClosure
{
    public abstract bool Invoke(GameplayMessageData* Message);

    public sealed override ulong Invoke(ulong Payload)
    {
        // One throwing listener must not starve the rest, nor surface inside the sender.
        try
        {
            return Invoke((GameplayMessageData*)Payload) ? 1u : 0u;
        }
        catch (Exception Exception)
        {
            Interop.LogException(Exception, "message bus listener");
            return 0;
        }
    }
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
            World.MessageBus.Unsubscribe(Id);
        }
        Id = 0;
    }
}
