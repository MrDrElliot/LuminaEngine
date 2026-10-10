#include <gtest/gtest.h>
#include "Core/Math/Math.h"
#include "Core/Math/Packing.h"
#include "Renderer/Vertex.h"
#include "Renderer/VertexOps.h"
#include <bit>
#include <limits>
#include <random>
#include <vector>

using namespace Lumina;

namespace
{
    // An odd count past a multiple of eight so every kernel runs its scalar tail as well.
    constexpr size_t kRandomCount = (1u << 18) + 5;

    std::vector<uint32> MakePackedPairs()
    {
        std::vector<uint32> Packed;
        const uint16 Edges[] = { 0x0000, 0x0001, 0x7FFF, 0x7FFE, 0x8000, 0x8001, 0xFFFF, 0x4000, 0xC000, 0x3FFF, 0xC001 };
        for (uint16 Lo : Edges)
        {
            for (uint16 Hi : Edges)
            {
                Packed.push_back(uint32(Lo) | (uint32(Hi) << 16));
            }
        }

        std::mt19937 Rng(1234);
        while (Packed.size() < kRandomCount)
        {
            Packed.push_back(Rng());
        }
        return Packed;
    }

    std::vector<float> MakeVectorComponents(size_t Stride)
    {
        const float Nan = std::numeric_limits<float>::quiet_NaN();
        const float Inf = std::numeric_limits<float>::infinity();
        const float Special[][4] = {
            { 0.0f, 0.0f, 0.0f, 1.0f }, { -0.0f, -0.0f, -0.0f, -0.0f }, { 1.0f, 0.0f, 0.0f, -1.0f },
            { 0.0f, -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f, 1.0f }, { 1e-30f, -1e-30f, 1e-30f, -1.0f },
            { 1e30f, 2e30f, -3e30f, 1.0f }, { Nan, 0.0f, 1.0f, Nan }, { Inf, 1.0f, 0.0f, 1.0f },
            { 0.5f, 0.5f, -0.0f, 1.0f }, { -0.5f, 0.5f, -1e-7f, -1.0f }, { 0.25f, -0.75f, 0.0f, 1.0f },
        };

        std::vector<float> Out;
        for (const auto& S : Special)
        {
            Out.insert(Out.end(), S, S + Stride);
        }

        std::mt19937 Rng(5678);
        std::uniform_real_distribution<float> Unit(-1.0f, 1.0f);
        std::uniform_real_distribution<float> Wide(-100.0f, 100.0f);
        while (Out.size() < kRandomCount * Stride)
        {
            const bool bWide = (Rng() & 3) == 0;
            for (size_t c = 0; c < 3; ++c)
            {
                Out.push_back(bWide ? Wide(Rng) : Unit(Rng));
            }
            if (Stride == 4)
            {
                Out.push_back((Rng() & 1) ? 1.0f : -1.0f);
            }
        }
        return Out;
    }

    uint32 Bits(float F)
    {
        return std::bit_cast<uint32>(F);
    }
}

TEST(VertexOps, UnpackNormalsMatchesScalar)
{
    const std::vector<uint32> Packed = MakePackedPairs();
    std::vector<FVector3> Out(Packed.size());
    VertexOps::UnpackNormals(Packed.data(), Out.data(), Packed.size());

    for (size_t i = 0; i < Packed.size(); ++i)
    {
        const FVector3 Expected = UnpackNormal(Packed[i]);
        ASSERT_EQ(Bits(Out[i].x), Bits(Expected.x)) << "x of " << std::hex << Packed[i];
        ASSERT_EQ(Bits(Out[i].y), Bits(Expected.y)) << "y of " << std::hex << Packed[i];
        ASSERT_EQ(Bits(Out[i].z), Bits(Expected.z)) << "z of " << std::hex << Packed[i];
    }
}

TEST(VertexOps, PackNormalsMatchesScalar)
{
    const std::vector<float> Components = MakeVectorComponents(3);
    const size_t Count = Components.size() / 3;
    std::vector<FVector3> Normals(Count);
    for (size_t i = 0; i < Count; ++i)
    {
        Normals[i] = FVector3(Components[i * 3], Components[i * 3 + 1], Components[i * 3 + 2]);
    }

    std::vector<uint32> Out(Count);
    VertexOps::PackNormals(Normals.data(), Out.data(), Count);

    for (size_t i = 0; i < Count; ++i)
    {
        ASSERT_EQ(Out[i], PackNormal(Normals[i])) << "normal " << i << " (" << Normals[i].x << ", " << Normals[i].y << ", " << Normals[i].z << ")";
    }
}

TEST(VertexOps, PackTangentsMatchesScalar)
{
    const std::vector<float> Components = MakeVectorComponents(4);
    const size_t Count = Components.size() / 4;
    std::vector<uint32> Out(Count);
    VertexOps::PackTangents(Components.data(), Out.data(), Count);

    for (size_t i = 0; i < Count; ++i)
    {
        const float* T = &Components[i * 4];
        ASSERT_EQ(Out[i], PackTangent(FVector3(T[0], T[1], T[2]), T[3])) << "tangent " << i;
    }
}

TEST(VertexOps, UnpackHalf2x16sMatchesScalar)
{
    const std::vector<uint32> Packed = MakePackedPairs();
    std::vector<FVector2> Out(Packed.size());
    VertexOps::UnpackHalf2x16s(Packed.data(), Out.data(), Packed.size());

    for (size_t i = 0; i < Packed.size(); ++i)
    {
        const FVector2 Expected = Math::UnpackHalf2x16(Packed[i]);
        ASSERT_EQ(Bits(Out[i].x), Bits(Expected.x)) << std::hex << Packed[i];
        ASSERT_EQ(Bits(Out[i].y), Bits(Expected.y)) << std::hex << Packed[i];
    }
}
