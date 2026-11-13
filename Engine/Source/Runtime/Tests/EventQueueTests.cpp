#include <gtest/gtest.h>

#include "Containers/String.h"
#include "TaskSystem/TaskSystem.h"
#include "World/ECS/EventMeta.h"
#include "World/ECS/EventQueue.h"
#include "World/ECS/Registry.h"

using namespace Lumina;

namespace LuminaEventQueueTests
{
    struct FPing
    {
        int32 Value = 0;
    };

    struct FNamedPing
    {
        FString Name;
    };

    struct FEcho
    {
        int32 Value = 0;
    };

    struct FQueueFixture
    {
        ECS::FRegistry     Registry;
        ECS::FEventQueues  Queues{ Registry };

        void EndFrame()
        {
            Queues.Commit();
            Queues.Advance();
        }
    };

    int32 Sum(ECS::TEventRange<FPing> Range)
    {
        int32 Total = 0;
        for (const FPing& Ping : Range)
        {
            Total += Ping.Value;
        }
        return Total;
    }

    // Counted by the fan-out below, and the echo it sends proves a fan-out may send while a commit runs.
    struct FFanOutLog
    {
        int32 Delivered = 0;
        ECS::FEventQueues* Queues = nullptr;
    };
}

using namespace LuminaEventQueueTests;

namespace Lumina
{
    template<>
    struct TEventFanOut<LuminaEventQueueTests::FPing>
    {
        static void Deliver(ECS::FRegistry& Registry, const LuminaEventQueueTests::FPing& Ping)
        {
            FFanOutLog& Log = *Registry.Ctx().Get<FFanOutLog*>();
            ++Log.Delivered;
            ECS::SendEvent(*Log.Queues, LuminaEventQueueTests::FEcho{ Ping.Value * 10 });
        }
    };
}

TEST(EventQueue, ASendIsReadableOnlyAfterTheCommit)
{
    FQueueFixture F;
    ECS::TEventReader<FPing> Reader;

    ECS::SendEvent(F.Queues, FPing{ 4 });
    EXPECT_TRUE(ECS::ReadEvents(F.Queues, Reader).IsEmpty()) << "staged events wait for the sync point";

    F.Queues.Commit();
    EXPECT_EQ(Sum(ECS::ReadEvents(F.Queues, Reader)), 4);
    EXPECT_TRUE(ECS::ReadEvents(F.Queues, Reader).IsEmpty()) << "a reader never sees the same event twice";
}

TEST(EventQueue, EveryReaderSeesEveryEventOnce)
{
    FQueueFixture F;
    ECS::TEventReader<FPing> Audio;
    ECS::TEventReader<FPing> Effects;

    ECS::SendEvent(F.Queues, FPing{ 1 });
    ECS::SendEvent(F.Queues, FPing{ 2 });
    F.Queues.Commit();

    EXPECT_EQ(Sum(ECS::ReadEvents(F.Queues, Audio)), 3);
    EXPECT_EQ(Sum(ECS::ReadEvents(F.Queues, Effects)), 3);
    EXPECT_EQ(Sum(ECS::ReadEvents(F.Queues, Audio)), 0);
}

TEST(EventQueue, AnEventLivesThroughTheFrameAfterItThenAgesOut)
{
    FQueueFixture F;
    ECS::TEventReader<FPing> Late;
    ECS::TEventReader<FPing> Absent;

    ECS::SendEvent(F.Queues, FPing{ 7 });
    F.EndFrame();
    EXPECT_EQ(Sum(ECS::ReadEvents(F.Queues, Late)), 7) << "a reader that runs before its writer catches up next frame";

    F.EndFrame();
    EXPECT_EQ(Sum(ECS::ReadEvents(F.Queues, Absent)), 0) << "two frames on, the event is gone";
}

TEST(EventQueue, UnreadCountsWhatAReaderHasNotSeen)
{
    FQueueFixture F;
    ECS::TEventReader<FPing> Reader;
    EXPECT_FALSE(ECS::HasUnreadEvents(F.Queues, Reader));

    ECS::SendEvent(F.Queues, FPing{ 1 });
    F.Queues.Commit();
    EXPECT_TRUE(ECS::HasUnreadEvents(F.Queues, Reader));

    (void)ECS::ReadEvents(F.Queues, Reader);
    EXPECT_FALSE(ECS::HasUnreadEvents(F.Queues, Reader));
}

TEST(EventQueue, UnreadGoesFalseWhenAReloadResizesTheEventMidFrame)
{
    FQueueFixture F;

    const ECS::FComponentTypeID TypeID = ECS::AcquireScriptEventType("LuminaEventQueueTests_ReloadedEvent", sizeof(int32), alignof(int32));
    ECS::FEventQueue& Queue = F.Queues.Get(TypeID);

    const int32 Payload = 9;
    Queue.Send(&Payload);
    F.Queues.Commit();

    const uint64 Cursor = 0;
    EXPECT_NE(Queue.NumUnread(Cursor), 0u);

    // A reload mid-frame changes the type's size, so the queue's stride no longer matches until the next Advance.
    ECS::AcquireScriptEventType("LuminaEventQueueTests_ReloadedEvent", sizeof(int64), alignof(int64));
    EXPECT_EQ(Queue.NumUnread(Cursor), 0u) << "NumUnread must agree with Read, which refuses the mismatched buffer";
}

TEST(EventQueue, EventsThatOwnMemoryAreCopiedAndReleased)
{
    FQueueFixture F;
    ECS::TEventReader<FNamedPing> Reader;

    // Long enough to live on the heap, so a byte copy instead of a real one would double free.
    const FString Name = "a name long enough to leave the small string buffer behind";
    for (int32 Index = 0; Index < 100; ++Index)
    {
        ECS::SendEvent(F.Queues, FNamedPing{ Name });
    }
    F.Queues.Commit();

    int32 Matching = 0;
    for (const FNamedPing& Ping : ECS::ReadEvents(F.Queues, Reader))
    {
        Matching += Ping.Name == Name ? 1 : 0;
    }
    EXPECT_EQ(Matching, 100);

    F.EndFrame();
    F.EndFrame();
}

TEST(EventQueue, SendsFromEveryWorkerArrive)
{
    FQueueFixture F;
    ECS::TEventReader<FPing> Reader;

    constexpr uint32 Count = 20000;
    Jobs::ParallelFor(Count, [&](uint32)
    {
        ECS::SendEvent(F.Queues, FPing{ 1 });
    });
    F.Queues.Commit();

    EXPECT_EQ(Sum(ECS::ReadEvents(F.Queues, Reader)), (int32)Count);
}

TEST(EventQueue, FanOutRunsAtTheCommitAndItsSendsSettleInTheSameSyncPoint)
{
    FQueueFixture F;
    FFanOutLog Log;
    Log.Queues = &F.Queues;
    F.Registry.Ctx().Emplace<FFanOutLog*>(&Log);

    const ECS::FComponentTypeID PingType = ECS::GetComponentTypeID<FPing>();
    ECS::FComponentTypeRegistry::Get().SetEventFanOut(PingType, &Meta::FanOutEvents<FPing>);

    ECS::SendEvent(F.Queues, FPing{ 2 });
    ECS::SendEvent(F.Queues, FPing{ 3 });
    EXPECT_EQ(Log.Delivered, 0);

    EXPECT_TRUE(F.Queues.Commit());
    EXPECT_EQ(Log.Delivered, 2);

    ECS::TEventReader<FEcho> Echoes;
    int32 EchoSum = 0;
    for (const FEcho& Echo : ECS::ReadEvents(F.Queues, Echoes))
    {
        EchoSum += Echo.Value;
    }
    EXPECT_EQ(EchoSum, 50) << "what a fan-out sends is committed before the sync point ends";

    ECS::FComponentTypeRegistry::Get().SetEventFanOut(PingType, nullptr);
}

TEST(EventQueue, ReadingMarksInterestSoAProducerKeepsFilling)
{
    FQueueFixture F;
    const ECS::FComponentTypeID PingType = ECS::GetComponentTypeID<FPing>();
    EXPECT_FALSE(F.Queues.HasInterest(PingType));

    ECS::TEventReader<FPing> Reader;
    (void)ECS::ReadEvents(F.Queues, Reader);
    EXPECT_TRUE(F.Queues.HasInterest(PingType));
}
