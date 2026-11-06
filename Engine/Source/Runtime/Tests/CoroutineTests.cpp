#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Scripting/EntityScript.h"
#include "TaskSystem/Coroutine.h"
#include "TaskSystem/Generator.h"
#include "TaskSystem/ThreadedCallback.h"
#include "World/World.h"
#include "World/Entity/Components/TransformComponent.h"

using namespace Lumina;

namespace
{
    constexpr double kStep = 0.1;

    CWorld* NewCoroutineWorld()
    {
        STransformComponent::StaticStruct();
        SEntityScriptComponent::StaticStruct();
        CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
        ECS::FRegistry& Registry = World->GetEntityRegistry();
        Registry.Ctx().Emplace<CWorld*>(World);
        return World;
    }

    void Advance(CWorld* World, int32 Steps, bool bPaused = false, double Step = kStep)
    {
        for (int32 Index = 0; Index < Steps; ++Index)
        {
            World->GetTimers()->GetCoroutineManager().Tick(Step, Step, bPaused);
        }
    }

    struct FDestructionProbe
    {
        int32* Count;
        ~FDestructionProbe() { ++*Count; }
    };

    TCoroutine<> CountTicks(CWorld*, int32& Out, int32 Ticks)
    {
        for (int32 Index = 0; Index < Ticks; ++Index)
        {
            co_await Coro::NextTick();
            ++Out;
        }
    }

    TCoroutine<> WaitSeconds(CWorld*, bool& bDone, double Seconds, ECoroutineClock Clock)
    {
        co_await Coro::Seconds(Seconds, Clock);
        bDone = true;
    }

    TCoroutine<int32> Twice(CWorld*, int32 Value)
    {
        co_await Coro::NextTick();
        co_return Value * 2;
    }

    TCoroutine<int32> SumOfTwice(CWorld* World)
    {
        const int32 A = co_await Twice(World, 3);
        const int32 B = co_await Twice(World, 4);
        co_return A + B;
    }

    TCoroutine<> WaitForever(CWorld*, int32& Destroyed, int32& CanceledHooks, int32& Reached)
    {
        FDestructionProbe Probe{ &Destroyed };
        auto Hook = co_await Coro::OnCanceled([&CanceledHooks]() { ++CanceledHooks; });
        co_await Coro::Until([]() { return false; });
        ++Reached;
    }
}

TEST(Coroutine, RunsEagerlyUntilItsFirstSuspension)
{
    CWorld* World = NewCoroutineWorld();
    int32 Count = 0;
    TCoroutine<> Coroutine = CountTicks(World, Count, 3);
    EXPECT_FALSE(Coroutine.IsDone());
    EXPECT_EQ(Count, 0);

    Advance(World, 1);
    EXPECT_EQ(Count, 1);
    Advance(World, 2);
    EXPECT_EQ(Count, 3);
    EXPECT_TRUE(Coroutine.IsDone());
    EXPECT_TRUE(Coroutine.WasSuccessful());
}

TEST(Coroutine, ClocksFollowPauseAndDilation)
{
    CWorld* World = NewCoroutineWorld();
    bool bWorld = false;
    bool bUnpaused = false;
    bool bReal = false;
    WaitSeconds(World, bWorld, 0.5, ECoroutineClock::World);
    WaitSeconds(World, bUnpaused, 0.5, ECoroutineClock::Unpaused);
    WaitSeconds(World, bReal, 0.5, ECoroutineClock::Real);

    Advance(World, 6, /*bPaused*/ true);
    EXPECT_FALSE(bWorld) << "world time stops while paused";
    EXPECT_TRUE(bUnpaused);
    EXPECT_TRUE(bReal);

    Advance(World, 6);
    EXPECT_TRUE(bWorld);

    // A dilated frame advances world time by the scaled delta and real time by the clock.
    bool bDilatedWorld = false;
    bool bDilatedReal = false;
    WaitSeconds(World, bDilatedWorld, 1.0, ECoroutineClock::World);
    WaitSeconds(World, bDilatedReal, 1.0, ECoroutineClock::Real);
    for (int32 Index = 0; Index < 6; ++Index)
    {
        World->GetTimers()->GetCoroutineManager().Tick(0.05, 0.2, false);
    }
    EXPECT_FALSE(bDilatedWorld);
    EXPECT_TRUE(bDilatedReal);
    World->GetTimers()->GetCoroutineManager().Clear();
}

TEST(Coroutine, UntilPollsOncePerTick)
{
    CWorld* World = NewCoroutineWorld();
    int32 Polls = 0;
    bool bDone = false;
    auto Run = [](CWorld*, int32& Polls, bool& bDone) -> TCoroutine<>
    {
        co_await Coro::Until([&Polls]() { return ++Polls >= 3; });
        bDone = true;
    };
    Run(World, Polls, bDone);
    EXPECT_EQ(Polls, 1) << "checked once before suspending";
    Advance(World, 1);
    EXPECT_FALSE(bDone);
    Advance(World, 1);
    EXPECT_TRUE(bDone);
    EXPECT_EQ(Polls, 3);
}

TEST(Coroutine, AwaitingAChildReturnsItsValue)
{
    CWorld* World = NewCoroutineWorld();
    TCoroutine<int32> Sum = SumOfTwice(World);
    Advance(World, 3);
    ASSERT_TRUE(Sum.IsDone());
    EXPECT_EQ(Sum.GetResult(), 14);
}

TEST(Coroutine, CancelRunsLocalDestructorsAndTheCanceledHook)
{
    CWorld* World = NewCoroutineWorld();
    int32 Destroyed = 0;
    int32 Hooks = 0;
    int32 Reached = 0;
    TCoroutine<> Coroutine = WaitForever(World, Destroyed, Hooks, Reached);
    Advance(World, 2);
    EXPECT_EQ(Destroyed, 0);

    Coroutine.Cancel();
    EXPECT_FALSE(Coroutine.IsDone()) << "served at the next tick, not inside Cancel";
    Advance(World, 1);
    EXPECT_TRUE(Coroutine.IsDone());
    EXPECT_FALSE(Coroutine.WasSuccessful());
    EXPECT_EQ(Destroyed, 1);
    EXPECT_EQ(Hooks, 1);
    EXPECT_EQ(Reached, 0);
}

TEST(Coroutine, ACompletedCoroutineIgnoresALateCancel)
{
    CWorld* World = NewCoroutineWorld();
    int32 Count = 0;
    TCoroutine<> Coroutine = CountTicks(World, Count, 1);
    Advance(World, 1);
    Coroutine.Cancel();
    EXPECT_TRUE(Coroutine.WasSuccessful());
}

TEST(Coroutine, AGuardDefersCancelToTheNextUnguardedAwait)
{
    CWorld* World = NewCoroutineWorld();
    int32 Steps = 0;
    auto Run = [](CWorld*, int32& Steps) -> TCoroutine<>
    {
        {
            auto Guard = co_await Coro::CancellationGuard();
            co_await Coro::NextTick();
            ++Steps;
            co_await Coro::NextTick();
            ++Steps;
        }
        co_await Coro::NextTick();
        ++Steps;
    };
    TCoroutine<> Coroutine = Run(World, Steps);
    Coroutine.Cancel();
    Advance(World, 4);
    EXPECT_EQ(Steps, 2) << "both guarded steps finish and the unguarded one never runs";
    EXPECT_TRUE(Coroutine.IsDone());
    EXPECT_FALSE(Coroutine.WasSuccessful());
}

TEST(Coroutine, CancelingAParentCancelsTheChildItAwaits)
{
    CWorld* World = NewCoroutineWorld();
    int32 ChildDestroyed = 0;
    int32 Hooks = 0;
    int32 Reached = 0;
    auto Parent = [](CWorld* World, int32& Destroyed, int32& Hooks, int32& Reached) -> TCoroutine<>
    {
        co_await WaitForever(World, Destroyed, Hooks, Reached);
        ++Reached;
    };
    TCoroutine<> Coroutine = Parent(World, ChildDestroyed, Hooks, Reached);
    Advance(World, 1);
    Coroutine.Cancel();
    Advance(World, 1);
    EXPECT_TRUE(Coroutine.IsDone());
    EXPECT_FALSE(Coroutine.WasSuccessful());
    EXPECT_EQ(ChildDestroyed, 1);
    EXPECT_EQ(Reached, 0);
}

TEST(Coroutine, OwnerDestructionEndsItsCoroutines)
{
    CWorld* World = NewCoroutineWorld();
    ECS::FRegistry& Registry = World->GetEntityRegistry();
    CEntityScript* Script = NewObject<CEntityScript>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    Script->SetOwner(Registry.Create(), World);

    int32 Ticks = 0;
    auto Run = [](CEntityScript* Owner, int32& Ticks) -> TCoroutine<>
    {
        for (;;)
        {
            co_await Coro::NextTick();
            ++Ticks;
        }
    };
    TCoroutine<> Coroutine = Run(Script, Ticks);
    Advance(World, 2);
    EXPECT_EQ(Ticks, 2);

    Script->ConditionalBeginDestroy();
    Advance(World, 2);
    EXPECT_EQ(Ticks, 2);
    EXPECT_TRUE(Coroutine.IsDone());
    EXPECT_FALSE(Coroutine.WasSuccessful());
}

TEST(Coroutine, WhenAllWhenAnyAndRace)
{
    CWorld* World = NewCoroutineWorld();
    int32 Fast = 0;
    int32 Slow = 0;
    int32 AllDone = 0;
    auto All = [](CWorld* World, int32& Fast, int32& Slow, int32& Done) -> TCoroutine<>
    {
        co_await Coro::WhenAll(CountTicks(World, Fast, 1), CountTicks(World, Slow, 3));
        Done = Fast + Slow;
    };
    All(World, Fast, Slow, AllDone);
    Advance(World, 3);
    EXPECT_EQ(AllDone, 4);

    int32 AnyWinner = -1;
    int32 RaceWinner = -1;
    int32 AnyLoser = 0;
    int32 RaceLoser = 0;
    int32 Unused = 0;
    auto Any = [](CWorld* World, int32& Winner, int32& Loser, int32& Unused) -> TCoroutine<>
    {
        Winner = co_await Coro::WhenAny(CountTicks(World, Loser, 5), CountTicks(World, Unused, 1));
    };
    auto RaceTo = [](CWorld* World, int32& Winner, int32& Loser, int32& Unused) -> TCoroutine<>
    {
        Winner = co_await Coro::Race(CountTicks(World, Loser, 5), CountTicks(World, Unused, 1));
    };
    int32 UnusedRace = 0;
    Any(World, AnyWinner, AnyLoser, Unused);
    RaceTo(World, RaceWinner, RaceLoser, UnusedRace);
    Advance(World, 6);
    EXPECT_EQ(AnyWinner, 1);
    EXPECT_EQ(RaceWinner, 1);
    EXPECT_EQ(AnyLoser, 5) << "WhenAny leaves the others running";
    EXPECT_LT(RaceLoser, 5) << "Race cancels the others";
}

TEST(Coroutine, ChainResumesFromACallbackAndIgnoresOneAfterCancel)
{
    CWorld* World = NewCoroutineWorld();
    TMoveOnlyFunction<void()> Pending;
    bool bResumed = false;
    auto Run = [](CWorld*, TMoveOnlyFunction<void()>& Pending, bool& bResumed) -> TCoroutine<>
    {
        co_await Coro::Chain([&Pending](auto Resume) { Pending = std::move(Resume); });
        bResumed = true;
    };
    Run(World, Pending, bResumed);
    ASSERT_TRUE(static_cast<bool>(Pending));
    Pending();
    EXPECT_TRUE(bResumed);

    TMoveOnlyFunction<void()> Late;
    bool bLateResumed = false;
    TCoroutine<> Canceled = Run(World, Late, bLateResumed);
    Canceled.Cancel();
    Advance(World, 1);
    EXPECT_TRUE(Canceled.IsDone());
    Late();
    EXPECT_FALSE(bLateResumed);
}

TEST(Coroutine, WorldTeardownEndsEveryCoroutineAndScriptWait)
{
    CWorld* World = NewCoroutineWorld();
    int32 Destroyed = 0;
    int32 Hooks = 0;
    int32 Reached = 0;
    auto Guarded = [](CWorld* World, int32& Destroyed, int32& Hooks, int32& Reached) -> TCoroutine<>
    {
        auto Guard = co_await Coro::CancellationGuard();
        co_await WaitForever(World, Destroyed, Hooks, Reached);
        ++Reached;
    };
    TCoroutine<> Coroutine = Guarded(World, Destroyed, Hooks, Reached);

    int32 ScriptResult = -1;
    CoroDetail::FLatentWaitSpec Spec;
    Spec.Kind  = CoroDetail::FLatentWaitSpec::EKind::Ticks;
    Spec.Ticks = 100;
    World->GetTimers()->GetCoroutineManager().AddWait(std::move(Spec), [&ScriptResult](bool bCompleted) { ScriptResult = bCompleted ? 1 : 0; });

    World->GetTimers()->GetCoroutineManager().Clear();
    EXPECT_TRUE(Coroutine.IsDone());
    EXPECT_FALSE(Coroutine.WasSuccessful()) << "teardown goes past guards";
    EXPECT_EQ(Destroyed, 1);
    EXPECT_EQ(Reached, 0);
    EXPECT_EQ(ScriptResult, 0);
    EXPECT_EQ(World->GetTimers()->GetCoroutineManager().NumTracked(), 0);
}

TEST(Coroutine, ScriptWaitsFireAndCancel)
{
    CWorld* World = NewCoroutineWorld();
    int32 Fired = 0;
    CoroDetail::FLatentWaitSpec Spec;
    Spec.Kind    = CoroDetail::FLatentWaitSpec::EKind::Time;
    Spec.Seconds = 0.25;
    World->GetTimers()->GetCoroutineManager().AddWait(std::move(Spec), [&Fired](bool bCompleted) { Fired += bCompleted ? 1 : 100; });

    CoroDetail::FLatentWaitSpec Dropped;
    Dropped.Kind  = CoroDetail::FLatentWaitSpec::EKind::Ticks;
    Dropped.Ticks = 1;
    const uint64 Id = World->GetTimers()->GetCoroutineManager().AddWait(std::move(Dropped), [&Fired](bool) { Fired += 1000; });
    World->GetTimers()->GetCoroutineManager().CancelWait(Id);

    Advance(World, 2);
    EXPECT_EQ(Fired, 0);
    Advance(World, 1);
    EXPECT_EQ(Fired, 1);
}

TEST(Coroutine, FinishNowIfCanceledEndsATightLoop)
{
    CWorld* World = NewCoroutineWorld();
    int32 Iterations = 0;
    auto Run = [](CWorld*, int32& Iterations) -> TCoroutine<>
    {
        co_await Coro::NextTick();
        for (int32 Index = 0; Index < 100; ++Index)
        {
            co_await Coro::FinishNowIfCanceled();
            ++Iterations;
        }
    };
    TCoroutine<> Coroutine = Run(World, Iterations);
    Coroutine.Cancel();
    Advance(World, 1);
    EXPECT_EQ(Iterations, 0);
    EXPECT_FALSE(Coroutine.WasSuccessful());

    auto SelfCancel = [](CWorld*, int32& Iterations) -> TCoroutine<>
    {
        ++Iterations;
        co_await Coro::SelfCancel();
        ++Iterations;
    };
    TCoroutine<> Self = SelfCancel(World, Iterations);
    EXPECT_EQ(Iterations, 1);
    EXPECT_TRUE(Self.IsDone());
    EXPECT_FALSE(Self.WasSuccessful());
}

TEST(Coroutine, MovesToAWorkerAndBackToTheGameThread)
{
    bool bOnWorker = false;
    bool bBackHome = false;
    auto Run = [](bool& bOnWorker, bool& bBackHome) -> TCoroutine<>
    {
        co_await Coro::MoveToTask();
        bOnWorker = !CoroDetail::IsOnGameThread();
        co_await Coro::MoveToGameThread();
        bBackHome = CoroDetail::IsOnGameThread();
    };
    ASSERT_TRUE(CoroDetail::IsOnGameThread());
    TCoroutine<> Coroutine = Run(bOnWorker, bBackHome);
    for (int32 Spin = 0; Spin < 20000 && !Coroutine.IsDone(); ++Spin)
    {
        MainThread::ProcessQueue();
        Threading::ThreadYield();
    }
    ASSERT_TRUE(Coroutine.IsDone());
    EXPECT_TRUE(bOnWorker);
    EXPECT_TRUE(bBackHome);
}

TEST(Coroutine, ContinueWithRunsOnceAndAtOnceWhenDone)
{
    CWorld* World = NewCoroutineWorld();
    int32 Count = 0;
    int32 Calls = 0;
    TCoroutine<> Coroutine = CountTicks(World, Count, 1);
    Coroutine.ContinueWith([&Calls]() { ++Calls; });
    EXPECT_EQ(Calls, 0);
    Advance(World, 1);
    EXPECT_EQ(Calls, 1);
    Coroutine.ContinueWith([&Calls]() { ++Calls; });
    EXPECT_EQ(Calls, 2);
}

namespace
{
    TGenerator<int32> Fibonacci(int32 Count)
    {
        int32 A = 0;
        int32 B = 1;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            co_yield A;
            const int32 Next = A + B;
            A = B;
            B = Next;
        }
    }
}

TEST(Coroutine, GeneratorsYieldLazily)
{
    TVector<int32> Values;
    for (int32 Value : Fibonacci(7))
    {
        Values.Add(Value);
    }
    const TVector<int32> Expected = { 0, 1, 1, 2, 3, 5, 8 };
    EXPECT_EQ(Values, Expected);

    TGenerator<int32> Partial = Fibonacci(1000);
    int32 Seen = 0;
    for (int32 Value : Partial)
    {
        if (++Seen == 3)
        {
            EXPECT_EQ(Value, 1);
            break;
        }
    }
}
