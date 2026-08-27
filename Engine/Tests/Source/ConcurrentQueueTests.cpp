#include <gtest/gtest.h>

#include "Containers/ConcurrentQueue.h"
#include "Containers/Vector.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"

using namespace Lumina;

namespace
{
    struct FConcurrentPayload
    {
        int Value = 0;
    };
}

TEST(ConcurrentQueue, RoundTripsInOrderWithinTheRing)
{
    TConcurrentQueue<int> Queue(16);

    int Out = -1;
    EXPECT_FALSE(Queue.TryDequeue(Out));

    for (int i = 0; i < 16; ++i)
    {
        Queue.Enqueue(i);
    }

    for (int i = 0; i < 16; ++i)
    {
        ASSERT_TRUE(Queue.TryDequeue(Out)) << "item " << i;
        EXPECT_EQ(Out, i);
    }

    EXPECT_FALSE(Queue.TryDequeue(Out));
}

TEST(ConcurrentQueue, GrowsPastTheRingWithoutDropping)
{
    constexpr int kCount = 10000;

    // A tiny ring forces almost everything through the spill.
    TConcurrentQueue<int> Queue(8);

    for (int i = 0; i < kCount; ++i)
    {
        Queue.Enqueue(i);
    }

    EXPECT_EQ(Queue.SizeApprox(), (uint32)kCount);

    int Out = -1;
    for (int i = 0; i < kCount; ++i)
    {
        ASSERT_TRUE(Queue.TryDequeue(Out)) << "item " << i;
        EXPECT_EQ(Out, i) << "spill must stay in order behind the ring";
    }

    EXPECT_FALSE(Queue.TryDequeue(Out));
    EXPECT_EQ(Queue.SizeApprox(), 0u);
}

TEST(ConcurrentQueue, AlternatingPushAndPopStaysBounded)
{
    TConcurrentQueue<int> Queue(8);

    int Out = -1;
    for (int Round = 0; Round < 5000; ++Round)
    {
        Queue.Enqueue(Round);
        ASSERT_TRUE(Queue.TryDequeue(Out));
        EXPECT_EQ(Out, Round);
    }

    EXPECT_EQ(Queue.SizeApprox(), 0u);
}

TEST(ConcurrentQueue, MovesOwningPayloads)
{
    TConcurrentQueue<TSharedPtr<FConcurrentPayload>> Queue(4);

    TSharedPtr<FConcurrentPayload> Item = MakeShared<FConcurrentPayload>();
    Queue.Enqueue(Item);
    EXPECT_EQ(Item.GetRefCount(), 2u);

    {
        TSharedPtr<FConcurrentPayload> Out;
        ASSERT_TRUE(Queue.TryDequeue(Out));
        EXPECT_EQ(Item.GetRefCount(), 2u);
    }

    EXPECT_EQ(Item.GetRefCount(), 1u);
}

TEST(ConcurrentQueue, BulkOperationsMatchSingleOnes)
{
    TConcurrentQueue<int> Queue(8);

    int Source[64];
    for (int i = 0; i < 64; ++i)
    {
        Source[i] = i;
    }

    Queue.EnqueueBulk(Source, 64);
    EXPECT_EQ(Queue.SizeApprox(), 64u);

    int Batch[16];
    int Expected = 0;
    for (int Round = 0; Round < 4; ++Round)
    {
        const size_t Count = Queue.DequeueBulk(Batch, 16);
        ASSERT_EQ(Count, 16u) << "round " << Round;
        for (size_t i = 0; i < Count; ++i)
        {
            EXPECT_EQ(Batch[i], Expected++);
        }
    }

    EXPECT_EQ(Queue.DequeueBulk(Batch, 16), 0u);
}

TEST(ConcurrentQueue, LosesNothingUnderContentionThatOverflowsTheRing)
{
    constexpr uint32 kProducers = 4;
    constexpr uint32 kConsumers = 4;
    constexpr uint32 kPerProducer = 25000;
    constexpr uint32 kTotal = kProducers * kPerProducer;

    // Deliberately far too small, so the spill path carries most of the traffic.
    TConcurrentQueue<uint32> Queue(64);

    TVector<uint8> Seen(kTotal, 0);
    TAtomic<uint32> Consumed{0};
    TVector<FThread> Threads;

    for (uint32 p = 0; p < kProducers; ++p)
    {
        Threads.push_back(FThread([&Queue, p]()
        {
            for (uint32 i = 0; i < kPerProducer; ++i)
            {
                Queue.Enqueue(p * kPerProducer + i);
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
    EXPECT_EQ(Queue.SizeApprox(), 0u);

    for (uint32 i = 0; i < kTotal; ++i)
    {
        ASSERT_EQ(Seen[i], 1u) << "value " << i;
    }
}

TEST(ConcurrentQueue, SurvivesADrainRaceWithoutLosingItems)
{
    constexpr uint32 kProducers = 3;
    constexpr uint32 kPerProducer = 40000;
    constexpr uint32 kTotal = kProducers * kPerProducer;

    TConcurrentQueue<uint32> Queue(256);

    TVector<uint8> Seen(kTotal, 0);
    TVector<FThread> Threads;
    TAtomic<uint32> Produced{0};

    for (uint32 p = 0; p < kProducers; ++p)
    {
        Threads.push_back(FThread([&Queue, &Produced, p]()
        {
            for (uint32 i = 0; i < kPerProducer; ++i)
            {
                Queue.Enqueue(p * kPerProducer + i);
                Produced.fetch_add(1, Atomic::MemoryOrderRelease);
            }
        }));
    }

    // A single drainer racing the producers, the shape a per-frame drain actually has.
    uint32 Consumed = 0;
    uint32 Value = 0;
    while (Consumed < kTotal)
    {
        if (Queue.TryDequeue(Value))
        {
            Seen[Value] += 1;
            ++Consumed;
        }
        else
        {
            Threading::ThreadYield();
        }
    }

    for (FThread& Thread : Threads)
    {
        Thread.Join();
    }

    EXPECT_EQ(Produced.load(Atomic::MemoryOrderAcquire), kTotal);
    for (uint32 i = 0; i < kTotal; ++i)
    {
        ASSERT_EQ(Seen[i], 1u) << "value " << i;
    }
}
