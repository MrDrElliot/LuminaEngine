using System;
using System.Buffers.Binary;
using System.Runtime.CompilerServices;
using System.Text;

namespace LuminaSharp;

// Integers are zigzag varints, so the small values most arguments carry cost a byte. Each typeof test folds away per T.
public ref struct NetWriter
{
    [ThreadStatic] private static byte[]? Pool;

    private byte[] Buffer;
    private int Count;
    private readonly ulong World;

    internal NetWriter(ulong World)
    {
        Buffer = Pool ??= new byte[256];
        Count = 0;
        this.World = World;
    }

    internal readonly ReadOnlySpan<byte> Written => new(Buffer, 0, Count);

    private Span<byte> Reserve(int Size)
    {
        if (Count + Size > Buffer.Length)
        {
            var Grown = new byte[Math.Max(Count + Size, Buffer.Length * 2)];
            Buffer.AsSpan(0, Count).CopyTo(Grown);
            Buffer = Grown;
            Pool = Grown;
        }
        Span<byte> Slot = Buffer.AsSpan(Count, Size);
        Count += Size;
        return Slot;
    }

    public void WriteVarUInt(ulong Value)
    {
        while (Value >= 0x80)
        {
            Reserve(1)[0] = (byte)(Value | 0x80);
            Value >>= 7;
        }
        Reserve(1)[0] = (byte)Value;
    }

    public void WriteVarInt(long Value) => WriteVarUInt((ulong)((Value << 1) ^ (Value >> 63)));

    public void Write<T>(T Value)
    {
        if (typeof(T) == typeof(bool)) { Reserve(1)[0] = Unsafe.As<T, bool>(ref Value) ? (byte)1 : (byte)0; return; }
        if (typeof(T) == typeof(byte)) { Reserve(1)[0] = Unsafe.As<T, byte>(ref Value); return; }
        if (typeof(T) == typeof(sbyte)) { WriteVarInt(Unsafe.As<T, sbyte>(ref Value)); return; }
        if (typeof(T) == typeof(short)) { WriteVarInt(Unsafe.As<T, short>(ref Value)); return; }
        if (typeof(T) == typeof(ushort)) { WriteVarUInt(Unsafe.As<T, ushort>(ref Value)); return; }
        if (typeof(T) == typeof(int)) { WriteVarInt(Unsafe.As<T, int>(ref Value)); return; }
        if (typeof(T) == typeof(uint)) { WriteVarUInt(Unsafe.As<T, uint>(ref Value)); return; }
        if (typeof(T) == typeof(long)) { WriteVarInt(Unsafe.As<T, long>(ref Value)); return; }
        if (typeof(T) == typeof(ulong)) { WriteVarUInt(Unsafe.As<T, ulong>(ref Value)); return; }
        if (typeof(T) == typeof(float)) { BinaryPrimitives.WriteSingleLittleEndian(Reserve(4), Unsafe.As<T, float>(ref Value)); return; }
        if (typeof(T) == typeof(double)) { BinaryPrimitives.WriteDoubleLittleEndian(Reserve(8), Unsafe.As<T, double>(ref Value)); return; }
        if (typeof(T) == typeof(Entity)) { WriteEntity(Unsafe.As<T, Entity>(ref Value)); return; }
        if (typeof(T) == typeof(Connection)) { WriteVarUInt(Unsafe.As<T, Connection>(ref Value).Id); return; }
        if (typeof(T) == typeof(string)) { WriteString((string?)(object?)Value); return; }

        if (typeof(T).IsEnum)
        {
            WriteVarInt(EnumToLong(ref Value));
            return;
        }

        if (!RuntimeHelpers.IsReferenceOrContainsReferences<T>())
        {
            Unsafe.WriteUnaligned(ref Reserve(Unsafe.SizeOf<T>())[0], Value);
            return;
        }

        throw new NotSupportedException($"{typeof(T).Name} cannot be sent over the network. Use primitives, strings, entities, enums or plain structs.");
    }

    private void WriteEntity(Entity Value)
    {
        // A null or local-only entity travels as 0, which the reader turns back into Entity.Null.
        WriteVarUInt(Value.IsNull ? 0u : NetNative.EntityToNetId(World, Value.Id));
    }

    private void WriteString(string? Value)
    {
        if (string.IsNullOrEmpty(Value))
        {
            WriteVarUInt(0);
            return;
        }
        int Size = Encoding.UTF8.GetByteCount(Value);
        WriteVarUInt((ulong)Size);
        Encoding.UTF8.GetBytes(Value, Reserve(Size));
    }

    internal static long EnumToLong<T>(ref T Value)
    {
        return Unsafe.SizeOf<T>() switch
        {
            1 => Unsafe.As<T, sbyte>(ref Value),
            2 => Unsafe.As<T, short>(ref Value),
            4 => Unsafe.As<T, int>(ref Value),
            _ => Unsafe.As<T, long>(ref Value),
        };
    }
}

public ref struct NetReader
{
    private readonly ReadOnlySpan<byte> Data;
    private int Position;
    private readonly ulong World;

    internal NetReader(ReadOnlySpan<byte> Data, ulong World)
    {
        this.Data = Data;
        Position = 0;
        this.World = World;
        Failed = false;
    }

    // Set once a read runs past the end, after which every read returns its default.
    public bool Failed { get; private set; }

    private ReadOnlySpan<byte> Take(int Size)
    {
        if (Failed || Size < 0 || Position + Size > Data.Length)
        {
            Failed = true;
            return default;
        }
        ReadOnlySpan<byte> Slice = Data.Slice(Position, Size);
        Position += Size;
        return Slice;
    }

    public ulong ReadVarUInt()
    {
        ulong Result = 0;
        for (int Shift = 0; Shift < 64; Shift += 7)
        {
            ReadOnlySpan<byte> Next = Take(1);
            if (Failed)
            {
                return 0;
            }
            Result |= (ulong)(Next[0] & 0x7F) << Shift;
            if ((Next[0] & 0x80) == 0)
            {
                return Result;
            }
        }
        Failed = true;
        return 0;
    }

    public long ReadVarInt()
    {
        ulong Raw = ReadVarUInt();
        return (long)(Raw >> 1) ^ -(long)(Raw & 1);
    }

    public T Read<T>()
    {
        if (typeof(T) == typeof(bool)) { bool V = ReadByte() != 0; return Unsafe.As<bool, T>(ref V); }
        if (typeof(T) == typeof(byte)) { byte V = ReadByte(); return Unsafe.As<byte, T>(ref V); }
        if (typeof(T) == typeof(sbyte)) { sbyte V = (sbyte)ReadVarInt(); return Unsafe.As<sbyte, T>(ref V); }
        if (typeof(T) == typeof(short)) { short V = (short)ReadVarInt(); return Unsafe.As<short, T>(ref V); }
        if (typeof(T) == typeof(ushort)) { ushort V = (ushort)ReadVarUInt(); return Unsafe.As<ushort, T>(ref V); }
        if (typeof(T) == typeof(int)) { int V = (int)ReadVarInt(); return Unsafe.As<int, T>(ref V); }
        if (typeof(T) == typeof(uint)) { uint V = (uint)ReadVarUInt(); return Unsafe.As<uint, T>(ref V); }
        if (typeof(T) == typeof(long)) { long V = ReadVarInt(); return Unsafe.As<long, T>(ref V); }
        if (typeof(T) == typeof(ulong)) { ulong V = ReadVarUInt(); return Unsafe.As<ulong, T>(ref V); }
        if (typeof(T) == typeof(float)) { float V = ReadSingle(); return Unsafe.As<float, T>(ref V); }
        if (typeof(T) == typeof(double)) { double V = ReadDouble(); return Unsafe.As<double, T>(ref V); }
        if (typeof(T) == typeof(Entity)) { Entity V = ReadEntity(); return Unsafe.As<Entity, T>(ref V); }
        if (typeof(T) == typeof(Connection)) { Connection V = new((uint)ReadVarUInt()); return Unsafe.As<Connection, T>(ref V); }
        if (typeof(T) == typeof(string)) { return (T)(object)ReadString(); }

        if (typeof(T).IsEnum)
        {
            long Raw = ReadVarInt();
            T V = default!;
            switch (Unsafe.SizeOf<T>())
            {
                case 1: Unsafe.As<T, sbyte>(ref V) = (sbyte)Raw; break;
                case 2: Unsafe.As<T, short>(ref V) = (short)Raw; break;
                case 4: Unsafe.As<T, int>(ref V) = (int)Raw; break;
                default: Unsafe.As<T, long>(ref V) = Raw; break;
            }
            return V;
        }

        if (!RuntimeHelpers.IsReferenceOrContainsReferences<T>())
        {
            ReadOnlySpan<byte> Bytes = Take(Unsafe.SizeOf<T>());
            return Failed ? default! : Unsafe.ReadUnaligned<T>(ref Unsafe.AsRef(in Bytes[0]));
        }

        throw new NotSupportedException($"{typeof(T).Name} cannot be received over the network.");
    }

    private byte ReadByte()
    {
        ReadOnlySpan<byte> Bytes = Take(1);
        return Failed ? (byte)0 : Bytes[0];
    }

    private float ReadSingle()
    {
        ReadOnlySpan<byte> Bytes = Take(4);
        return Failed ? 0f : BinaryPrimitives.ReadSingleLittleEndian(Bytes);
    }

    private double ReadDouble()
    {
        ReadOnlySpan<byte> Bytes = Take(8);
        return Failed ? 0.0 : BinaryPrimitives.ReadDoubleLittleEndian(Bytes);
    }

    private Entity ReadEntity()
    {
        uint NetId = (uint)ReadVarUInt();
        return NetId == 0 ? Entity.Null : new Entity(NetNative.NetIdToEntity(World, NetId));
    }

    private string ReadString()
    {
        int Size = (int)ReadVarUInt();
        if (Size == 0)
        {
            return "";
        }
        ReadOnlySpan<byte> Bytes = Take(Size);
        return Failed ? "" : Encoding.UTF8.GetString(Bytes);
    }
}
