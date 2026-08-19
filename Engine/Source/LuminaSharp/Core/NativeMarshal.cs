using System;
using System.Runtime.CompilerServices;
using System.Text;

namespace LuminaSharp;

// Zero-crossing readers over a native FString / TVector laid out in place (little-endian; layout guarded
// by VerifyContainerInteropLayout). Returned views alias native storage: read synchronously, never retain.
public static unsafe class NativeMarshal
{
    // Lumina::FString: 16-byte SSO/heap union, last byte = mode (top bit set => heap, ptr@0 size@8 as uint32).
    private const int FStringFlagOffset = 15;
    private const int FStringSSOCapacity = 15;

    // Longer than this => treated as a corrupt read, not a multi-gigabyte allocation.
    private const long MaxNativeStringBytes = 64 * 1024 * 1024;

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

        if (Data == null || Length <= 0 || Length >= MaxNativeStringBytes)
        {
            return string.Empty;
        }

        return Encoding.UTF8.GetString(Data, (int)Length);
    }

    /// <summary>A zero-copy span over a native TVector&lt;T&gt; embedded at Container+Offset (blittable element).</summary>
    public static ReadOnlySpan<T> ReadVector<T>(nint Container, nint Offset) where T : unmanaged
    {
        return DecodeVector<T>((byte*)Container + Offset);
    }

    // The single source of truth for the TVector layout (Data@0, Count@8 as uint32); shared by ReadVector and TVector.
    internal static Span<T> DecodeVector<T>(byte* Header) where T : unmanaged
    {
        DecodeVectorRaw(Header, Unsafe.SizeOf<T>(), out byte* Data, out int Count);
        return Count == 0 ? Span<T>.Empty : new Span<T>(Data, Count);
    }

    /// <summary>
    /// The same decode without a type: the element stride comes from the caller (the ops table reports it),
    /// so this works for an element whose NATIVE size is not <c>Unsafe.SizeOf&lt;T&gt;()</c> -- an FString
    /// element is 24 native bytes while its managed handle is not, and an object slot is a bare pointer.
    ///
    /// Reading the header in place is what keeps <see cref="TVector{T}.Count"/> free. Asking the ops table
    /// instead would be a <c>delegate* unmanaged</c> call with a GC transition on every loop iteration.
    /// </summary>
    internal static void DecodeVectorRaw(byte* Header, int ElementSize, out byte* Data, out int Count)
    {
        Data = null;
        Count = 0;
        if (Header == null || ElementSize <= 0)
        {
            return;
        }
        byte* Begin = *(byte**)Header;
        uint Length = *(uint*)(Header + sizeof(void*));
        if (Begin == null || Length == 0)
        {
            return;
        }
        Data = Begin;
        Count = (int)Length;
    }
}
