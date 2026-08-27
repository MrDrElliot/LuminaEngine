#include <gtest/gtest.h>

#include <cstdio>

#include "Containers/BoundedQueue.h"
#include "Containers/ConcurrentQueue.h"
#include "Containers/Vector.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "Platform/Time/PlatformTime.h"

// Namespaced because the unity build merges this file with others that do "using namespace Lumina".
namespace LuminaBoundedQueueBench
{
    using Lumina::uint32;
    using Lumina::uint64;
    using Lumina::uint8;
    using Lumina::FThread;
    using Lumina::TVector;
    using Lumina::TAtomic;
    using Lumina::TBoundedMPMCQueue;
    using Lumina::TBoundedMPSCQueue;
    using Lumina::TBoundedSPSCQueue;
    using Lumina::TConcurrentQueue;

    namespace Atomic = Lumina::Atomic;
    namespace Threading = Lumina::Threading;

    volatile uint64 GQueueBenchSink = 0;

    constexpr int    kRepeats  = 5;
    constexpr uint32 kCapacity = 1024;
    constexpr uint32 kItems    = 400000;

    template <typename TBody>
    double BestMillisOf(int Repeats, TBody&& Body)
    {
        double Best = 1e30;
        for (int Attempt = 0; Attempt < Repeats; ++Attempt)
        {
            const uint64 Start = Lumina::PlatformTime::Cycles();
            Body();
            const uint64 Stop = Lumina::PlatformTime::Cycles();

            const double Millis = Lumina::PlatformTime::ToMilliseconds(Stop - Start);
            Best = Millis < Best ? Millis : Best;
        }
        return Best;
    }

    void ReportHeader(const char* Title)
    {
        std::printf("\n%s\n", Title);
        std::printf("  %-32s %12s %14s\n", "variant", "best (ms)", "M items/sec");
    }

    void ReportRow(const char* Name, double Millis, uint64 Items)
    {
        const double PerSecond = Millis > 0.0 ? (double)Items / (Millis * 1000.0) : 0.0;
        std::printf("  %-32s %12.3f %14.2f\n", Name, Millis, PerSecond);
    }

    /** One producer and one consumer, the shape a stream or a single worker feed actually has. */
    template <typename TQueue>
    double RunSingleProducerSingleConsumer(TQueue& Queue)
    {
        return BestMillisOf(kRepeats, [&Queue]
        {
            TAtomic<uint32> Consumed{0};

            FThread Producer([&Queue]
            {
                for (uint32 i = 0; i < kItems; ++i)
                {
                    while (!Queue.TryEnqueue(i))
                    {
                        Threading::ThreadYield();
                    }
                }
            });

            uint32 Value = 0;
            uint32 Seen = 0;
            uint64 Sum = 0;
            while (Seen < kItems)
            {
                if (Queue.TryDequeue(Value))
                {
                    Sum += Value;
                    ++Seen;
                }
            }

            Producer.Join();
            Consumed.store(Seen, Atomic::MemoryOrderRelaxed);
            GQueueBenchSink += Sum;
        });
    }

    template <typename TQueue>
    double RunMultiProducerMultiConsumer(TQueue& Queue, uint32 Producers, uint32 Consumers)
    {
        return BestMillisOf(kRepeats, [&Queue, Producers, Consumers]
        {
            const uint32 PerProducer = kItems / Producers;
            const uint32 Total = PerProducer * Producers;

            TAtomic<uint32> Consumed{0};
            TVector<FThread> Threads;

            for (uint32 p = 0; p < Producers; ++p)
            {
                Threads.push_back(FThread([&Queue, p, PerProducer]
                {
                    for (uint32 i = 0; i < PerProducer; ++i)
                    {
                        while (!Queue.TryEnqueue(p * PerProducer + i))
                        {
                            Threading::ThreadYield();
                        }
                    }
                }));
            }

            for (uint32 c = 0; c < Consumers; ++c)
            {
                Threads.push_back(FThread([&Queue, &Consumed, Total]
                {
                    uint32 Value = 0;
                    uint64 Sum = 0;
                    while (Consumed.load(Atomic::MemoryOrderAcquire) < Total)
                    {
                        if (Queue.TryDequeue(Value))
                        {
                            Sum += Value;
                            Consumed.fetch_add(1, Atomic::MemoryOrderRelease);
                        }
                    }
                    GQueueBenchSink += Sum;
                }));
            }

            for (FThread& Thread : Threads)
            {
                Thread.Join();
            }
        });
    }

    /** No contention at all, so this isolates the per-operation instruction cost. */
    template <typename TQueue>
    double RunUncontendedRoundTrip(TQueue& Queue)
    {
        return BestMillisOf(kRepeats, [&Queue]
        {
            uint64 Sum = 0;
            uint32 Value = 0;
            for (uint32 i = 0; i < kItems; ++i)
            {
                Queue.TryEnqueue(i);
                if (Queue.TryDequeue(Value))
                {
                    Sum += Value;
                }
            }
            GQueueBenchSink += Sum;
        });
    }

    TEST(BoundedQueueBench, UncontendedRoundTrip)
    {
        TBoundedMPMCQueue<uint32> Runtime;
        Runtime.Initialize(kCapacity);

        TBoundedMPMCQueue<uint32, kCapacity> Fixed;
        TBoundedSPSCQueue<uint32, kCapacity> Spsc;

        const double RuntimeMs = RunUncontendedRoundTrip(Runtime);
        const double FixedMs   = RunUncontendedRoundTrip(Fixed);
        const double SpscMs    = RunUncontendedRoundTrip(Spsc);

        ReportHeader("uncontended enqueue + dequeue, 400,000 round trips");
        ReportRow("MPMC (runtime capacity)", RuntimeMs, kItems);
        ReportRow("MPMC (compile-time capacity)", FixedMs, kItems);
        ReportRow("SPSC (compile-time capacity)", SpscMs, kItems);
        SUCCEED();
    }

    TEST(BoundedQueueBench, SingleProducerSingleConsumer)
    {
        TBoundedMPMCQueue<uint32> Runtime;
        Runtime.Initialize(kCapacity);

        TBoundedMPMCQueue<uint32, kCapacity> Fixed;
        TBoundedSPSCQueue<uint32, kCapacity> Spsc;

        const double RuntimeMs = RunSingleProducerSingleConsumer(Runtime);
        const double FixedMs   = RunSingleProducerSingleConsumer(Fixed);
        const double SpscMs    = RunSingleProducerSingleConsumer(Spsc);

        ReportHeader("1 producer, 1 consumer, 400,000 items");
        ReportRow("MPMC (runtime capacity)", RuntimeMs, kItems);
        ReportRow("MPMC (compile-time capacity)", FixedMs, kItems);
        ReportRow("SPSC (compile-time capacity)", SpscMs, kItems);
        SUCCEED();
    }

    TEST(BoundedQueueBench, FourProducersFourConsumers)
    {
        TBoundedMPMCQueue<uint32> Runtime;
        Runtime.Initialize(kCapacity);

        TBoundedMPMCQueue<uint32, kCapacity> Fixed;

        const double RuntimeMs = RunMultiProducerMultiConsumer(Runtime, 4, 4);
        const double FixedMs   = RunMultiProducerMultiConsumer(Fixed, 4, 4);

        ReportHeader("4 producers, 4 consumers, 400,000 items");
        ReportRow("MPMC (runtime capacity)", RuntimeMs, kItems);
        ReportRow("MPMC (compile-time capacity)", FixedMs, kItems);
        SUCCEED();
    }

    TEST(BoundedQueueBench, FourProducersOneConsumer)
    {
        TBoundedMPMCQueue<uint32, kCapacity> Mpmc;
        TBoundedMPSCQueue<uint32, kCapacity> Mpsc;

        const double MpmcMs = RunMultiProducerMultiConsumer(Mpmc, 4, 1);
        const double MpscMs = RunMultiProducerMultiConsumer(Mpsc, 4, 1);

        ReportHeader("4 producers, 1 consumer, 400,000 items");
        ReportRow("MPMC (compile-time capacity)", MpmcMs, kItems);
        ReportRow("MPSC (compile-time capacity)", MpscMs, kItems);
        SUCCEED();
    }

    TEST(BoundedQueueBench, Footprint)
    {
        std::printf("\nfootprint\n");
        std::printf("  %-32s %10zu\n", "MPMC runtime (ring on heap)", sizeof(TBoundedMPMCQueue<uint32>));
        std::printf("  %-32s %10zu\n", "MPMC fixed 1024 (ring inline)", sizeof(TBoundedMPMCQueue<uint32, kCapacity>));
        std::printf("  %-32s %10zu\n", "SPSC fixed 1024 (ring inline)", sizeof(TBoundedSPSCQueue<uint32, kCapacity>));
        SUCCEED();
    }

    TEST(BoundedQueueBench, UnboundedAgainstBounded)
    {
        TBoundedMPMCQueue<uint32, kCapacity> Bounded;

        TConcurrentQueue<uint32> Unbounded(kCapacity);
        TConcurrentQueue<uint32> Spilling(64);

        const double BoundedMs   = RunMultiProducerMultiConsumer(Bounded, 4, 4);
        const double UnboundedMs = RunMultiProducerMultiConsumer(Unbounded, 4, 4);
        const double SpillingMs  = RunMultiProducerMultiConsumer(Spilling, 4, 4);

        ReportHeader("4 producers, 4 consumers, 400,000 items");
        ReportRow("bounded ring only", BoundedMs, kItems);
        ReportRow("unbounded, ring fits", UnboundedMs, kItems);
        ReportRow("unbounded, spilling hard", SpillingMs, kItems);
        SUCCEED();
    }
}
