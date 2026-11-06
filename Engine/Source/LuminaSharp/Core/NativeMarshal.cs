using System;
using System.Runtime.CompilerServices;

namespace LuminaSharp;

// Readers over a native FString or TVector in place, whose views alias native storage and must not be retained.
public static unsafe class NativeMarshal
{
    // Lumina::FString: 16-byte SSO/heap union, last byte = mode (top bit set => heap, ptr@0 size@8 as uint32).
    private const int FStringFlagOffset = 15;
    private const int FStringSSOCapacity = 15;

    /// <summary>Transcodes a native FString to a managed string in place (no crossing); decodes the SSO/heap union.</summary>
    public static string ReadString(nint FStringPtr)
    {
        if (FStringPtr == 0)
        {
            return string.Empty;
        }

        byte* Base = (byte*)FStringPtr;
        byte Flag = Base[FStringFlagOffset];
        byte* Data;
        long Length;
        if ((Flag & 0x80) != 0)
        {
            Data = *(byte**)Base;                   // heap.Data
            Length = *(uint*)(Base + 8);            // heap.Size (uint32)
        }
        else
        {
            Data = Base;                            // the inline buffer starts at offset zero
            Length = FStringSSOCapacity - Flag;     // the mode byte counts the slack, not the length
        }

        return Interop.GetString(Data, (int)Math.Min(Length, int.MaxValue));
    }

    /// <summary>A zero-copy span over a native TVector&lt;T&gt; embedded at Container+Offset (blittable element).</summary>
    public static ReadOnlySpan<T> ReadVector<T>(nint Container, nint Offset) where T : unmanaged
    {
        return DecodeVector<T>((byte*)NativeBindings.FieldAt(Container, Offset));
    }

    // The single source of truth for the TVector layout (Data@0, Count@8 as uint32); shared by ReadVector and TVector.
    internal static Span<T> DecodeVector<T>(byte* Header) where T : unmanaged
    {
        DecodeVectorRaw(Header, out byte* Data, out int Count);
        return Count == 0 ? Span<T>.Empty : new Span<T>(Data, Count);
    }

    // Only a real TVector counts elements here, so a script array takes its length from the ops table instead.
    internal static void DecodeVectorRaw(byte* Header, out byte* Data, out int Count)
    {
        Data = null;
        Count = 0;
        if (Header == null)
        {
            return;
        }
        byte* Begin = *(byte**)Header;
        uint Elements = *(uint*)(Header + sizeof(void*));
        if (Begin == null || Elements == 0)
        {
            return;
        }
        Data = Begin;
        Count = (int)Elements;
    }
}
