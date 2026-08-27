#include <gtest/gtest.h>

#include "Containers/BoundedQueue.h"
#include "Containers/Vector.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"

using namespace Lumina;

namespace
{
    struct FMpmcPayload
    {
        int Value = 0;
    };

    /** Move-only, so the queue has to accept it without ever instantiating a copy. */
    struct FMpmcMoveOnly
    {
        FMpmcMoveOnly() = default;
        explicit FMpmcMoveOnly(int InValue) : Value(InValue) {}

        FMpmcMoveOnly(const FMpmcMoveOnly&) = delete;
        FMpmcMoveOnly& operator=(const FMpmcMoveOnly&) = delete;

        FMpmcMoveOnly(FMpmcMoveOnly&& Other) noexcept : Value(Other.Value) { Other.Value = 0; }

        FMpmcMoveOnly& operator=(FMpmcMoveOnly&& Other) noexcept
        {
            Value = Other.Value;
            Other.Value = 0;
            return *this;
        }

        int Value = 0;
    };
}

TEST(BoundedQueue, StartsUninitialized)
{
    TBoundedMPMCQueue<int> Queue;
    EXPECT_FALSE(Queue.IsInitialized());

    Queue.Initialize(8);
    EXPECT_TRUE(Queue.IsInitialized());

    Queue.Shutdown();
    EXPECT_FALSE(Queue.IsInitialized());
}

TEST(BoundedQueue, PreservesOrderAndReportsEmpty)
{
    TBoundedMPMCQueue<int> Queue;
    Queue.Initialize(8);

    int Out = -1;
    EXPECT_FALSE(Queue.TryDequeue(Out));

    for (int i = 0; i < 8; ++i)
    {
        ASSERT_TRUE(Queue.TryEnqueue(i)) << "item " << i;
    }

    for (int i = 0; i < 8; ++i)
    {
        ASSERT_TRUE(Queue.TryDequeue(Out)) << "item " << i;
        EXPECT_EQ(Out, i);
    }

    EXPECT_FALSE(Queue.TryDequeue(Out));
}

TEST(BoundedQueue, RefusesWhenFull)
{
    TBoundedMPMCQueue<int> Queue;
    Queue.Initialize(4);

    for (int i = 0; i < 4; ++i)
    {
        ASSERT_TRUE(Queue.TryEnqueue(i));
    }

    EXPECT_FALSE(Queue.TryEnqueue(99));

    int Out = -1;
    ASSERT_TRUE(Queue.TryDequeue(Out));
    EXPECT_EQ(Out, 0);

    // One slot freed, so exactly one more fits.
    EXPECT_TRUE(Queue.TryEnqueue(99));
    EXPECT_FALSE(Queue.TryEnqueue(100));
}

TEST(BoundedQueue, WrapsAcrossManyLaps)
{
    TBoundedMPMCQueue<int> Queue;
    Queue.Initialize(4);

    int Expected = 0;
    for (int Round = 0; Round < 500; ++Round)
    {
        ASSERT_TRUE(Queue.TryEnqueue(Round));

        int Out = -1;
        ASSERT_TRUE(Queue.TryDequeue(Out));
        EXPECT_EQ(Out, Expected++);
    }
}

TEST(BoundedQueue, DequeueReleasesTheStoredPayload)
{
    TBoundedMPMCQueue<TSharedPtr<FMpmcPayload>> Queue;
    Queue.Initialize(8);

    TSharedPtr<FMpmcPayload> Item = MakeShared<FMpmcPayload>();
    EXPECT_EQ(Item.GetRefCount(), 1u);

    ASSERT_TRUE(Queue.TryEnqueue(Item));
    EXPECT_EQ(Item.GetRefCount(), 2u);

    {
        TSharedPtr<FMpmcPayload> Out;
        ASSERT_TRUE(Queue.TryDequeue(Out));

        // The cell must not keep a reference, or a payload stays pinned until the ring laps.
        EXPECT_EQ(Item.GetRefCount(), 2u);
    }

    EXPECT_EQ(Item.GetRefCount(), 1u);
}

TEST(BoundedQueue, AcceptsMoveOnlyPayloads)
{
    TBoundedMPMCQueue<FMpmcMoveOnly> Queue;
    Queue.Initialize(8);

    ASSERT_TRUE(Queue.TryEnqueue(FMpmcMoveOnly(7)));

    FMpmcMoveOnly Source(11);
    ASSERT_TRUE(Queue.TryEnqueue(Move(Source)));
    EXPECT_EQ(Source.Value, 0) << "the queue should have moved from the source";

    FMpmcMoveOnly Out;
    ASSERT_TRUE(Queue.TryDequeue(Out));
    EXPECT_EQ(Out.Value, 7);

    ASSERT_TRUE(Queue.TryDequeue(Out));
    EXPECT_EQ(Out.Value, 11);
}

TEST(BoundedQueue, LosesNothingUnderManyProducersAndConsumers)
{
    constexpr uint32 kProducers = 4;
    constexpr uint32 kConsumers = 4;
    constexpr uint32 kPerProducer = 20000;
    constexpr uint32 kTotal = kProducers * kPerProducer;

    TBoundedMPMCQueue<uint32> Queue;
    Queue.Initialize(1024);

    // One slot per value, and a correct queue hands each value to exactly one consumer, so no slot races.
    TVector<uint8> Seen(kTotal, 0);

    TAtomic<uint32> Consumed{0};

    TVector<FThread> Threads;

    for (uint32 p = 0; p < kProducers; ++p)
    {
        Threads.push_back(FThread([&Queue, p]()
        {
            for (uint32 i = 0; i < kPerProducer; ++i)
            {
                const uint32 Value = p * kPerProducer + i;
                while (!Queue.TryEnqueue(Value))
                {
                    Threading::ThreadYield();
                }
            }
        }));
    }

    for (uint32 c = 0; c < kConsumers; ++c)
    {
        Threads.push_back(FThread([&Queue, &Seen, &Consumed]()
        {
            uint32 Value = 0;
            while (Consumed.load(Atomic::MemoryOrderAcquire) < kTotal)
            {
                if (Queue.TryDequeue(Value))
                {
                    Seen[Value] += 1;
                    Consumed.fetch_add(1, Atomic::MemoryOrderRelease);
                }
                else
                {
                    Threading::ThreadYield();
                }
            }
        }));
    }

    for (FThread& Thread : Threads)
    {
        Thread.Join();
    }

    EXPECT_EQ(Consumed.load(Atomic::MemoryOrderAcquire), kTotal);

    // Every value arrives exactly once; a duplicate means a slot was handed to two consumers.
    for (uint32 i = 0; i < kTotal; ++i)
    {
        ASSERT_EQ(Seen[i], 1u) << "value " << i;
    }
}

TEST(BoundedQueue, FixedCapacityNeedsNoInitialize)
{
    TBoundedMPMCQueue<int, 8> Queue;
    EXPECT_TRUE(Queue.IsInitialized());
    EXPECT_TRUE(Queue.IsFixedCapacity());
    EXPECT_EQ(Queue.GetCapacity(), 8u);

    for (int i = 0; i < 8; ++i)
    {
        ASSERT_TRUE(Queue.TryEnqueue(i));
    }
    EXPECT_FALSE(Queue.TryEnqueue(99));

    int Out = -1;
    for (int i = 0; i < 8; ++i)
    {
        ASSERT_TRUE(Queue.TryDequeue(Out));
        EXPECT_EQ(Out, i);
    }
}

TEST(BoundedQueue, CapacityRoundsUpToAPowerOfTwo)
{
    TBoundedMPMCQueue<int, 5> Fixed;
    EXPECT_EQ(Fixed.GetCapacity(), 8u);

    TBoundedMPMCQueue<int> Runtime;
    Runtime.Initialize(5);
    EXPECT_EQ(Runtime.GetCapacity(), 8u);
}

TEST(BoundedQueue, EveryShapeKeepsFifoOrder)
{
    TBoundedSPSCQueue<int, 16> Spsc;
    TBoundedMPSCQueue<int, 16> Mpsc;
    TBoundedSPMCQueue<int, 16> Spmc;
    TBoundedMPMCQueue<int, 16> Mpmc;

    auto RoundTrip = [](auto& Queue, const char* Name)
    {
        for (int i = 0; i < 16; ++i)
        {
            ASSERT_TRUE(Queue.TryEnqueue(i)) << Name << " item " << i;
        }
        EXPECT_FALSE(Queue.TryEnqueue(99)) << Name;

        int Out = -1;
        for (int i = 0; i < 16; ++i)
        {
            ASSERT_TRUE(Queue.TryDequeue(Out)) << Name << " item " << i;
            EXPECT_EQ(Out, i) << Name;
        }
        EXPECT_FALSE(Queue.TryDequeue(Out)) << Name;
    };

    RoundTrip(Spsc, "SPSC");
    RoundTrip(Mpsc, "MPSC");
    RoundTrip(Spmc, "SPMC");
    RoundTrip(Mpmc, "MPMC");
}

TEST(BoundedQueue, SpscMovesOwningPayloads)
{
    TBoundedSPSCQueue<TSharedPtr<FMpmcPayload>, 8> Queue;

    TSharedPtr<FMpmcPayload> Item = MakeShared<FMpmcPayload>();
    ASSERT_TRUE(Queue.TryEnqueue(Item));
    EXPECT_EQ(Item.GetRefCount(), 2u);

    {
        TSharedPtr<FMpmcPayload> Out;
        ASSERT_TRUE(Queue.TryDequeue(Out));
        EXPECT_EQ(Item.GetRefCount(), 2u);
    }

    EXPECT_EQ(Item.GetRefCount(), 1u);
}

TEST(BoundedQueue, SpscStreamsInOrderAcrossThreads)
{
    constexpr uint32 kCount = 200000;

    TBoundedSPSCQueue<uint32, 512> Queue;

    FThread Producer([&Queue]()
    {
        for (uint32 i = 0; i < kCount; ++i)
        {
            while (!Queue.TryEnqueue(i))
            {
                Threading::ThreadYield();
            }
        }
    });

    uint32 Expected = 0;
    uint32 Value = 0;
    while (Expected < kCount)
    {
        if (Queue.TryDequeue(Value))
        {
            ASSERT_EQ(Value, Expected) << "SPSC must preserve order exactly";
            ++Expected;
        }
    }

    Producer.Join();
    EXPECT_EQ(Expected, kCount);
}

TEST(BoundedQueue, MpscLosesNothingAcrossManyProducers)
{
    constexpr uint32 kProducers = 4;
    constexpr uint32 kPerProducer = 20000;
    constexpr uint32 kTotal = kProducers * kPerProducer;

    TBoundedMPSCQueue<uint32, 1024> Queue;

    TVector<uint8> Seen(kTotal, 0);
    TVector<FThread> Threads;

    for (uint32 p = 0; p < kProducers; ++p)
    {
        Threads.push_back(FThread([&Queue, p]()
        {
            for (uint32 i = 0; i < kPerProducer; ++i)
            {
                while (!Queue.TryEnqueue(p * kPerProducer + i))
                {
                    Threading::ThreadYield();
                }
            }
        }));
    }

    uint32 Consumed = 0;
    uint32 Value = 0;
    while (Consumed < kTotal)
    {
        if (Queue.TryDequeue(Value))
        {
            Seen[Value] += 1;
            ++Consumed;
        }
    }

    for (FThread& Thread : Threads)
    {
        Thread.Join();
    }

    for (uint32 i = 0; i < kTotal; ++i)
    {
        ASSERT_EQ(Seen[i], 1u) << "value " << i;
    }
}
