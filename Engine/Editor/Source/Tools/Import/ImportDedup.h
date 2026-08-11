#pragma once

#include <cstring>

#include "Lumina.h"
#include "Containers/Array.h"
#include "Containers/String.h"
#include "Core/Templates/LuminaTemplate.h"

namespace Lumina::Import
{
    /** Content-addressed dedup over small integer keys: buckets by hash, compares the full key on a hit.
     *  Equivalence is decided from source identifiers without touching the vertex or pixel data. */
    class FKeyDedup
    {
    public:

        explicit FKeyDedup(size_t Reserve) { Keys.reserve(Reserve); }

        // Returns the slot this key maps to, and whether the slot is newly created.
        uint32 Insert(TVector<uint32>&& Key, bool& bOutIsNew)
        {
            const uint64 Hash = HashKey(Key);
            TVector<uint32>& Bucket = Buckets[Hash];

            for (uint32 Slot : Bucket)
            {
                if (Keys[Slot] == Key)
                {
                    bOutIsNew = false;
                    return Slot;
                }
            }

            const uint32 Slot = (uint32)Keys.size();
            Keys.push_back(Move(Key));
            Bucket.push_back(Slot);
            bOutIsNew = true;
            return Slot;
        }

    private:

        FORCEINLINE static void HashCombine64(uint64& Seed, uint64 Value)
        {
            Seed ^= Value + 0x9E3779B97F4A7C15ull + (Seed << 6) + (Seed >> 2);
        }

        static uint64 HashKey(const TVector<uint32>& Key)
        {
            uint64 Seed = 0xCBF29CE484222325ull;
            for (uint32 Word : Key)
            {
                HashCombine64(Seed, Word);
            }
            return Seed;
        }

        THashMap<uint64, TVector<uint32>> Buckets;
        TVector<TVector<uint32>>          Keys;
    };

    /** Bucketed float for a dedup key. 1e-4 buckets: enough that authored duplicates collapse, tight
     *  enough that a deliberate difference never does. Rounds half away from zero, so it does not pull in
     *  the whole math header for one call. */
    FORCEINLINE uint32 QuantizeFloat(float Value)
    {
        const float Scaled = Value * 10000.0f;
        return (uint32)(int32)(Scaled + (Scaled >= 0.0f ? 0.5f : -0.5f));
    }

    /** Source name reduced to something usable as an asset-name fragment. */
    inline FFixedString SanitizedSourceName(const char* Raw, size_t Length)
    {
        FFixedString Name;
        if (Raw == nullptr)
        {
            return Name;
        }
        for (size_t i = 0; i < Length && Raw[i] != '\0'; ++i)
        {
            const char C = Raw[i];
            Name.push_back((C == '.' || C == '/' || C == '\\') ? '_' : C);
        }
        return Name;
    }

    inline FFixedString SanitizedSourceName(const char* Raw)
    {
        return SanitizedSourceName(Raw, Raw != nullptr ? strlen(Raw) : 0);
    }
}
