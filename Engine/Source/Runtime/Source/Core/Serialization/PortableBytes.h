#pragma once

#include "Containers/Name.h"
#include "Containers/Vector.h"
#include "Core/Serialization/Archiver.h"
#include "GUID/GUID.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class CStruct;

    // Serialized bytes held apart from the archive that wrote them, carrying what their name slots and object indices stand for.
    class RUNTIME_API FPortableBytes
    {
    public:

        struct FNameSlot
        {
            uint32 Slot = 0;
            FName  Name;

            friend FArchive& operator<<(FArchive& Ar, FNameSlot& Entry) { return Ar << Entry.Slot << Entry.Name; }
        };

        struct FObjectSlot
        {
            int32 Index = 0;
            FGuid Object;
        };

        // Takes Size bytes at the cursor, false when the archive cannot say what its slots mean.
        bool Cut(FArchive& Ar, int64 Size);

        // Serializes a live value as a package would, with slots numbered within these bytes alone.
        void Capture(const CStruct& Struct, const void* Value);

        // Reads the bytes back, valid while this holder is alive and unchanged.
        TUniquePtr<FArchive> MakeReader() const;

        // The names and objects go through Ar itself, so whatever it writes into encodes them in its own tables.
        friend FArchive& operator<<(FArchive& Ar, FPortableBytes& Data) { return Ar << Data.FileVersion << Data.Names << Data.Objects << Data.Bytes; }

        bool IsEmpty() const { return Bytes.IsEmpty(); }

        int32                FileVersion = 0;
        TVector<uint8>       Bytes;
        TVector<FNameSlot>   Names;
        TVector<FObjectSlot> Objects;
    };

    RUNTIME_API FArchive& operator<<(FArchive& Ar, FPortableBytes::FObjectSlot& Entry);
}
