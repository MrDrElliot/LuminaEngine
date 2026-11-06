#include <gtest/gtest.h>

#include <cstring>

#include "Core/Math/Hash/Hash.h"
#include "Core/Math/Hash/XXHashCore.h"

namespace LuminaXXHashTests
{
    using Lumina::XXHashCore::Hash32;
    using Lumina::XXHashCore::Hash64;

    struct FVector
    {
        const char* Input;
        uint64_t    Hash64;
        uint64_t    Hash64Seeded;
        uint32_t    Hash32;
        uint32_t    Hash32Seeded;
    };

    // Produced by the reference xxHash library before it was removed; these values are stored in cooked data and must never change.
    constexpr FVector kVectors[] =
    {
        { "", 0xEF46DB3751D8E999ull, 0xA2F5C44D0F46C872ull, 0x02CC5D05u, 0x8D3B42D8u },
        { "a", 0xD24EC4F1A98C6E5Bull, 0x24E13E82FA519133ull, 0x550D7456u, 0x12B7E114u },
        { "abc", 0x44BC2CF5AD770999ull, 0x735174907585E19Dull, 0x32D153FFu, 0x4D4CB222u },
        { "Lumina", 0x3525BC198B23815Full, 0x84867BF274EB0375ull, 0x5EFA9879u, 0xAED1A0F1u },
        { "message digest", 0x066ED728FCEEB3BEull, 0x38726609758C47F9ull, 0x7C948494u, 0xAD12CCBFu },
        { "abcdefghijklmnopqrstuvwxyz", 0xCFE1F278FA89835Cull, 0xEEF777245617A6D1ull, 0x63A14D5Fu, 0x82D26340u },
        { "The quick brown fox jumps over the lazy dog", 0x0B242D361FDA71BCull, 0xB87E5F0FA73B6F85ull, 0xE85EA4DEu, 0xC8579D72u },
        { "0123456789012345678901234567890123456789012345678901234567890123456789", 0x4916A0F3F0E1C781ull, 0x8F53F26F6F601136ull, 0x5699316Au, 0x1F105C2Fu },
    };

    TEST(XXHash, MatchesTheReferenceVectors)
    {
        for (const FVector& Vector : kVectors)
        {
            const size_t Length = std::strlen(Vector.Input);
            EXPECT_EQ(Hash64(Vector.Input, Length, 0), Vector.Hash64) << Vector.Input;
            EXPECT_EQ(Hash64(Vector.Input, Length, 0x1234567890ABCDEFull), Vector.Hash64Seeded) << Vector.Input;
            EXPECT_EQ(Hash32(Vector.Input, Length, 0), Vector.Hash32) << Vector.Input;
            EXPECT_EQ(Hash32(Vector.Input, Length, 0x9747B28Cu), Vector.Hash32Seeded) << Vector.Input;
        }
    }

    TEST(XXHash, IgnoresAlignment)
    {
        alignas(16) char Buffer[96] = {};
        const char* Text = "The quick brown fox jumps over the lazy dog";
        const size_t Length = std::strlen(Text);
        for (size_t Offset = 0; Offset < 16; ++Offset)
        {
            std::memcpy(Buffer + Offset, Text, Length);
            EXPECT_EQ(Hash64(Buffer + Offset, Length), 0x0B242D361FDA71BCull) << Offset;
            EXPECT_EQ(Hash32(Buffer + Offset, Length), 0xE85EA4DEu) << Offset;
        }
    }

    TEST(XXHash, EngineWrappersUseIt)
    {
        EXPECT_EQ(Lumina::Hash::XXHash::GetHash64("Lumina"), 0x3525BC198B23815Full);
        EXPECT_EQ(Lumina::Hash::XXHash::GetHash32("Lumina"), 0x5EFA9879u);
    }
}
