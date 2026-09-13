using System;
using System.Runtime.CompilerServices;

namespace LuminaSharp;

/// <summary>
/// Moves one argument between a native call frame and a managed value.
/// </summary>
/// <remarks>
/// A frame is a container like an object is, so this is the same split the generated property wrappers use:
/// a blittable slot is read and written in place at the property's offset, and anything owning memory goes
/// through the accessor that knows how to keep it straight.
/// </remarks>
internal static unsafe class FrameMarshal
{
    public static object? Read(IntPtr Frame, IntPtr Property, Type Wanted)
    {
        if (Property == IntPtr.Zero)
        {
            return Default(Wanted);
        }

        nint Slot = (nint)Frame + Native.PropertyOffset(Property);

        if (Wanted == typeof(int))    { return Unsafe.ReadUnaligned<int>((void*)Slot); }
        if (Wanted == typeof(float))  { return Unsafe.ReadUnaligned<float>((void*)Slot); }
        if (Wanted == typeof(bool))   { return Unsafe.ReadUnaligned<byte>((void*)Slot) != 0; }
        if (Wanted == typeof(double)) { return Unsafe.ReadUnaligned<double>((void*)Slot); }
        if (Wanted == typeof(long))   { return Unsafe.ReadUnaligned<long>((void*)Slot); }
        if (Wanted == typeof(uint))   { return Unsafe.ReadUnaligned<uint>((void*)Slot); }
        if (Wanted == typeof(short))  { return Unsafe.ReadUnaligned<short>((void*)Slot); }
        if (Wanted == typeof(ushort)) { return Unsafe.ReadUnaligned<ushort>((void*)Slot); }
        if (Wanted == typeof(byte))   { return Unsafe.ReadUnaligned<byte>((void*)Slot); }
        if (Wanted == typeof(sbyte))  { return Unsafe.ReadUnaligned<sbyte>((void*)Slot); }
        if (Wanted == typeof(ulong))  { return Unsafe.ReadUnaligned<ulong>((void*)Slot); }

        if (Wanted.IsEnum)
        {
            return Enum.ToObject(Wanted, Unsafe.ReadUnaligned<int>((void*)Slot));
        }

        // An FString is read in place, exactly as a blittable one is; there is no crossing for it.
        if (Wanted == typeof(string))
        {
            return NativeMarshal.ReadString(Slot);
        }

        if (typeof(NativeObject).IsAssignableFrom(Wanted))
        {
            IntPtr Object = Native.PropGetObject(Frame, Property);
            return Object == IntPtr.Zero ? null : NativeObjectMarshal.FromHandleOfType(Object, Wanted);
        }

        Debug.LogError($"Script function argument of type {Wanted.Name} cannot be read from a call frame; it comes through as its default.");
        return Default(Wanted);
    }

    public static void Write(IntPtr Frame, IntPtr Property, Type Declared, object? Value)
    {
        if (Property == IntPtr.Zero)
        {
            return;
        }

        nint Slot = (nint)Frame + Native.PropertyOffset(Property);

        if (Declared == typeof(int))    { Unsafe.WriteUnaligned((void*)Slot, Value is int V ? V : 0); return; }
        if (Declared == typeof(float))  { Unsafe.WriteUnaligned((void*)Slot, Value is float V ? V : 0.0f); return; }
        if (Declared == typeof(bool))   { Unsafe.WriteUnaligned((void*)Slot, (byte)(Value is true ? 1 : 0)); return; }
        if (Declared == typeof(double)) { Unsafe.WriteUnaligned((void*)Slot, Value is double V ? V : 0.0); return; }
        if (Declared == typeof(long))   { Unsafe.WriteUnaligned((void*)Slot, Value is long V ? V : 0L); return; }
        if (Declared == typeof(uint))   { Unsafe.WriteUnaligned((void*)Slot, Value is uint V ? V : 0u); return; }
        if (Declared == typeof(short))  { Unsafe.WriteUnaligned((void*)Slot, Value is short V ? V : (short)0); return; }
        if (Declared == typeof(ushort)) { Unsafe.WriteUnaligned((void*)Slot, Value is ushort V ? V : (ushort)0); return; }
        if (Declared == typeof(byte))   { Unsafe.WriteUnaligned((void*)Slot, Value is byte V ? V : (byte)0); return; }
        if (Declared == typeof(sbyte))  { Unsafe.WriteUnaligned((void*)Slot, Value is sbyte V ? V : (sbyte)0); return; }
        if (Declared == typeof(ulong))  { Unsafe.WriteUnaligned((void*)Slot, Value is ulong V ? V : 0ul); return; }

        if (Declared.IsEnum)
        {
            Unsafe.WriteUnaligned((void*)Slot, Value == null ? 0 : Convert.ToInt32(Value));
            return;
        }

        // Assigned rather than written in place, since the frame's string owns its memory.
        if (Declared == typeof(string))
        {
            Native.PropSetString(Frame, Property, Value as string ?? string.Empty);
            return;
        }

        if (typeof(NativeObject).IsAssignableFrom(Declared))
        {
            Native.PropSetObject(Frame, Property, NativeObjectMarshal.ToHandle(Value as NativeObject));
            return;
        }

        Debug.LogError($"Script function return of type {Declared.Name} cannot be written to a call frame; the caller sees the default.");
    }

    private static object? Default(Type Wanted)
    {
        return Wanted.IsValueType ? Activator.CreateInstance(Wanted) : null;
    }
}
