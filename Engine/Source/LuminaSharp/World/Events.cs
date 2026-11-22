using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Lumina;

namespace LuminaSharp;

// Marks a struct as an event, sent into a world's queue and read there by any system or script that asks for it.
public interface IEvent
{
}

// How far one reader has read a queue. Each reader keeps its own, so every reader sees each event exactly once.
public struct EventReader<T> where T : struct, IEvent
{
    internal ulong Cursor;
}

// The unread events of one queue as spans over its native memory, valid until the next sync point.
public readonly ref struct EventSpan<T> where T : unmanaged, IEvent
{
    public readonly ReadOnlySpan<T> First;
    public readonly ReadOnlySpan<T> Second;

    internal EventSpan(ReadOnlySpan<T> InFirst, ReadOnlySpan<T> InSecond)
    {
        First = InFirst;
        Second = InSecond;
    }

    public int Count => First.Length + Second.Length;
    public bool IsEmpty => Count == 0;

    public Enumerator GetEnumerator() => new(First, Second);

    public ref struct Enumerator
    {
        private readonly ReadOnlySpan<T> First;
        private readonly ReadOnlySpan<T> Second;
        private int Index;

        internal Enumerator(ReadOnlySpan<T> InFirst, ReadOnlySpan<T> InSecond)
        {
            First = InFirst;
            Second = InSecond;
            Index = -1;
        }

        public bool MoveNext() => ++Index < First.Length + Second.Length;

        public ref readonly T Current => ref Index < First.Length ? ref First[Index] : ref Second[Index - First.Length];
    }
}

// The unread events of a queue whose events own native memory, each a view that reads the queue in place.
public readonly ref struct EventViews<T> where T : struct, INativeStruct<T>, IEvent
{
    private readonly EventRuns Runs;

    internal EventViews(EventRuns InRuns)
    {
        Runs = InRuns;
    }

    public int Count => (int)(Runs.First.Count + Runs.Second.Count);
    public bool IsEmpty => Count == 0;

    public Enumerator GetEnumerator() => new(Runs);

    public ref struct Enumerator
    {
        private readonly EventRuns Runs;
        private int Index;

        internal Enumerator(EventRuns InRuns)
        {
            Runs = InRuns;
            Index = -1;
        }

        public bool MoveNext() => ++Index < Runs.First.Count + Runs.Second.Count;

        public T Current
        {
            get
            {
                EventRun Run = Index < Runs.First.Count ? Runs.First : Runs.Second;
                int InRun = Index < Runs.First.Count ? Index : Index - (int)Runs.First.Count;
                NativeRef Ref = new(Run.Data + InRun * (nint)Runs.Stride);
                return Unsafe.As<NativeRef, T>(ref Ref);
            }
        }
    }
}

// Mirrors ECS::FEventRuns, which EventQueueLibrary.cpp checks the size and offsets of.
[StructLayout(LayoutKind.Sequential)]
internal struct EventRun
{
    public IntPtr Data;
    public uint Count;
}

[StructLayout(LayoutKind.Sequential)]
internal struct EventRuns
{
    public EventRun First;
    public EventRun Second;
    public uint Stride;
}

internal static class EventTypes
{
    [StructLayout(LayoutKind.Sequential)]
    private struct AlignmentProbe<T>
    {
        public byte Lead;
        public T Value;
    }

    // An engine event goes by its native name, and a script event by its own, laid out from the C# struct.
    internal static (uint Id, string Name, bool bView) Resolve<T>()
    {
        Type Type = typeof(T);
        string? NativeName = Type.GetCustomAttribute<NativeTypeAttribute>()?.Name;
        string Name = NativeName ?? "cs:" + Type.FullName;
        bool bView = NativeName != null && typeof(INativeStruct).IsAssignableFrom(Type);

        int Size = Unsafe.SizeOf<T>();
        int Alignment = Unsafe.SizeOf<AlignmentProbe<T>>() - Size;
        uint Id = CMessageSubsystem.ResolveEventType(Name, (uint)Size, (uint)Math.Max(Alignment, 1));
        return (Id, Name, bView);
    }
}

// Resolved once per event type, so a send or read crosses the boundary with an id rather than a name.
internal static class EventType<T> where T : struct, IEvent
{
    public static readonly uint Id;
    public static readonly string Name;
    public static readonly bool bView;

    static EventType()
    {
        (Id, Name, bView) = EventTypes.Resolve<T>();
        if (typeof(INetMessage).IsAssignableFrom(typeof(T)) && Name.StartsWith("cs:", StringComparison.Ordinal))
        {
            NetMessageLayout.Register<T>(Id);
        }
    }
}

// A world's event queues, the same ones its C++ systems send to and read.
public readonly unsafe struct WorldEvents
{
    private readonly CWorld? World;

    internal WorldEvents(CWorld InWorld)
    {
        World = InWorld;
    }

    private bool IsLive => World is not null && World.IsValid;

    // Readable after the next sync point, by every system and script that reads T.
    public void Send<T>(in T Event) where T : unmanaged, IEvent
    {
        RequireValueEvent<T>();
        if (IsLive)
        {
            fixed (T* Pointer = &Event)
            {
                World!.MessageBus.SendEvents(EventType<T>.Id, (IntPtr)Pointer, 1);
            }
        }
    }

    public void Send<T>(ReadOnlySpan<T> Events) where T : unmanaged, IEvent
    {
        RequireValueEvent<T>();
        if (IsLive && !Events.IsEmpty)
        {
            fixed (T* Pointer = Events)
            {
                World!.MessageBus.SendEvents(EventType<T>.Id, (IntPtr)Pointer, (uint)Events.Length);
            }
        }
    }

    // Every T sent since Reader last read, oldest first, straight from native memory.
    public EventSpan<T> Read<T>(ref EventReader<T> Reader) where T : unmanaged, IEvent
    {
        RequireValueEvent<T>();
        EventRuns Runs = ReadRuns(EventType<T>.Id, ref Reader.Cursor);
        return new EventSpan<T>(new ReadOnlySpan<T>((void*)Runs.First.Data, (int)Runs.First.Count),
                                new ReadOnlySpan<T>((void*)Runs.Second.Data, (int)Runs.Second.Count));
    }

    // Read for an event that holds strings or object references, each one a view over the queued native value.
    public EventViews<T> ReadViews<T>(ref EventReader<T> Reader) where T : struct, INativeStruct<T>, IEvent
    {
        return new EventViews<T>(ReadRuns(EventType<T>.Id, ref Reader.Cursor));
    }

    public bool HasUnread<T>(in EventReader<T> Reader) where T : struct, IEvent
        => IsLive && World!.MessageBus.NumUnreadEvents(EventType<T>.Id, Reader.Cursor) != 0;

    private EventRuns ReadRuns(uint Id, ref ulong Cursor)
    {
        EventRuns Runs = default;
        if (IsLive)
        {
            fixed (ulong* CursorPointer = &Cursor)
            {
                World!.MessageBus.ReadEvents(Id, (IntPtr)CursorPointer, (IntPtr)(&Runs));
            }
        }
        return Runs;
    }

    // A view over native memory would send or read its own pointer as the event, so those go through ReadViews.
    private static void RequireValueEvent<T>() where T : unmanaged, IEvent
    {
        if (EventType<T>.bView)
        {
            throw new InvalidOperationException($"{typeof(T).Name} holds native memory, so read it with ReadViews and send it from C++.");
        }
    }
}
