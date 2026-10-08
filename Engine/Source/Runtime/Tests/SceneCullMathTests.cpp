#include <gtest/gtest.h>

#include "Core/Math/Transform.h"
#include "World/Scene/RenderScene/SceneCullMath.h"

#include <random>

using namespace Lumina;

namespace
{
    constexpr uint32 kActiveBit = (uint32)EInstanceFlags::Active << 16;

    TVector<FInstanceCullEntry> MakeEntries(uint32 Count, uint32 Seed)
    {
        std::mt19937 Rng(Seed);
        std::uniform_real_distribution<float> Position(-500.0f, 500.0f);
        std::uniform_real_distribution<float> Radius(0.1f, 20.0f);
        std::uniform_real_distribution<float> Distance(-10.0f, 400.0f);

        TVector<FInstanceCullEntry> Entries(Count);
        for (FInstanceCullEntry& Entry : Entries)
        {
            Entry.SphereBounds    = FVector4(Position(Rng), Position(Rng), Position(Rng), Radius(Rng));
            Entry.DrawIDAndFlags  = (Rng() % 5u) != 0u ? kActiveBit : 0u;
            Entry.MaxDrawDistance = Distance(Rng);
        }
        return Entries;
    }

    FInstanceBlockBounds ScalarBlockBounds(const FInstanceCullEntry* Entries, uint32 First, uint32 End)
    {
        FVector3 Lo(std::numeric_limits<float>::max());
        FVector3 Hi(-std::numeric_limits<float>::max());
        float Reach = -1.0f;
        bool bUnbounded = false;
        for (uint32 Slot = First; Slot < End; ++Slot)
        {
            const FInstanceCullEntry& Entry = Entries[Slot];
            if ((Entry.DrawIDAndFlags & kActiveBit) == 0u)
            {
                continue;
            }
            bUnbounded = bUnbounded || Entry.MaxDrawDistance <= 0.0f;
            const FVector3 Center(Entry.SphereBounds);
            Lo = Math::Min(Lo, Center - FVector3(Entry.SphereBounds.w));
            Hi = Math::Max(Hi, Center + FVector3(Entry.SphereBounds.w));
            Reach = Math::Max(Reach, Entry.MaxDrawDistance);
        }

        FInstanceBlockBounds Out;
        Out.bAlwaysScan = bUnbounded ? 1u : 0u;
        Out.Reach = Reach;
        Out.Min = Reach < 0.0f ? FVector3(0.0f) : Lo;
        Out.Max = Reach < 0.0f ? FVector3(0.0f) : Hi;
        return Out;
    }

    FMatrix4 MakeTransform(std::mt19937& Rng)
    {
        std::uniform_real_distribution<float> Unit(-1.0f, 1.0f);
        std::uniform_real_distribution<float> Scale(0.2f, 3.0f);
        FTransform Transform;
        Transform.SetLocation(FVector3(Unit(Rng), Unit(Rng), Unit(Rng)) * 200.0f);
        Transform.SetRotation(Math::Normalize(FQuat(Unit(Rng), Unit(Rng), Unit(Rng), Unit(Rng))));
        Transform.SetScale(FVector3(Scale(Rng), Scale(Rng), Scale(Rng)));
        return Transform.GetMatrix();
    }
}

TEST(SceneCullMath, BlockBoundsMatchTheScalarLoopExactly)
{
    const TVector<FInstanceCullEntry> Entries = MakeEntries(64 * 40 + 13, 7);
    for (uint32 First = 0; First < (uint32)Entries.size(); First += 64)
    {
        const uint32 End = Math::Min(First + 64u, (uint32)Entries.size());
        const FInstanceBlockBounds Simd   = SceneCull::ComputeInstanceBlockBounds(Entries.data(), First, End, false);
        const FInstanceBlockBounds Scalar = ScalarBlockBounds(Entries.data(), First, End);

        EXPECT_EQ(Simd.Min, Scalar.Min);
        EXPECT_EQ(Simd.Max, Scalar.Max);
        EXPECT_EQ(Simd.Reach, Scalar.Reach);
        EXPECT_EQ(Simd.bAlwaysScan, Scalar.bAlwaysScan);
    }
}

TEST(SceneCullMath, AnEmptyBlockIsUnreachable)
{
    TVector<FInstanceCullEntry> Entries = MakeEntries(64, 3);
    for (FInstanceCullEntry& Entry : Entries)
    {
        Entry.DrawIDAndFlags = 0u;
    }
    const FInstanceBlockBounds Bounds = SceneCull::ComputeInstanceBlockBounds(Entries.data(), 0, 64, false);
    EXPECT_LT(Bounds.Reach, 0.0f);
    EXPECT_EQ(Bounds.Min, FVector3(0.0f));
    EXPECT_EQ(Bounds.Max, FVector3(0.0f));
}

TEST(SceneCullMath, PackTransformIsTheExactTranspose)
{
    std::mt19937 Rng(11);
    for (int32 i = 0; i < 64; ++i)
    {
        const FMatrix4 M = MakeTransform(Rng);
        const FTransform3x4 Packed = PackTransform3x4(M);
        for (int32 Column = 0; Column < 4; ++Column)
        {
            EXPECT_EQ(Packed.Row0[Column], M[Column][0]);
            EXPECT_EQ(Packed.Row1[Column], M[Column][1]);
            EXPECT_EQ(Packed.Row2[Column], M[Column][2]);
        }
    }
}

TEST(SceneCullMath, TransformSphereMatchesTheScalarFormula)
{
    std::mt19937 Rng(23);
    std::uniform_real_distribution<float> Unit(-1.0f, 1.0f);
    for (int32 i = 0; i < 256; ++i)
    {
        const FMatrix4 M = MakeTransform(Rng);
        const FVector3 LocalCenter(Unit(Rng) * 5.0f, Unit(Rng) * 5.0f, Unit(Rng) * 5.0f);
        const float LocalRadius = 1.0f + Unit(Rng) * 0.5f;

        const FVector3 Center = FVector3(M[0]) * LocalCenter.x + FVector3(M[1]) * LocalCenter.y + FVector3(M[2]) * LocalCenter.z + FVector3(M[3]);
        const float ScaleSq = Math::Max(Math::Max(Math::Dot(FVector3(M[0]), FVector3(M[0])), Math::Dot(FVector3(M[1]), FVector3(M[1]))),
                                        Math::Dot(FVector3(M[2]), FVector3(M[2])));

        const FVector4 Sphere = SceneCull::TransformSphere(M, LocalCenter, LocalRadius);
        EXPECT_NEAR(Sphere.x, Center.x, 1e-3f);
        EXPECT_NEAR(Sphere.y, Center.y, 1e-3f);
        EXPECT_NEAR(Sphere.z, Center.z, 1e-3f);
        EXPECT_NEAR(Sphere.w, LocalRadius * Math::Sqrt(ScaleSq), 1e-4f);
    }
}
