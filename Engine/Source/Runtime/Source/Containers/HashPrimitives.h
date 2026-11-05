#pragma once

#include <bit>
#include <cstring>
#include <type_traits>
#include <utility>

#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

#if defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
    #include <intrin.h>
#endif

namespace Lumina::Containers
{
    inline constexpr uint64 GHashGoldenRatio64 = 0x9e3779b97f4a7c15ull;

    namespace HashInternal
    {
        inline constexpr uint64 kSecret0 = 0x2d358dccaa6c78a5ull;
        inline constexpr uint64 kSecret1 = 0x8bb84b93962eacc9ull;
        inline constexpr uint64 kSecret2 = 0x4b33a62ed433d4a3ull;

        NODISCARD FORCEINLINE constexpr uint64 PortableMultiply(uint64 A, uint64 B, uint64& OutHigh) noexcept
        {
            const uint64 LowA = A & 0xFFFFFFFFull;
            const uint64 HighA = A >> 32;
            const uint64 LowB = B & 0xFFFFFFFFull;
            const uint64 HighB = B >> 32;
            const uint64 LowLow = LowA * LowB;
            const uint64 LowHigh = LowA * HighB;
            const uint64 HighLow = HighA * LowB;
            const uint64 Middle = (LowLow >> 32) + (LowHigh & 0xFFFFFFFFull) + (HighLow & 0xFFFFFFFFull);
            OutHigh = HighA * HighB + (LowHigh >> 32) + (HighLow >> 32) + (Middle >> 32);
            return (Middle << 32) | (LowLow & 0xFFFFFFFFull);
        }

        /** The full 128-bit product, low half returned and high half in OutHigh. */
        NODISCARD FORCEINLINE constexpr uint64 Multiply128(uint64 A, uint64 B, uint64& OutHigh) noexcept
        {
#if defined(__SIZEOF_INT128__)
            const unsigned __int128 Product = static_cast<unsigned __int128>(A) * B;
            OutHigh = static_cast<uint64>(Product >> 64);
            return static_cast<uint64>(Product);
#elif defined(_MSC_VER) && defined(_M_X64) && !defined(__clang__)
            if consteval
            {
                return PortableMultiply(A, B, OutHigh);
            }
            else
            {
                return _umul128(A, B, &OutHigh);
            }
#else
            return PortableMultiply(A, B, OutHigh);
#endif
        }

        NODISCARD FORCEINLINE constexpr uint64 MultiplyFold(uint64 A, uint64 B) noexcept
        {
            uint64 High = 0;
            const uint64 Low = Multiply128(A, B, High);
            return Low ^ High;
        }

        NODISCARD FORCEINLINE uint64 Read64(const uint8* Bytes) noexcept
        {
            uint64 Value;
            std::memcpy(&Value, Bytes, sizeof(Value));
            return Value;
        }

        NODISCARD FORCEINLINE uint64 Read32(const uint8* Bytes) noexcept
        {
            uint32 Value;
            std::memcpy(&Value, Bytes, sizeof(Value));
            return Value;
        }
    }

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

    /** rapidhash's construction: 16 bytes per multiply on short keys, three independent lanes on long ones. Not stable across versions. */
    NODISCARD inline uint64 HashBytes(const void* Data, size_t Size) noexcept
    {
        using namespace HashInternal;

        const uint8* Bytes = static_cast<const uint8*>(Data);
        uint64 Seed = 0xbdd89aa982704029ull;
        Seed ^= MultiplyFold(Seed ^ kSecret0, kSecret1) ^ static_cast<uint64>(Size);

        uint64 A = 0;
        uint64 B = 0;
        if (Size <= 16) [[likely]]
        {
            if (Size >= 4)
            {
                const size_t Delta = (Size & 24) >> (Size >> 3);
                A = (Read32(Bytes) << 32) | Read32(Bytes + Size - 4);
                B = (Read32(Bytes + Delta) << 32) | Read32(Bytes + Size - 4 - Delta);
            }
            else if (Size > 0)
            {
                A = (static_cast<uint64>(Bytes[0]) << 56) | (static_cast<uint64>(Bytes[Size >> 1]) << 32) | Bytes[Size - 1];
            }
        }
        else
        {
            size_t Remaining = Size;
            if (Remaining > 48)
            {
                uint64 LaneOne = Seed;
                uint64 LaneTwo = Seed;
                do
                {
                    Seed = MultiplyFold(Read64(Bytes) ^ kSecret0, Read64(Bytes + 8) ^ Seed);
                    LaneOne = MultiplyFold(Read64(Bytes + 16) ^ kSecret1, Read64(Bytes + 24) ^ LaneOne);
                    LaneTwo = MultiplyFold(Read64(Bytes + 32) ^ kSecret2, Read64(Bytes + 40) ^ LaneTwo);
                    Bytes += 48;
                    Remaining -= 48;
                }
                while (Remaining >= 48);
                Seed ^= LaneOne ^ LaneTwo;
            }
            if (Remaining > 16)
            {
                Seed = MultiplyFold(Read64(Bytes) ^ kSecret2, Read64(Bytes + 8) ^ Seed ^ kSecret1);
                if (Remaining > 32)
                {
                    Seed = MultiplyFold(Read64(Bytes + 16) ^ kSecret2, Read64(Bytes + 24) ^ Seed);
                }
            }
            A = Read64(Bytes + Remaining - 16);
            B = Read64(Bytes + Remaining - 8);
        }

        A ^= kSecret1;
        B ^= Seed;
        uint64 High = 0;
        A = Multiply128(A, B, High);
        return MultiplyFold(A ^ kSecret0 ^ static_cast<uint64>(Size), High ^ kSecret1);
    }

    /** One 128-bit multiply folded to 64 bits, which keeps the high input bits in the low output bits a table indexes by. */
    NODISCARD FORCEINLINE constexpr uint64 HashScalar(uint64 Value) noexcept
    {
        return HashInternal::MultiplyFold(Value ^ HashInternal::kSecret0, GHashGoldenRatio64);
    }

    // Extension point: declare GetTypeHash next to your own type and ADL finds it from FDefaultHash.
    template <typename T>
    requires std::is_integral_v<T>
    NODISCARD FORCEINLINE constexpr uint64 GetTypeHash(T Value) noexcept
    {
        return HashScalar(static_cast<uint64>(Value));
    }

    template <typename T>
    requires std::is_enum_v<T>
    NODISCARD FORCEINLINE constexpr uint64 GetTypeHash(T Value) noexcept
    {
        return HashScalar(static_cast<uint64>(std::to_underlying(Value)));
    }

    template <typename T>
    NODISCARD FORCEINLINE uint64 GetTypeHash(T* Value) noexcept
    {
        return HashScalar(reinterpret_cast<uintptr_t>(Value));
    }

    NODISCARD FORCEINLINE constexpr uint64 GetTypeHash(std::nullptr_t) noexcept
    {
        return HashScalar(0);
    }

    // Negative zero has to land on the same bucket as positive zero, or lookups miss.
    NODISCARD FORCEINLINE constexpr uint64 GetTypeHash(float Value) noexcept
    {
        const uint32 Bits = std::bit_cast<uint32>(Value);
        return HashScalar(Bits == 0x80000000u ? 0u : Bits);
    }

    NODISCARD FORCEINLINE constexpr uint64 GetTypeHash(double Value) noexcept
    {
        const uint64 Bits = std::bit_cast<uint64>(Value);
        return HashScalar(Bits == 0x8000000000000000ull ? 0ull : Bits);
    }
}

namespace Lumina
{
    // Hoisted so an unqualified GetTypeHash inside Lumina reaches the scalar overloads; ADL cannot,
    // because a pointer to a Lumina type does not associate Lumina::Containers.
    using Containers::GetTypeHash;
}
