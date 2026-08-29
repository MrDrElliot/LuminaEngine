#include <gtest/gtest.h>
#include <cstdio>

#include "Containers/Vector.h"
#include "Memory/Allocators/ScratchArray.h"
#include "Platform/Time/PlatformTime.h"

using namespace Lumina;

// Sized to one 64^3 terrain chunk's three noise buffers.
TEST(ScratchArrayBench, AgainstVector)
{
    constexpr SIZE_T Count = 64 * 64 * 64;
    constexpr int32  Runs  = 200;

    const auto Now = [] { return Lumina::PlatformTime::Cycles(); };
    const auto Ms  = [](auto Start, auto End)
    {
        return Lumina::PlatformTime::ToMilliseconds(End - Start);
    };

    double Sink = 0.0;

    const auto VectorStart = Now();
    for (int32 Run = 0; Run < Runs; ++Run)
    {
        TVector<float> A(Count, 0.0f);
        TVector<float> B(Count, 0.0f);
        TVector<float> C(Count, 0.0f);
        A[Run] = 1.0f; B[Run] = 2.0f; C[Run] = 3.0f;
        Sink += A[Run] + B[Run] + C[Run];
    }
    const double VectorMs = Ms(VectorStart, Now());

    ScratchPool::Trim();

    const auto ScratchStart = Now();
    for (int32 Run = 0; Run < Runs; ++Run)
    {
        TScratchArray<float> A(Count);
        TScratchArray<float> B(Count);
        TScratchArray<float> C(Count);
        A[Run] = 1.0f; B[Run] = 2.0f; C[Run] = 3.0f;
        Sink += A[Run] + B[Run] + C[Run];
    }
    const double ScratchMs = Ms(ScratchStart, Now());

    std::printf("\n  3 x %llu floats, %d runs\n", (unsigned long long)Count, Runs);
    std::printf("  TVector      %8.3f ms  (%6.4f ms/chunk)\n", VectorMs, VectorMs / Runs);
    std::printf("  TScratchArray%8.3f ms  (%6.4f ms/chunk)  %.1fx\n",
                ScratchMs, ScratchMs / Runs, VectorMs / ScratchMs);
    std::printf("  retained after: %llu KiB\n\n",
                (unsigned long long)(ScratchPool::GetStats().RetainedBytes / 1024));

    EXPECT_GT(Sink, 0.0);
}
