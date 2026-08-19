#include "RuntimePCH.h"
#include "NetArchive.h"

#include <algorithm>
#include "Memory/Memcpy.h"

namespace Lumina
{
    FNetArchive::FNetArchive(TVector<uint8>& OutBytes)
        : WriteBytes(&OutBytes)
        , BitCursor(static_cast<int64>(OutBytes.size()) * 8)
        , TotalBits(static_cast<int64>(OutBytes.size()) * 8)
    {
        SetFlag(EArchiverFlags::Writing);
    }

    FNetArchive::FNetArchive(const uint8* Data, SIZE_T Size)
        : ReadBytes(Data)
        , TotalBits(static_cast<int64>(Size) * 8)
    {
        SetFlag(EArchiverFlags::Reading);
    }

    void FNetArchive::WriteBit(uint32 Bit)
    {
        const int64 ByteIndex = BitCursor >> 3;
        const int64 BitIndex  = BitCursor & 7;
        if (static_cast<int64>(WriteBytes->size()) <= ByteIndex)
        {
            WriteBytes->resize(static_cast<size_t>(ByteIndex) + 1, 0);
        }
        if (Bit)
        {
            (*WriteBytes)[ByteIndex] |= static_cast<uint8>(1u << BitIndex);
        }
        else
        {
            (*WriteBytes)[ByteIndex] &= static_cast<uint8>(~(1u << BitIndex));
        }
        ++BitCursor;
        TotalBits = Math::Max(BitCursor, TotalBits);
    }

    uint32 FNetArchive::ReadBit()
    {
        if (BitCursor >= TotalBits)
        {
            SetHasError(true);
            return 0;
        }
        const int64 ByteIndex = BitCursor >> 3;
        const int64 BitIndex  = BitCursor & 7;
        const uint32 Bit = (ReadBytes[ByteIndex] >> BitIndex) & 1u;
        ++BitCursor;
        return Bit;
    }

    void FNetArchive::SerializeBits(void* Value, uint32 NumBits)
    {
        if (NumBits == 0)
        {
            return;
        }

        uint8* Bytes = static_cast<uint8*>(Value);

        // Fast path: byte-aligned cursor + whole-byte count -> memcpy.
        if ((BitCursor & 7) == 0 && (NumBits & 7) == 0)
        {
            const int64 ByteCount = NumBits / 8;
            const int64 ByteIndex = BitCursor >> 3;

            if (IsWriting())
            {
                if (static_cast<int64>(WriteBytes->size()) < ByteIndex + ByteCount)
                {
                    WriteBytes->resize(static_cast<size_t>(ByteIndex + ByteCount), 0);
                }
                Memory::Memcpy(WriteBytes->data() + ByteIndex, Bytes, static_cast<size_t>(ByteCount));
                BitCursor += NumBits;
                TotalBits = Math::Max(BitCursor, TotalBits);
            }
            else
            {
                if (BitCursor + NumBits > TotalBits)
                {
                    SetHasError(true);
                    return;
                }
                Memory::Memcpy(Bytes, ReadBytes + ByteIndex, static_cast<size_t>(ByteCount));
                BitCursor += NumBits;
            }
            return;
        }

        // Bit path (e.g. a bool, or a multi-byte value after an odd number of bits).
        if (IsWriting())
        {
            for (uint32 i = 0; i < NumBits; ++i)
            {
                WriteBit((Bytes[i >> 3] >> (i & 7)) & 1u);
            }
        }
        else
        {
            const uint32 NumBytes = (NumBits + 7) / 8;
            for (uint32 b = 0; b < NumBytes; ++b)
            {
                Bytes[b] = 0;
            }
            for (uint32 i = 0; i < NumBits; ++i)
            {
                if (ReadBit())
                {
                    Bytes[i >> 3] |= static_cast<uint8>(1u << (i & 7));
                }
            }
        }
    }

    void FNetArchive::Serialize(void* V, int64 Length)
    {
        if (Length > 0)
        {
            SerializeBits(V, static_cast<uint32>(Length * 8));
        }
    }

    void FNetArchive::SerializeBit(bool& bValue)
    {
        if (IsWriting())
        {
            WriteBit(bValue ? 1u : 0u);
        }
        else
        {
            bValue = (ReadBit() != 0);
        }
    }

    void FNetArchive::AlignToByte()
    {
        const int64 Rem = BitCursor & 7;
        if (Rem == 0)
        {
            return;
        }
        const int64 Pad = 8 - Rem;
        if (IsWriting())
        {
            for (int64 i = 0; i < Pad; ++i)
            {
                WriteBit(0u);
            }
        }
        else
        {
            BitCursor += Pad;
            if (BitCursor > TotalBits)
            {
                SetHasError(true);
            }
        }
    }

    const uint8* FNetArchive::ReadBytesInPlace(int64 NumBytes)
    {
        if (!IsReading() || NumBytes < 0 || (BitCursor & 7) != 0)
        {
            return nullptr;
        }
        if (BitCursor + NumBytes * 8 > TotalBits)
        {
            return nullptr; // out of range
        }
        const uint8* Ptr = ReadBytes + (BitCursor >> 3);
        BitCursor += NumBytes * 8;
        return Ptr;
    }

    void WriteVarUInt(FNetArchive& Ar, uint32 Value)
    {
        while (Value >= 0x80)
        {
            uint8 Byte = static_cast<uint8>(Value) | 0x80;
            Ar << Byte;
            Value >>= 7;
        }
        uint8 Last = static_cast<uint8>(Value);
        Ar << Last;
    }

    uint32 ReadVarUInt(FNetArchive& Ar)
    {
        uint32 Value = 0;
        int    Shift = 0;
        uint8  Byte  = 0;
        do
        {
            Ar << Byte;
            if (Ar.HasError())
            {
                break;
            }
            Value |= static_cast<uint32>(Byte & 0x7F) << Shift;
            Shift += 7;
        }
        while ((Byte & 0x80) != 0 && Shift < 35);
        return Value;
    }
}
