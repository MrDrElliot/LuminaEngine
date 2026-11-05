#pragma once

#include "PakFile.h"
#include "Containers/HashTable.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Containers/String.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    /** Loaded .pak: raw blob + TOC keyed by virtual path. Open() loads the entire file at once. */
    class RUNTIME_API FPakArchive
    {
    public:

        /** nullptr on missing/malformed/bad-magic. */
        static TSharedPtr<FPakArchive> Open(FStringView NativeFilePath);

        /** Empty span if missing. */
        TSpan<const uint8> ReadEntry(FStringView VirtualPath) const;

        bool   HasEntry(FStringView VirtualPath) const;
        int64 EntrySize(FStringView VirtualPath) const;
        int32 NumEntries() const { return Entries.Num(); }

        /** Unique top-level components (e.g. "/Engine", "/Game") for VFS mount setup. */
        TVector<FString> GetTopLevelAliases() const;

        void ForEachEntry(const TFunction<void(FStringView, int64)>& Func) const;

        /** Walks entries under Prefix + '/' (or equal to Prefix). */
        void ForEachEntryUnder(FStringView Prefix, const TFunction<void(FStringView, int64)>& Func) const;

    private:

        FPakArchive() = default;

        TVector64<uint8>                        RawData;
        THashMap<FFixedString, FPakEntry>       Entries;
    };
}
