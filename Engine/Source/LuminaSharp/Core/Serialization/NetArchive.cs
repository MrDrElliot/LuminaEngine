using System;
using System.Numerics;
using System.Text;
using Lumina;

namespace LuminaSharp;

// The C# side of a native FNetArchive, the bit stream a struct's NetSerialize fills or drains. Each method writes Value when writing and returns what it read when reading.
public readonly unsafe ref struct FNetArchive
{
    private readonly IntPtr Handle;

    internal FNetArchive(IntPtr Handle)
    {
        this.Handle = Handle;
    }

    public bool IsReading => Native.NetArchiveIsReading(Handle) != 0;

    public bool IsWriting => !IsReading;

    // Set once a read ran past the end of what arrived, after which every read returns zero.
    public bool HasError => Native.NetArchiveHasError(Handle) != 0;

    // Rejects what arrived, as for a value outside the range it may take. The message carrying it is dropped.
    public void SetError() => Native.NetArchiveSetError(Handle);

    public ulong SerializeBits(ulong Value, int Bits)
    {
        ulong Stored = IsReading ? 0ul : Value;
        Native.NetArchiveBits(Handle, &Stored, (uint)Math.Clamp(Bits, 0, 64));
        return Bits >= 64 ? Stored : Stored & ((1ul << Bits) - 1ul);
    }

    public bool Serialize(bool Value) => SerializeBits(Value ? 1ul : 0ul, 1) != 0;

    public byte Serialize(byte Value) => (byte)SerializeBits(Value, 8);

    public sbyte Serialize(sbyte Value) => (sbyte)(byte)SerializeBits((byte)Value, 8);

    public short Serialize(short Value) => (short)(ushort)SerializeBits((ushort)Value, 16);

    public ushort Serialize(ushort Value) => (ushort)SerializeBits(Value, 16);

    public int Serialize(int Value) => (int)(uint)SerializeBits((uint)Value, 32);

    public uint Serialize(uint Value) => (uint)SerializeBits(Value, 32);

    public long Serialize(long Value) => (long)SerializeBits((ulong)Value, 64);

    public ulong Serialize(ulong Value) => SerializeBits(Value, 64);

    public float Serialize(float Value) => BitConverter.UInt32BitsToSingle((uint)SerializeBits(BitConverter.SingleToUInt32Bits(Value), 32));

    public double Serialize(double Value) => BitConverter.UInt64BitsToDouble(SerializeBits(BitConverter.DoubleToUInt64Bits(Value), 64));

    public FVector2 Serialize(FVector2 Value) => new(Serialize(Value.X), Serialize(Value.Y));

    public FVector3 Serialize(FVector3 Value) => new(Serialize(Value.X), Serialize(Value.Y), Serialize(Value.Z));

    public FVector4 Serialize(FVector4 Value) => new(Serialize(Value.X), Serialize(Value.Y), Serialize(Value.Z), Serialize(Value.W));

    public FQuat Serialize(FQuat Value) => new(Serialize(Value.X), Serialize(Value.Y), Serialize(Value.Z), Serialize(Value.W));

    // An entity crosses as the id both peers know it by, so it names the same entity on the receiver.
    public FEntity Serialize(FEntity Value)
    {
        uint Id = Value.Id;
        Native.NetArchiveEntity(Handle, &Id);
        return new FEntity(Id);
    }

    // Seven bits a byte, so a small count costs one byte and only a large one pays for all five.
    public uint SerializeVarUInt(uint Value)
    {
        if (IsReading)
        {
            uint Result = 0;
            for (int Shift = 0; Shift < 35; Shift += 7)
            {
                uint Group = (uint)SerializeBits(0, 8);
                Result |= (Group & 0x7Fu) << Shift;
                if ((Group & 0x80u) == 0)
                {
                    break;
                }
            }
            return Result;
        }

        uint Remaining = Value;
        do
        {
            uint Group = Remaining & 0x7Fu;
            Remaining >>= 7;
            SerializeBits(Remaining != 0 ? Group | 0x80u : Group, 8);
        }
        while (Remaining != 0);
        return Value;
    }

    // Only as many bits as the range needs. A value outside it is clamped on the writer.
    public int SerializeRange(int Value, int Min, int Max)
    {
        ulong Span = (ulong)((long)Max - Min);
        int Bits = Span == 0 ? 0 : 64 - BitOperations.LeadingZeroCount(Span);
        long Clamped = Math.Clamp((long)Value, Min, Max);
        return (int)(Min + (long)SerializeBits((ulong)(Clamped - Min), Bits));
    }

    // Snapped to one of 2^Bits evenly spaced steps between Min and Max, for values whose precision is known ahead.
    public float SerializeQuantized(float Value, float Min, float Max, int Bits)
    {
        Bits = Math.Clamp(Bits, 1, 32);
        double Steps = (1ul << Bits) - 1ul;
        double Fraction = Max > Min ? Math.Clamp((Value - Min) / (double)(Max - Min), 0.0, 1.0) : 0.0;
        ulong Stored = SerializeBits((ulong)Math.Round(Fraction * Steps), Bits);
        return (float)(Min + (Max - Min) * (Stored / Steps));
    }

    public string Serialize(string? Value)
    {
        if (IsReading)
        {
            int Length = (int)SerializeVarUInt(0);
            if (Length == 0 || HasError)
            {
                return string.Empty;
            }
            byte[] Read = new byte[Length];
            fixed (byte* Data = Read)
            {
                Native.NetArchiveBytes(Handle, Data, Length);
            }
            return Encoding.UTF8.GetString(Read);
        }

        byte[] Bytes = Encoding.UTF8.GetBytes(Value ?? string.Empty);
        SerializeVarUInt((uint)Bytes.Length);
        fixed (byte* Data = Bytes)
        {
            Native.NetArchiveBytes(Handle, Data, Bytes.Length);
        }
        return Value ?? string.Empty;
    }
}
