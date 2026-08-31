#include <gtest/gtest.h>

#include <atomic>
#include <vector>

#include "Containers/Algorithm.h"
#include "Containers/String.h"
#include "Containers/StringFormat.h"
#include "Containers/Vector.h"
#include "TaskSystem/ParallelSort.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaParallelSortTests
{
    namespace Algo = Lumina::Algo;
    using Lumina::FString;
    using Lumina::TVector;
    using Lumina::int32;
    using Lumina::uint32;
    using Lumina::uint64;

    uint32 NextRandom(uint64& State)
    {
        State = State * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32>(State >> 33);
    }

    TVector<int32> MakeRandom(size_t Count, uint32 Modulus, uint64 Seed)
    {
        uint64 State = Seed;
        TVector<int32> Values;
        Values.reserve(Count);
        for (size_t Index = 0; Index < Count; ++Index)
        {
            Values.push_back(static_cast<int32>(NextRandom(State) % Modulus));
        }

        return Values;
    }

    struct FCounted
    {
        static inline std::atomic<int32> Alive{ 0 };

        int32 Key = 0;

        FCounted() { ++Alive; }
        explicit FCounted(int32 InKey) : Key(InKey) { ++Alive; }
        FCounted(const FCounted& Other) : Key(Other.Key) { ++Alive; }
        FCounted(FCounted&& Other) noexcept : Key(Other.Key) { ++Alive; }
        FCounted& operator=(const FCounted&) = default;
        FCounted& operator=(FCounted&&) noexcept = default;
        ~FCounted() { --Alive; }
    };

    // Sizes that cross the serial threshold and land on odd chunk counts, where a run has no partner.
    const std::vector<size_t> kSizes = { 0, 1, 2, 17, 4095, 4096, 4097, 6143, 8192, 12345, 20000, 100000 };

    TEST(ParallelSort, MatchesTheSerialSortAcrossSizes)
    {
        for (size_t Count : kSizes)
        {
            TVector<int32> Ours = MakeRandom(Count, 100000, Count + 1);
            TVector<int32> Reference = Ours;

            Lumina::Task::ParallelSort(Ours.begin(), Ours.end());
            Algo::Sort(Reference);

            ASSERT_EQ(Ours.size(), Reference.size()) << "count " << Count;
            for (size_t Index = 0; Index < Ours.size(); ++Index)
            {
                ASSERT_EQ(Ours[Index], Reference[Index]) << "count " << Count << " at " << Index;
            }
        }
    }

    TEST(ParallelSort, HandlesAdversarialInputs)
    {
        constexpr size_t kCount = 50000;

        TVector<int32> Ascending;
        Ascending.reserve(kCount);
        for (size_t Index = 0; Index < kCount; ++Index)
        {
            Ascending.push_back(static_cast<int32>(Index));
        }

        TVector<int32> Descending;
        Descending.reserve(kCount);
        for (size_t Index = 0; Index < kCount; ++Index)
        {
            Descending.push_back(static_cast<int32>(kCount - Index));
        }

        TVector<int32> AllEqual;
        AllEqual.reserve(kCount);
        for (size_t Index = 0; Index < kCount; ++Index)
        {
            AllEqual.push_back(7);
        }

        TVector<int32> FewDistinct = MakeRandom(kCount, 3, 99);

        for (TVector<int32>* Case : { &Ascending, &Descending, &AllEqual, &FewDistinct })
        {
            TVector<int32> Reference = *Case;
            Lumina::Task::ParallelSort(Case->begin(), Case->end());
            Algo::Sort(Reference);

            for (size_t Index = 0; Index < Case->size(); ++Index)
            {
                ASSERT_EQ((*Case)[Index], Reference[Index]) << "at " << Index;
            }
        }
    }

    TEST(ParallelSort, TakesACustomComparator)
    {
        TVector<int32> Values = MakeRandom(30000, 5000, 4);

        Lumina::Task::ParallelSort(Values.begin(), Values.end(),
                                   [](int32 Left, int32 Right) { return Left > Right; });

        for (size_t Index = 1; Index < Values.size(); ++Index)
        {
            ASSERT_GE(Values[Index - 1], Values[Index]) << "at " << Index;
        }
    }

    TEST(ParallelSort, DestroysEveryElementItMovedThroughTheBuffer)
    {
        FCounted::Alive.store(0);
        {
            constexpr size_t kCount = 30000;

            TVector<FCounted> Values;
            Values.reserve(kCount);

            uint64 State = 12345;
            for (size_t Index = 0; Index < kCount; ++Index)
            {
                Values.push_back(FCounted(static_cast<int32>(NextRandom(State) % 10000)));
            }

            EXPECT_EQ(FCounted::Alive.load(), static_cast<int32>(kCount));

            Lumina::Task::ParallelSort(Values.begin(), Values.end(),
                                       [](const FCounted& Left, const FCounted& Right)
                                       {
                                           return Left.Key < Right.Key;
                                       });

            EXPECT_EQ(FCounted::Alive.load(), static_cast<int32>(kCount));
            for (size_t Index = 1; Index < Values.size(); ++Index)
            {
                ASSERT_LE(Values[Index - 1].Key, Values[Index].Key) << "at " << Index;
            }
        }

        EXPECT_EQ(FCounted::Alive.load(), 0);
    }

    TEST(ParallelSort, SortsAHeapAllocatingElementType)
    {
        constexpr size_t kCount = 20000;

        TVector<FString> Values;
        Values.reserve(kCount);

        uint64 State = 777;
        for (size_t Index = 0; Index < kCount; ++Index)
        {
            Values.push_back(Lumina::Format("a-fairly-long-key-that-heap-allocates-{:06}",
                                            NextRandom(State) % 1000000));
        }

        TVector<FString> Reference = Values;

        Lumina::Task::ParallelSort(Values.begin(), Values.end());
        Algo::Sort(Reference);

        ASSERT_EQ(Values.size(), Reference.size());
        for (size_t Index = 0; Index < Values.size(); ++Index)
        {
            ASSERT_EQ(Values[Index], Reference[Index]) << "at " << Index;
        }
    }

    TEST(ParallelSort, StaysCorrectBelowTheParallelThreshold)
    {
        TVector<int32> Values = MakeRandom(1000, 500, 8);
        TVector<int32> Reference = Values;

        Lumina::Task::ParallelSort(Values.begin(), Values.end());
        Algo::Sort(Reference);

        for (size_t Index = 0; Index < Values.size(); ++Index)
        {
            ASSERT_EQ(Values[Index], Reference[Index]) << "at " << Index;
        }
    }
}
