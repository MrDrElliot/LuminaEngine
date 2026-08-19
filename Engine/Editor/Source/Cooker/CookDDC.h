#pragma once

#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Platform/GenericPlatform.h"


namespace Lumina
{
    // Stable key for a cooked artifact: source content hash + cooker version stamp + cook-mode flags.
    // Bump FCookDDC::kCookStamp when cook logic changes in a way that should invalidate cached bytes.
    struct FCookInputHash
    {
        uint64 Hash = 0;

        bool IsValid() const { return Hash != 0; }
        bool operator==(const FCookInputHash& O) const { return Hash == O.Hash; }
        bool operator!=(const FCookInputHash& O) const { return Hash != O.Hash; }
    };

    // Disk-backed derived data cache under <EngineInstall>/Intermediates/DDC/<XX>/<full-hash>.ddc (two-byte hex prefix = 256 buckets).
    // Hit = identical source + cook logic, so cached bytes equal a fresh BundleAssetCooked; miss does the full load + cook-mode resave then Puts.
    class FCookDDC
    {
    public:
        // Bump to invalidate every cached entry; triggers: cook-mode serialize/saver layout, import wire format, EditorOnly strip rules.
        static constexpr uint32 kCookStamp = 2;

        static FCookInputHash ComputeKey(uint64 SourceContentHash);

        // Returns true and fills OutBytes on hit; bumps the hit counter.
        // Bumps the miss counter on absence (so the caller doesn't need to).
        static bool TryGet(const FCookInputHash& Key, TVector<uint8>& OutBytes);

        // Idempotent; safe to call concurrently from cook tasks.
        static bool Put(const FCookInputHash& Key, const TVector<uint8>& Bytes);

        // Per-Cook() diagnostics; reset at the top of FAssetCooker::Cook.
        static void Reset();
        static size_t Hits();
        static size_t Misses();
        static size_t WrittenBytes();
    };
}
