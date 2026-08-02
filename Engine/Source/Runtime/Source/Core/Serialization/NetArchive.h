#pragma once

#include "Archiver.h"
#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CObject;
    class FName;
    struct FAssetRef;

    // Compact bit-oriented archive for replication.
    class RUNTIME_API FNetArchive : public FArchive
    {
    public:

        // Writer: appends bits onto OutBytes (which it does not own).
        explicit FNetArchive(TVector<uint8>& OutBytes);

        // Reader: consumes up to Size bytes from Data (which it does not own).
        FNetArchive(const uint8* Data, SIZE_T Size);

        // FArchive byte path -> whole-byte bit writes, so operator<< keeps working.
        void  Serialize(void* V, int64 Length) override;

        // Tight bit path. Reads/writes NumBits of Value (LSB-first), per the archive's mode.
        void  SerializeBits(void* Value, uint32 NumBits);
        void  SerializeBit(bool& bValue);

        // Advance the cursor to the next byte boundary (no-op when already aligned). Writing pads with zero
        // bits; reading just skips. Used to frame per-field net deltas so each changed field stays byte-addressable.
        void  AlignToByte();

        // Reader fast path: when the cursor is byte-aligned and NumBytes are available, return a pointer to
        // those raw bytes IN the source buffer and advance past them (zero-copy). Returns nullptr when writing,
        // not byte-aligned, or out of range -- the caller must fall back to Serialize. The pointer stays valid
        // only as long as the source buffer does.
        const uint8* ReadBytesInPlace(int64 NumBytes);

        int64 Tell() override      { return BitCursor / 8; }
        int64 TotalSize() override { return (TotalBits + 7) / 8; }
        void  Seek(int64 BytePos) override { BitCursor = BytePos * 8; }

        // Optional NetGUID translation for entity-ref fields. Null passes the raw value through.
        TFunction<uint32(uint32 /*LocalEntity*/)> EntityToNetGUID;
        TFunction<uint32(uint32 /*NetGUID*/)>     NetGUIDToEntity;

        // Optional object-ref index translation. When set, refs serialize as a compact varint index
        // exported once via ObjectExport. Null reads/writes the full GUID.
        TFunction<uint32(CObject*)> ObjectToNetIndex;
        TFunction<CObject*(uint32)> NetIndexToObject;

        // Same scheme for FAssetRef fields, exported once via AssetExport. Reader uses an out-param so
        // FAssetRef need not be complete here.
        TFunction<uint32(const FAssetRef&)>     AssetRefToNetIndex;
        TFunction<void(uint32, FAssetRef&)>     NetIndexToAssetRef;

        // Same scheme for FName fields, exported once via NameExport. When set, names serialize as a compact
        // varint index; the string is carried by the export. Null reads/writes the full string.
        TFunction<uint32(const FName&)>         NameToNetIndex;
        TFunction<void(uint32, FName&)>         NetIndexToName;

    private:

        void   WriteBit(uint32 Bit);
        uint32 ReadBit();

        TVector<uint8>* WriteBytes = nullptr; // writer target (null when reading)
        const uint8*    ReadBytes  = nullptr; // reader source (null when writing)
        int64           BitCursor  = 0;
        int64           TotalBits  = 0;
    };

    // LEB128 varint over the archive byte path; 1 byte for values < 128.
    RUNTIME_API void   WriteVarUInt(FNetArchive& Ar, uint32 Value);
    RUNTIME_API uint32 ReadVarUInt(FNetArchive& Ar);
}
