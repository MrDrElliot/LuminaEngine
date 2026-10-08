#include <gtest/gtest.h>

#include "Platform/Time/PlatformTime.h"
#include "Core/Math/Transform.h"
#include "World/Scene/RenderScene/SceneCullMath.h"

#include <cstdio>
#include <random>

using namespace Lumina;

namespace
{
    constexpr uint32 kActiveBit = (uint32)EInstanceFlags::Active << 16;

    double NanosPer(uint64 Start, uint64 End, size_t Ops)
    {
        return PlatformTime::ToSeconds(End - Start) * 1e9 / (double)Ops;
    }

    FInstanceBlockBounds ScalarBlockBounds(const FInstanceCullEntry* Entries, uint32 First, uint32 End)
    {
        FVector3 Lo(std::numeric_limits<float>::max());
        FVector3 Hi(-std::numeric_limits<float>::max());
        float Reach = -1.0f;
        for (uint32 Slot = First; Slot < End; ++Slot)
        {
            const FInstanceCullEntry& Entry = Entries[Slot];
            if ((Entry.DrawIDAndFlags & kActiveBit) == 0u)
            {
                continue;
            }
            const FVector3 Center(Entry.SphereBounds);
            Lo = Math::Min(Lo, Center - FVector3(Entry.SphereBounds.w));
            Hi = Math::Max(Hi, Center + FVector3(Entry.SphereBounds.w));
            Reach = Math::Max(Reach, Entry.MaxDrawDistance);
        }
        FInstanceBlockBounds Out;
        Out.Reach = Reach;
        Out.Min = Lo;
        Out.Max = Hi;
        return Out;
    }

    FVector4 ScalarSphere(const FMatrix4& M, const FVector3& C, float Radius)
    {
        const FVector3 Center = FVector3(M[0]) * C.x + FVector3(M[1]) * C.y + FVector3(M[2]) * C.z + FVector3(M[3]);
        const float ScaleSq = Math::Max(Math::Max(Math::Dot(FVector3(M[0]), FVector3(M[0])), Math::Dot(FVector3(M[1]), FVector3(M[1]))),
                                        Math::Dot(FVector3(M[2]), FVector3(M[2])));
        return FVector4(Center, Radius * Math::Sqrt(ScaleSq));
    }

    FTransform3x4 ScalarPack(const FMatrix4& M)
    {
        return { FVector4(M[0][0], M[1][0], M[2][0], M[3][0]), FVector4(M[0][1], M[1][1], M[2][1], M[3][1]), FVector4(M[0][2], M[1][2], M[2][2], M[3][2]) };
    }
}

TEST(SceneCullBench, DISABLED_InstanceBlockBounds)
{
    constexpr uint32 kSlots = 64 * 4096;
    constexpr int32 kPasses = 20;

    std::mt19937 Rng(5);
    std::uniform_real_distribution<float> Position(-500.0f, 500.0f);
    TVector<FInstanceCullEntry> Entries(kSlots);
    for (FInstanceCullEntry& Entry : Entries)
    {
        Entry.SphereBounds = FVector4(Position(Rng), Position(Rng), Position(Rng), 2.0f);
        Entry.DrawIDAndFlags = kActiveBit;
        Entry.MaxDrawDistance = 300.0f;
    }

    float Sink = 0.0f;
    const uint64 ScalarStart = PlatformTime::Cycles();
    for (int32 Pass = 0; Pass < kPasses; ++Pass)
    {
        for (uint32 First = 0; First < kSlots; First += 64)
        {
            Sink += ScalarBlockBounds(Entries.data(), First, First + 64).Max.x;
        }
    }
    const uint64 SimdStart = PlatformTime::Cycles();
    for (int32 Pass = 0; Pass < kPasses; ++Pass)
    {
        for (uint32 First = 0; First < kSlots; First += 64)
        {
            Sink += SceneCull::ComputeInstanceBlockBounds(Entries.data(), First, First + 64, false).Max.x;
        }
    }
    const uint64 End = PlatformTime::Cycles();

    std::printf("\n  block bounds %u slots:  reference %6.3f ns/slot   engine %6.3f ns/slot   (sink %.0f)\n", kSlots,
        NanosPer(ScalarStart, SimdStart, (size_t)kSlots * kPasses), NanosPer(SimdStart, End, (size_t)kSlots * kPasses), Sink);
    SUCCEED();
}

TEST(SceneCullBench, DISABLED_SphereAndPack)
{
    constexpr uint32 kCount = 65536;
    constexpr int32 kPasses = 50;

    std::mt19937 Rng(9);
    std::uniform_real_distribution<float> Unit(-1.0f, 1.0f);
    TVector<FMatrix4> Matrices(kCount);
    for (FMatrix4& M : Matrices)
    {
        FTransform Transform;
        Transform.SetLocation(FVector3(Unit(Rng), Unit(Rng), Unit(Rng)) * 100.0f);
        Transform.SetRotation(Math::Normalize(FQuat(Unit(Rng), Unit(Rng), Unit(Rng), Unit(Rng))));
        Transform.SetScale(FVector3(1.5f));
        M = Transform.GetMatrix();
    }
    TVector<FVector4> Spheres(kCount);
    TVector<FTransform3x4> Packed(kCount);
    const FVector3 LocalCenter(0.1f, 0.9f, -0.2f);

    const uint64 ScalarStart = PlatformTime::Cycles();
    for (int32 Pass = 0; Pass < kPasses; ++Pass)
    {
        for (uint32 i = 0; i < kCount; ++i)
        {
            Spheres[i] = ScalarSphere(Matrices[i], LocalCenter, 1.0f);
            Packed[i]  = ScalarPack(Matrices[i]);
        }
    }
    const uint64 SimdStart = PlatformTime::Cycles();
    for (int32 Pass = 0; Pass < kPasses; ++Pass)
    {
        for (uint32 i = 0; i < kCount; ++i)
        {
            Spheres[i] = SceneCull::TransformSphere(Matrices[i], LocalCenter, 1.0f);
            Packed[i]  = PackTransform3x4(Matrices[i]);
        }
    }
    const uint64 End = PlatformTime::Cycles();

    std::printf("\n  sphere+pack %u transforms:  reference %6.3f ns/op   engine %6.3f ns/op   (sink %.0f)\n", kCount,
        NanosPer(ScalarStart, SimdStart, (size_t)kCount * kPasses), NanosPer(SimdStart, End, (size_t)kCount * kPasses),
        Spheres[kCount / 2].w + Packed[kCount / 3].Row1.w);
    SUCCEED();
}
