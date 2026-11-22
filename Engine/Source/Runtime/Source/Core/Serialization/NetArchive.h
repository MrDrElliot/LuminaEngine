#pragma once

#include "Archiver.h"
#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CObject;
    class FName;
    struct SUIDocumentRef;

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

        // An entity crosses as its NetGUID when this archive translates them, otherwise as its raw id.
        void  SerializeEntity(uint32& EntityId);

        // Advances to the next byte boundary so each changed net field stays byte-addressable. Writing pads with zero bits.
        void  AlignToByte();

        // Zero-copy reader path that returns nullptr when writing, unaligned or out of range, so the caller must fall back to Serialize.
        const uint8* ReadBytesInPlace(int64 NumBytes);

        int64 Tell() override      { return BitCursor / 8; }
        int64 TotalSize() override { return (TotalBits + 7) / 8; }
        void  Seek(int64 BytePos) override
        {
            if (BytePos < 0 || BytePos > (TotalBits + 7) / 8)
            {
                SetHasError(true);
                BitCursor = TotalBits;
                return;
            }
            BitCursor = BytePos * 8;
        }

        // Optional NetGUID translation for entity-ref fields. Null passes the raw value through.
        TFunction<uint32(uint32 /*LocalEntity*/)> EntityToNetGUID;
        TFunction<uint32(uint32 /*NetGUID*/)>     NetGUIDToEntity;

        // When set, object refs serialize as a varint index exported once via ObjectExport, otherwise as a full GUID.
        TFunction<uint32(CObject*)> ObjectToNetIndex;
        TFunction<CObject*(uint32)> NetIndexToObject;

        // Same scheme for UI document fields, exported once via AssetExport. The reader takes an out-param so the type can stay incomplete.
        TFunction<uint32(const SUIDocumentRef&)>    DocumentRefToNetIndex;
        TFunction<void(uint32, SUIDocumentRef&)>    NetIndexToDocumentRef;

        // Same scheme for FName fields via NameExport, otherwise the full string.
        TFunction<uint32(const FName&)>         NameToNetIndex;
        TFunction<void(uint32, FName&)>         NetIndexToName;

        // Set by a reader that met an index whose export has not arrived yet, so the sender can be asked to send it again.
        bool bUnresolvedReference = false;

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
