#pragma once

#include "PakFile.h"
#include "Containers/HashTable.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
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

        // Takes the bytes without a copy, and a precompressed entry such as a cooked package is stored without another deflate pass.
        bool AddEntry(FStringView VirtualPath, TVector<uint8>&& Data, bool bPrecompressed = false);

        /** Overwrites NativeFilePath. */
        bool Finalize(FStringView NativeFilePath);

        size_t NumEntries() const { return Entries.size(); }
        size_t TotalEntryBytes() const { return TotalDataSize; }

        // Every entry path in the order Finalize writes them.
        void GetEntryPaths(TVector<FFixedString>& OutPaths) const;

    private:

        struct FPendingEntry
        {
            FFixedString    VirtualPath;
            TVector<uint8>  Data;
            bool            bPrecompressed = false;
        };

        TVector<FPendingEntry>          Entries;
        THashSet<FFixedString>          SeenPaths;
        size_t                          TotalDataSize = 0;
    };
}
