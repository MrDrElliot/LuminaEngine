#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "Platform/Time/PlatformTime.h"
#include "Memory/Memcpy.h"

using namespace Lumina;

namespace
{
    volatile uint64 GBenchSink = 0;

    // Every iteration copies a different slice to a different address, so none of it folds away.
    struct FSweepRegion
    {
        explicit FSweepRegion(size_t Size)
        {
            const size_t Span = Size < 4096 ? 16384 : Size * 4;

            Stride    = Size < 64 ? 64 : Size;
            SlotCount = Span / Stride;
            SlotCount = SlotCount < 2 ? 2 : SlotCount;

            Storage.resize(SlotCount * Stride + Size + 256);
            for (size_t Index = 0; Index < Storage.size(); ++Index)
            {
                Storage[Index] = static_cast<uint8>(Index * 7 + 1);
            }

            Source.resize(Storage.size());
            for (size_t Index = 0; Index < Source.size(); ++Index)
            {
                Source[Index] = static_cast<uint8>(Index * 13 + 5);
            }
        }

        std::vector<uint8> Storage;
        std::vector<uint8> Source;
        size_t             Stride    = 0;
        size_t             SlotCount = 0;
    };

    double NanosPer(Lumina::uint64 Start, Lumina::uint64 End, size_t Ops)
    {
        return (Lumina::PlatformTime::ToSeconds(End - Start) * 1e9) / static_cast<double>(Ops);
    }

    template <typename TCopy>
    double MeasureOne(FSweepRegion& Region, size_t Size, size_t Offset, size_t Iterations, TCopy&& Copy)
    {
        uint8* DestBase      = Region.Storage.data() + 128 + Offset;
        const uint8* SrcBase = Region.Source.data() + 128 + Offset;

        uint64 Checksum = 0;
        size_t Slot = 0;

        const Lumina::uint64 Start = Lumina::PlatformTime::Cycles();
        for (size_t Index = 0; Index < Iterations; ++Index)
        {
            const size_t Byte = Slot * Region.Stride;
            Copy(DestBase + Byte, SrcBase + Byte, Size);
            Checksum += DestBase[Byte];

            ++Slot;
            if (Slot == Region.SlotCount)
            {
                Slot = 0;
            }
        }
        const double Nanos = NanosPer(Start, Lumina::PlatformTime::Cycles(), Iterations);

        GBenchSink += Checksum;
        return Nanos;
    }

    void MeasurePair(size_t Size, size_t Offset, size_t Iterations, double& OutCrt, double& OutLumina)
    {
        FSweepRegion Region(Size);

        double BestCrt    = 1e30;
        double BestLumina = 1e30;

        for (int32 Round = 0; Round < 5; ++Round)
        {
            const double Crt = MeasureOne(Region, Size, Offset, Iterations,
                [](uint8* D, const uint8* S, size_t N) { std::memcpy(D, S, N); });

            const double Lumina = MeasureOne(Region, Size, Offset, Iterations,
                [](uint8* D, const uint8* S, size_t N) { Memory::Memcpy(D, S, N); });

            BestCrt    = Crt < BestCrt ? Crt : BestCrt;
            BestLumina = Lumina < BestLumina ? Lumina : BestLumina;
        }

        OutCrt    = BestCrt;
        OutLumina = BestLumina;
    }

    void RunSweep(const char* Label, size_t Offset)
    {
        struct FCase { size_t Size; size_t Iterations; };

        const FCase Cases[] =
        {
            {     8, 2000000 }, {    16, 2000000 }, {    24, 2000000 }, {    32, 2000000 },
            {    48, 2000000 }, {    64, 2000000 }, {    96, 1000000 }, {   128, 1000000 },
            {   192, 1000000 }, {   256, 1000000 }, {   384,  500000 }, {   512,  500000 },
            {   768,  300000 }, {  1024,  300000 }, {  1536,  200000 }, {  2048,  200000 },
            {  4096,  100000 }, {  8192,   50000 }, { 16384,   30000 }, { 65536,   10000 },
            { 262144,   3000 }, { 1048576,   600 },
        };

        std::printf("\n[ MEMCPY   ] %s (dst/src offset %zu)\n", Label, Offset);
        std::printf("[ MEMCPY   ] %9s %12s %12s %10s\n", "size", "CRT ns", "Lumina ns", "speedup");

        for (const FCase& Case : Cases)
        {
            double Crt = 0.0;
            double Lumina = 0.0;
            MeasurePair(Case.Size, Offset, Case.Iterations, Crt, Lumina);

            std::printf("[ MEMCPY   ] %9zu %12.2f %12.2f %9.2fx\n",
                Case.Size, Crt, Lumina, Crt / Lumina);
        }
    }
}

TEST(MemcpyBench, FineSweepAroundTheSmallTiers)
{
    std::printf("\n[ MEMCPY   ] fine sweep 64..320, misaligned\n");
    std::printf("[ MEMCPY   ] %9s %12s %12s %10s\n", "size", "CRT ns", "Lumina ns", "speedup");

    for (size_t Size = 64; Size <= 320; Size += 8)
    {
        double Crt = 0.0;
        double Lumina = 0.0;
        MeasurePair(Size, 3, 1000000, Crt, Lumina);
        std::printf("[ MEMCPY   ] %9zu %12.2f %12.2f %9.2fx\n", Size, Crt, Lumina, Crt / Lumina);
    }
}

TEST(MemcpyBench, AgainstTheCrtAcrossSizes)
{
    RunSweep("both pointers 32B aligned", 0);
    RunSweep("both pointers misaligned", 3);
}
