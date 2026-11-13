#pragma once

#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Lumina.h"
#include "Platform/GenericPlatform.h"


namespace Lumina
{
    // Stable key for a cooked artifact; bumping either FCookDDC::kCookStamp or LUMINA_VERSION_NUM invalidates every cached entry.
    struct FCookInputHash
    {
        uint64 Hash = 0;

        bool IsValid() const { return Hash != 0; }
        bool operator==(const FCookInputHash& O) const { return Hash == O.Hash; }
        bool operator!=(const FCookInputHash& O) const { return Hash != O.Hash; }
    };

    // Disk-backed derived data cache under <EngineInstall>/Intermediates/DDC/<XX>/<full-hash>.ddc (two-byte hex prefix = 256 buckets).
    // Hit = identical source + cook logic, so cached bytes equal a fresh BundleAssetCooked; miss does the full load + cook-mode resave then Puts.
    class EDITOR_API FCookDDC
    {
    public:
        // Manual trigger for cook logic changes that don't bump LUMINA_VERSION_NUM, such as cook-mode serialize/saver layout or EditorOnly strip rules.
        static constexpr uint32 kCookStamp = 3;

        // Mixes EngineVersion in alongside kCookStamp, so a new engine build never reads a cache written by another one.
        static FCookInputHash ComputeKey(uint64 SourceContentHash, uint32 EngineVersion = LUMINA_VERSION_NUM);

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
