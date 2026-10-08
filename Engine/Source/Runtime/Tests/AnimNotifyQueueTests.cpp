#include <gtest/gtest.h>

#include "Animation/AnimNotify.h"
#include "Animation/AnimNotifyQueue.h"
#include "TaskSystem/TaskSystem.h"
#include "World/ECS/CommandBus.h"
#include "World/ECS/Registry.h"

using namespace Lumina;

namespace
{
    struct FNotified
    {
        int32 Count = 0;
    };

    struct FCall
    {
        uint32 Entity = 0;
        int32  Id     = 0;
        float  Alpha  = 0.0f;
        EAnimNotifyEventType Type = EAnimNotifyEventType::Trigger;
    };

    struct STestNotify : SAnimNotify
    {
        int32           Id       = 0;
        bool            bDestroy = false;
        TVector<FCall>* Calls    = nullptr;

        void Notify(ECS::FRegistry& Registry, ECS::FEntity Entity) const override
        {
            Calls->push_back({ Entity.Value, Id });

            // A structural write the animation system never declares, which is the case the queue exists for.
            FNotified* Notified = Registry.TryGet<FNotified>(Entity);
            const int32 Previous = Notified != nullptr ? Notified->Count : 0;
            Registry.EmplaceOrReplace<FNotified>(Entity, FNotified{ Previous + 1 });

            if (bDestroy)
            {
                Registry.Destroy(Entity);
            }
        }
    };

    struct STestNotifyState : SAnimNotifyState
    {
        TVector<FCall>* Calls = nullptr;

        void NotifyBegin(ECS::FRegistry&, ECS::FEntity Entity) const override { Calls->push_back({ Entity.Value, 0, 0.0f, EAnimNotifyEventType::Begin }); }
        void NotifyTick(ECS::FRegistry&, ECS::FEntity Entity, float Alpha) const override { Calls->push_back({ Entity.Value, 0, Alpha, EAnimNotifyEventType::Tick }); }
        void NotifyEnd(ECS::FRegistry&, ECS::FEntity Entity) const override { Calls->push_back({ Entity.Value, 0, 1.0f, EAnimNotifyEventType::End }); }
    };

    FAnimNotifyEvent Trigger(const SAnimNotify* Notify)
    {
        FAnimNotifyEvent Event;
        Event.Type   = EAnimNotifyEventType::Trigger;
        Event.Notify = Notify;
        return Event;
    }
}

TEST(AnimNotifyQueue, HandlersRunAtTheFlushInEntityOrder)
{
    ECS::FRegistry Registry;
    ECS::FCommandBus Bus(Registry);
    FAnimNotifyQueue Queue;
    Queue.Prepare();

    TVector<FCall> Calls;
    STestNotify First;  First.Id = 1;  First.Calls = &Calls;
    STestNotify Second; Second.Id = 2; Second.Calls = &Calls;
    STestNotify Third;  Third.Id = 3;  Third.Calls = &Calls;

    const ECS::FEntity A = Registry.Create();
    const ECS::FEntity B = Registry.Create();
    ASSERT_LT(A.Value, B.Value);

    FAnimNotifyEvent NameOnly;
    NameOnly.Name = FName("Footstep");

    Queue.Record(B, EAnimNotifyPass::Simple, { Trigger(&First), NameOnly, Trigger(&Second) });
    Queue.Record(A, EAnimNotifyPass::Simple, { Trigger(&Third) });
    Queue.Submit(Bus);

    EXPECT_TRUE(Calls.empty());
    EXPECT_TRUE(Bus.HasPending());

    Bus.Flush();

    ASSERT_EQ(Calls.size(), 3u);
    EXPECT_EQ(Calls[0].Entity, A.Value);
    EXPECT_EQ(Calls[0].Id, 3);
    EXPECT_EQ(Calls[1].Entity, B.Value);
    EXPECT_EQ(Calls[1].Id, 1);
    EXPECT_EQ(Calls[2].Entity, B.Value);
    EXPECT_EQ(Calls[2].Id, 2);
    EXPECT_EQ(Registry.Get<FNotified>(A).Count, 1);
    EXPECT_EQ(Registry.Get<FNotified>(B).Count, 2);
    EXPECT_FALSE(Bus.HasPending());
}

TEST(AnimNotifyQueue, SimplePassRunsBeforeGraphPass)
{
    ECS::FRegistry Registry;
    ECS::FCommandBus Bus(Registry);
    FAnimNotifyQueue Queue;
    Queue.Prepare();

    TVector<FCall> Calls;
    STestNotify FromGraph;  FromGraph.Id = 1;  FromGraph.Calls = &Calls;
    STestNotify FromSimple; FromSimple.Id = 0; FromSimple.Calls = &Calls;

    const ECS::FEntity A = Registry.Create();
    const ECS::FEntity B = Registry.Create();

    Queue.Record(A, EAnimNotifyPass::Graph, { Trigger(&FromGraph) });
    Queue.Record(B, EAnimNotifyPass::Simple, { Trigger(&FromSimple) });
    Queue.Submit(Bus);
    Bus.Flush();

    ASSERT_EQ(Calls.size(), 2u);
    EXPECT_EQ(Calls[0].Id, 0);
    EXPECT_EQ(Calls[0].Entity, B.Value);
    EXPECT_EQ(Calls[1].Id, 1);
    EXPECT_EQ(Calls[1].Entity, A.Value);
}

TEST(AnimNotifyQueue, ManyWorkersRecordAndOneFlushRunsEveryHandlerOnce)
{
    ECS::FRegistry Registry;
    ECS::FCommandBus Bus(Registry);
    FAnimNotifyQueue Queue;
    Queue.Prepare();

    TVector<FCall> Calls;
    STestNotify First;  First.Id = 0;  First.Calls = &Calls;
    STestNotify Second; Second.Id = 1; Second.Calls = &Calls;

    constexpr uint32 Count = 4000;
    TVector<ECS::FEntity> Entities(Count);
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        Entities[Index] = Registry.Create();
    }

    Task::ParallelFor(Count, [&](uint32 Index)
    {
        Queue.Record(Entities[Index], EAnimNotifyPass::Simple, { Trigger(&First), Trigger(&Second) });
    }, 16);

    Queue.Submit(Bus);
    EXPECT_TRUE(Calls.empty());
    Bus.Flush();

    ASSERT_EQ(Calls.size(), static_cast<SIZE_T>(Count) * 2);
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        ASSERT_EQ(Calls[Index * 2].Entity, Calls[Index * 2 + 1].Entity);
        EXPECT_EQ(Calls[Index * 2].Id, 0);
        EXPECT_EQ(Calls[Index * 2 + 1].Id, 1);
        if (Index > 0)
        {
            EXPECT_LT(Calls[Index * 2 - 1].Entity, Calls[Index * 2].Entity);
        }
        EXPECT_EQ(Registry.Get<FNotified>(Entities[Index]).Count, 2);
    }

    // A second submit with nothing recorded must not enqueue anything.
    Queue.Submit(Bus);
    EXPECT_FALSE(Bus.HasPending());
}

TEST(AnimNotifyQueue, EntityDestroyedByAnEarlierHandlerGetsNoMoreCallbacks)
{
    ECS::FRegistry Registry;
    ECS::FCommandBus Bus(Registry);
    FAnimNotifyQueue Queue;
    Queue.Prepare();

    TVector<FCall> Calls;
    STestNotify Killer; Killer.Id = 1; Killer.bDestroy = true; Killer.Calls = &Calls;
    STestNotify After;  After.Id = 2;  After.Calls = &Calls;

    const ECS::FEntity A = Registry.Create();
    const ECS::FEntity B = Registry.Create();

    Queue.Record(A, EAnimNotifyPass::Simple, { Trigger(&Killer), Trigger(&After) });
    Queue.Record(B, EAnimNotifyPass::Simple, { Trigger(&After) });
    Queue.Submit(Bus);
    Bus.Flush();

    EXPECT_FALSE(Registry.IsValid(A));
    ASSERT_EQ(Calls.size(), 2u);
    EXPECT_EQ(Calls[0].Entity, A.Value);
    EXPECT_EQ(Calls[0].Id, 1);
    EXPECT_EQ(Calls[1].Entity, B.Value);
    EXPECT_EQ(Calls[1].Id, 2);
}

TEST(AnimNotifyQueue, NotifyStateCallbacksKeepTheirTypeAndAlpha)
{
    ECS::FRegistry Registry;
    ECS::FCommandBus Bus(Registry);
    FAnimNotifyQueue Queue;
    Queue.Prepare();

    TVector<FCall> Calls;
    STestNotifyState State;
    State.Calls = &Calls;

    const ECS::FEntity A = Registry.Create();

    TVector<FAnimNotifyEvent> Events(3);
    Events[0].Type = EAnimNotifyEventType::Begin;
    Events[1].Type = EAnimNotifyEventType::Tick;
    Events[1].Alpha = 0.25f;
    Events[2].Type = EAnimNotifyEventType::End;
    for (FAnimNotifyEvent& Event : Events)
    {
        Event.State = &State;
    }

    Queue.Record(A, EAnimNotifyPass::Simple, Events);
    Queue.Submit(Bus);
    Bus.Flush();

    ASSERT_EQ(Calls.size(), 3u);
    EXPECT_EQ(Calls[0].Type, EAnimNotifyEventType::Begin);
    EXPECT_EQ(Calls[1].Type, EAnimNotifyEventType::Tick);
    EXPECT_FLOAT_EQ(Calls[1].Alpha, 0.25f);
    EXPECT_EQ(Calls[2].Type, EAnimNotifyEventType::End);
}

TEST(AnimNotifyQueue, DiscardedBatchRunsNoHandlers)
{
    ECS::FRegistry Registry;
    ECS::FCommandBus Bus(Registry);
    FAnimNotifyQueue Queue;
    Queue.Prepare();

    TVector<FCall> Calls;
    STestNotify Notify;
    Notify.Calls = &Calls;

    Queue.Record(Registry.Create(), EAnimNotifyPass::Simple, { Trigger(&Notify) });
    Queue.Submit(Bus);
    Bus.Discard();

    EXPECT_TRUE(Calls.empty());
    EXPECT_FALSE(Bus.HasPending());
}
