#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Bit-exact XXH32 and XXH64 (stored on disk and sent over the network), free of engine headers so the Reflector hashes names alike.
namespace Lumina::XXHashCore
{
    namespace Internal
    {
        inline constexpr uint32_t kPrime32_1 = 0x9E3779B1u;
        inline constexpr uint32_t kPrime32_2 = 0x85EBCA77u;
        inline constexpr uint32_t kPrime32_3 = 0xC2B2AE3Du;
        inline constexpr uint32_t kPrime32_4 = 0x27D4EB2Fu;
        inline constexpr uint32_t kPrime32_5 = 0x165667B1u;

        inline constexpr uint64_t kPrime64_1 = 0x9E3779B185EBCA87ull;
        inline constexpr uint64_t kPrime64_2 = 0xC2B2AE3D27D4EB4Full;
        inline constexpr uint64_t kPrime64_3 = 0x165667B19E3779F9ull;
        inline constexpr uint64_t kPrime64_4 = 0x85EBCA77C2B2AE63ull;
        inline constexpr uint64_t kPrime64_5 = 0x27D4EB2F165667C5ull;

        // The algorithm reads lanes little-endian whatever the host is.
        inline uint32_t Read32(const uint8_t* Bytes) noexcept
        {
            uint32_t Value;
            std::memcpy(&Value, Bytes, sizeof(Value));
            if constexpr (std::endian::native == std::endian::big)
            {
                Value = std::byteswap(Value);
            }
            return Value;
        }

        inline uint64_t Read64(const uint8_t* Bytes) noexcept
        {
            uint64_t Value;
            std::memcpy(&Value, Bytes, sizeof(Value));
            if constexpr (std::endian::native == std::endian::big)
            {
                Value = std::byteswap(Value);
            }
            return Value;
        }

        inline uint32_t Round32(uint32_t Accumulator, uint32_t Lane) noexcept
        {
            Accumulator += Lane * kPrime32_2;
            return std::rotl(Accumulator, 13) * kPrime32_1;
        }

        inline uint64_t Round64(uint64_t Accumulator, uint64_t Lane) noexcept
        {
            Accumulator += Lane * kPrime64_2;
            return std::rotl(Accumulator, 31) * kPrime64_1;
        }

        inline uint64_t MergeRound64(uint64_t Accumulator, uint64_t Value) noexcept
        {
            Accumulator ^= Round64(0, Value);
            return Accumulator * kPrime64_1 + kPrime64_4;
        }
    }

    inline uint32_t Hash32(const void* Data, size_t Size, uint32_t Seed = 0) noexcept
    {
        using namespace Internal;
        const uint8_t* Cursor = static_cast<const uint8_t*>(Data);
        const uint8_t* const End = Cursor + Size;
        uint32_t Hash;

        if (Size >= 16)
        {
            uint32_t V1 = Seed + kPrime32_1 + kPrime32_2;
            uint32_t V2 = Seed + kPrime32_2;
            uint32_t V3 = Seed;
            uint32_t V4 = Seed - kPrime32_1;
            const uint8_t* const Limit = End - 16;
            do
            {
                V1 = Round32(V1, Read32(Cursor));
                V2 = Round32(V2, Read32(Cursor + 4));
                V3 = Round32(V3, Read32(Cursor + 8));
                V4 = Round32(V4, Read32(Cursor + 12));
                Cursor += 16;
            }
            while (Cursor <= Limit);
            Hash = std::rotl(V1, 1) + std::rotl(V2, 7) + std::rotl(V3, 12) + std::rotl(V4, 18);
        }
        else
        {
            Hash = Seed + kPrime32_5;
        }

        Hash += static_cast<uint32_t>(Size);

        for (; Cursor + 4 <= End; Cursor += 4)
        {
            Hash += Read32(Cursor) * kPrime32_3;
            Hash = std::rotl(Hash, 17) * kPrime32_4;
        }
        for (; Cursor < End; ++Cursor)
        {
            Hash += *Cursor * kPrime32_5;
            Hash = std::rotl(Hash, 11) * kPrime32_1;
        }

        Hash ^= Hash >> 15;
        Hash *= kPrime32_2;
        Hash ^= Hash >> 13;
        Hash *= kPrime32_3;
        Hash ^= Hash >> 16;
        return Hash;
    }

    inline uint64_t Hash64(const void* Data, size_t Size, uint64_t Seed = 0) noexcept
    {
        using namespace Internal;
        const uint8_t* Cursor = static_cast<const uint8_t*>(Data);
        const uint8_t* const End = Cursor + Size;
        uint64_t Hash;

        if (Size >= 32)
        {
            uint64_t V1 = Seed + kPrime64_1 + kPrime64_2;
            uint64_t V2 = Seed + kPrime64_2;
            uint64_t V3 = Seed;
            uint64_t V4 = Seed - kPrime64_1;
            const uint8_t* const Limit = End - 32;
            do
            {
                V1 = Round64(V1, Read64(Cursor));
                V2 = Round64(V2, Read64(Cursor + 8));
                V3 = Round64(V3, Read64(Cursor + 16));
                V4 = Round64(V4, Read64(Cursor + 24));
                Cursor += 32;
            }
            while (Cursor <= Limit);
            Hash = std::rotl(V1, 1) + std::rotl(V2, 7) + std::rotl(V3, 12) + std::rotl(V4, 18);
            Hash = MergeRound64(Hash, V1);
            Hash = MergeRound64(Hash, V2);
            Hash = MergeRound64(Hash, V3);
            Hash = MergeRound64(Hash, V4);
        }
        else
        {
            Hash = Seed + kPrime64_5;
        }

        Hash += static_cast<uint64_t>(Size);

        for (; Cursor + 8 <= End; Cursor += 8)
        {
            Hash ^= Round64(0, Read64(Cursor));
            Hash = std::rotl(Hash, 27) * kPrime64_1 + kPrime64_4;
        }
        if (Cursor + 4 <= End)
        {
            Hash ^= static_cast<uint64_t>(Read32(Cursor)) * kPrime64_1;
            Hash = std::rotl(Hash, 23) * kPrime64_2 + kPrime64_3;
            Cursor += 4;
        }
        for (; Cursor < End; ++Cursor)
        {
            Hash ^= *Cursor * kPrime64_5;
            Hash = std::rotl(Hash, 11) * kPrime64_1;
        }

        Hash ^= Hash >> 33;
        Hash *= kPrime64_2;
        Hash ^= Hash >> 29;
        Hash *= kPrime64_3;
        Hash ^= Hash >> 32;
        return Hash;
    }
}
