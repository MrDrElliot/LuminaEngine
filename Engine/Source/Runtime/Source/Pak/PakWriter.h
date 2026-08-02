#pragma once

#include "PakFile.h"
#include "Containers/Array.h"
#include "Containers/Name.h"
#include "Containers/String.h"

namespace Lumina
{
    /** Buffers (path, bytes) entries in memory and writes the .pak in one shot. */
    class RUNTIME_API FPakWriter
    {
    public:

        /** Path stored verbatim. Returns false on duplicate. */
        bool AddEntry(FStringView VirtualPath, TSpan<const uint8> Data);
        bool AddEntry(FStringView VirtualPath, FStringView Data);

        /** Overwrites NativeFilePath. */
        bool Finalize(FStringView NativeFilePath);

        size_t NumEntries() const { return Entries.size(); }
        size_t TotalEntryBytes() const { return TotalDataSize; }

    private:

        struct FPendingEntry
        {
            FFixedString    VirtualPath;
            TVector<uint8>  Data;
        };

        TVector<FPendingEntry>          Entries;
        THashSet<FFixedString>          SeenPaths;
        size_t                          TotalDataSize = 0;
    };
}
