using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

// Blittable-only boundary: no runtime marshaling on any [UnmanagedCallersOnly] entry.
[assembly: DisableRuntimeMarshalling]

namespace LuminaSharp;

// Marshalling + boundary primitives shared by every native<->managed crossing: UTF-8 string transfer and
// keeping managed exceptions from unwinding into native code.
internal static unsafe class Interop
{
    // Longer than this => treated as corrupt rather than read (guards a bad pointer becoming a huge allocation).
    private const int MaxNativeStringBytes = 64 * 1024 * 1024;

    /// <summary>Reads a counted UTF-8 buffer from native into a managed string. Null/empty -&gt; "".</summary>
    public static string GetString(byte* Pointer, int ByteLength)
    {
        if (Pointer == null || ByteLength <= 0)
        {
            return string.Empty;
        }
        if (ByteLength >= MaxNativeStringBytes)
        {
            Native.Log(ELogLevel.Warn, "Interop.GetString: refusing to read an implausibly long native string.");
            return string.Empty;
        }
        return Encoding.UTF8.GetString(Pointer, ByteLength);
    }

    /// <summary>Reads a NUL-terminated UTF-8 string from native. Null -&gt; "".</summary>
    public static string GetString(byte* Pointer)
    {
        if (Pointer == null)
        {
            return string.Empty;
        }

        ReadOnlySpan<byte> Span = MemoryMarshal.CreateReadOnlySpanFromNullTerminated(Pointer);
        if (Span.Length >= MaxNativeStringBytes)
        {
            Native.Log(ELogLevel.Warn, "Interop.GetString: refusing to read an implausibly long native string.");
            return string.Empty;
        }

        return Encoding.UTF8.GetString(Span);
    }

    /// <summary>A UTF-8, NUL-terminated copy of a managed string for one native call: stack-encoded when it fits
    /// (alloc-free), else heap. Caller MUST <see cref="Free"/> it in a finally; freeing a stack copy is a no-op.</summary>
    public unsafe ref struct FInteropString
    {
        /// <summary>UTF-8 bytes (NUL-terminated); null when the source string was null.</summary>
        public byte* Pointer;

        /// <summary>UTF-8 byte count excluding the NUL terminator.</summary>
        public int Length;

        private bool bHeap;

        public FInteropString(string Value, Span<byte> ScratchBuffer)
        {
            Pointer = null;
            Length = 0;
            bHeap = false;
            if (Value == null)
            {
                return;
            }

            // UTF-8 never takes more than three bytes per UTF-16 unit, so a string that fits that bound encodes in one pass.
            if (Value.Length * 3 + 1 <= ScratchBuffer.Length)
            {
                int Encoded = Encoding.UTF8.GetBytes(Value, ScratchBuffer);
                ScratchBuffer[Encoded] = 0;
                Pointer = (byte*)Unsafe.AsPointer(ref MemoryMarshal.GetReference(ScratchBuffer));
                Length = Encoded;
                return;
            }

            int ByteCount = Encoding.UTF8.GetByteCount(Value);
            if (ByteCount + 1 <= ScratchBuffer.Length)
            {
                int Written = Encoding.UTF8.GetBytes(Value, ScratchBuffer);
                ScratchBuffer[Written] = 0;
                Pointer = (byte*)Unsafe.AsPointer(ref MemoryMarshal.GetReference(ScratchBuffer));
                Length = Written;
                return;
            }

            byte* Memory = (byte*)NativeMemory.Alloc((nuint)ByteCount + 1);
            fixed (char* Source = Value)
            {
                Encoding.UTF8.GetBytes(Source, Value.Length, Memory, ByteCount);
            }
            Memory[ByteCount] = 0;
            Pointer = Memory;
            Length = ByteCount;
            bHeap = true;
        }

        public void Free()
        {
            if (bHeap && Pointer != null)
            {
                NativeMemory.Free(Pointer);
                bHeap = false;
            }
            Pointer = null;
        }
    }

    /// <summary>Logs an exception caught at a native boundary; every [UnmanagedCallersOnly] entry funnels here so a
    /// managed exception never unwinds into native code. Swallows any secondary logging failure.</summary>
    public static void LogException(Exception Exception)
    {
        LogException(Exception, null);
    }

    // Keyed by throw site, so a script throwing every frame logs its stack once and then a count.
    private static readonly Dictionary<string, int> ExceptionCounts = new();
    private const int MaxTrackedExceptionSites = 4096;

    internal static void LogException(Exception Exception, string? Context)
    {
        try
        {
            string Where = Context != null ? " in " + Context : string.Empty;
            string Key = Exception.GetType().FullName + "@" + Exception.TargetSite?.DeclaringType?.FullName + "." + Exception.TargetSite?.Name + Where;

            int Count;
            lock (ExceptionCounts)
            {
                if (ExceptionCounts.Count >= MaxTrackedExceptionSites && !ExceptionCounts.ContainsKey(Key))
                {
                    ExceptionCounts.Clear();
                }
                ExceptionCounts.TryGetValue(Key, out Count);
                ExceptionCounts[Key] = ++Count;
            }

            if (Count == 1)
            {
                Native.Log(ELogLevel.Error, "LuminaSharp boundary exception" + Where + ": " + Exception);
            }
            else if (IsPowerOfTen(Count))
            {
                Native.Log(ELogLevel.Error, $"LuminaSharp boundary exception{Where} repeated {Count} times: {Exception.GetType().Name}: {Exception.Message}");
            }
        }
        catch
        {
            // The boundary must not throw, even if logging fails.
        }
    }

    // A reload usually means the throwing code changed, so its first throw deserves a full stack again.
    internal static void ResetExceptionThrottle()
    {
        lock (ExceptionCounts)
        {
            ExceptionCounts.Clear();
        }
    }

    private static bool IsPowerOfTen(int Value)
    {
        while (Value >= 10 && Value % 10 == 0)
        {
            Value /= 10;
        }
        return Value == 1;
    }
}
