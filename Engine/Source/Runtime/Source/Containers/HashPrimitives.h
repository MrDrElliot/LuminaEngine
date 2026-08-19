#pragma once

#include <cstring>
#include <type_traits>

#include "Memory/Memcpy.h"
#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

namespace Lumina::Containers
{
    inline constexpr uint64 GHashGoldenRatio64 = 0x9e3779b97f4a7c15ull;

    /** splitmix64's finalizer: two multiply-xorshift rounds that avalanche every input bit across all 64. */
    NODISCARD FORCEINLINE constexpr uint64 MixHash64(uint64 Value) noexcept
    {
        Value ^= Value >> 30;
        Value *= 0xbf58476d1ce4e5b9ull;
        Value ^= Value >> 27;
        Value *= 0x94d049bb133111ebull;
        Value ^= Value >> 31;
        return Value;
    }

    /** Folds Value into Seed; the golden-ratio step keeps a run of equal values from collapsing. */
    NODISCARD FORCEINLINE constexpr uint64 CombineHash(uint64 Seed, uint64 Value) noexcept
    {
        return MixHash64(Seed + GHashGoldenRatio64 + MixHash64(Value));
    }

    NODISCARD inline uint64 HashBytes(const void* Data, size_t Size) noexcept
    {
        const uint8* Bytes = static_cast<const uint8*>(Data);
        uint64 Accumulator = GHashGoldenRatio64 ^ (static_cast<uint64>(Size) * 0xff51afd7ed558ccdull);

        while (Size >= 8)
        {
            uint64 Word;
            Memory::Memcpy(&Word, Bytes, sizeof(Word));
            Accumulator = MixHash64(Accumulator ^ Word);
            Bytes += 8;
            Size -= 8;
        }

        if (Size != 0)
        {
            uint64 Tail = 0;
            Memory::Memcpy(&Tail, Bytes, Size);
            Accumulator = MixHash64(Accumulator ^ Tail);
        }

        return Accumulator;
    }

    // Extension point: declare GetTypeHash next to your own type and ADL finds it from FDefaultHash.
    template <typename T>
    requires std::is_integral_v<T>
    NODISCARD FORCEINLINE constexpr uint64 GetTypeHash(T Value) noexcept
    {
        return MixHash64(static_cast<uint64>(Value));
    }

    template <typename T>
    requires std::is_enum_v<T>
    NODISCARD FORCEINLINE constexpr uint64 GetTypeHash(T Value) noexcept
    {
        return MixHash64(static_cast<uint64>(static_cast<std::underlying_type_t<T>>(Value)));
    }

    template <typename T>
    NODISCARD FORCEINLINE uint64 GetTypeHash(T* Value) noexcept
    {
        return MixHash64(static_cast<uint64>(reinterpret_cast<uintptr_t>(Value)));
    }

    NODISCARD FORCEINLINE uint64 GetTypeHash(std::nullptr_t) noexcept
    {
        return MixHash64(0);
    }

    // Negative zero has to land on the same bucket as positive zero, or lookups miss.
    NODISCARD FORCEINLINE uint64 GetTypeHash(float Value) noexcept
    {
        uint32 Bits;
        Memory::Memcpy(&Bits, &Value, sizeof(Bits));
        return MixHash64(Bits == 0x80000000u ? 0u : Bits);
    }

    NODISCARD FORCEINLINE uint64 GetTypeHash(double Value) noexcept
    {
        uint64 Bits;
        Memory::Memcpy(&Bits, &Value, sizeof(Bits));
        return MixHash64(Bits == 0x8000000000000000ull ? 0ull : Bits);
    }
}

namespace Lumina
{
    // Hoisted so an unqualified GetTypeHash inside Lumina reaches the scalar overloads; ADL cannot,
    // because a pointer to a Lumina type does not associate Lumina::Containers.
    using Containers::GetTypeHash;
}
