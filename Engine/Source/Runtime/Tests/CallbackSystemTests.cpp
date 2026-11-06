#include <gtest/gtest.h>

#include "Containers/Algorithm.h"
#include "Containers/HashTable.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Events/Event.h"
#include "Input/InputContext.h"
#include "Input/InputViewport.h"
#include "Scripting/EntityScript.h"
#include "Scripting/ScriptableTest.h"
#include "TaskSystem/Coroutine.h"
#include "World/Entity/Components/InputComponent.h"
#include "World/Entity/Components/SequencePlayerComponent.h"
#include "World/Entity/Systems/EntitySystem.h"
#include "World/Subsystems/TweenManager.h"
#include "World/Subsystems/TimerSubsystem.h"
#include "World/World.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    constexpr double kFrame = 1.0 / 60.0;

    // The callback runners scheduled for real, so a test sees where the scheduler runs their deferred ticks.
    struct FCallbackWorld
    {
        CWorld*         World = nullptr;
        ECS::FRegistry* Registry = nullptr;

        FCallbackWorld()
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            Registry = &World->GetEntityRegistry();
            Registry->Ctx().Emplace<CWorld*>(World);
            Registry->Ctx().GetOrEmplace<FTimerManager>();
            Registry->Ctx().GetOrEmplace<FCoroutineManager>();
            Registry->Ctx().GetOrEmplace<FTweenManager>().SetWorldRegistry(Registry);

            // By name, since the system classes are not exported to the test binary.
            const FName Kept[] = { "CTimerSystem", "CCoroutineSystem", "CInputSystem", "CTweenSystem", "CSequencerSystem" };
            THashSet<FName> Disabled;
            EntitySystems::ForEachSystemClass([&](CClass* Class)
            {
                if (!Algo::Contains(Kept, Class->GetName()))
                {
                    Disabled.Add(Class->GetName());
                }
            });
            EntitySystems::CreateMissing(*World, Disabled, World->Systems);
            FWorldTestAccess::RebuildSystemSchedule(*World);
        }

        ~FCallbackWorld()
        {
            World->GetTimers()->GetCoroutineManager().Clear();
        }

        FTweenManager& Tweens() const { return Registry->Ctx().Get<FTweenManager>(); }

        void Tick(EUpdateStage Stage = EUpdateStage::FrameStart) const { FWorldTestAccess::TickStage(*World, Stage, kFrame); }
    };

    // A structural change made from inside a callback lands at once only where the whole world is in reach.
    bool CreatesAtOnce(ECS::FRegistry& Registry)
    {
        const ECS::FEntity Entity = Registry.Create();
        return SSystemContext::CanReachWorld() && !ECS::FCommandBus::ShouldDefer() && Registry.IsValid(Entity);
    }

    TCoroutine<> ResumeNextTick(CWorld*, ECS::FRegistry& Registry, TVector<FString>& Log, bool& bReachedWorld)
    {
        co_await Coro::NextTick();
        bReachedWorld = CreatesAtOnce(Registry);
        Log.Add("C");
    }

    // Registered and focused for the test's lifetime, so the world counts as receiving input.
    struct FFocusedViewport
    {
        FInputViewportRegistry& Registry = FInputViewportRegistry::Get();
        FInputViewport          Viewport;
        const bool              bWasGameFocused = Registry.IsGameInputFocused();

        explicit FFocusedViewport(CWorld* World)
        {
            Viewport.SetWindowRect(0, 0, 100, 100);
            Viewport.SetWorld(World);
            Registry.Register(&Viewport);
            Registry.SetActiveViewport(&Viewport);
            Registry.SetGameInputFocused(true);
        }

        ~FFocusedViewport()
        {
            Registry.Unregister(&Viewport);
            Registry.SetGameInputFocused(bWasGameFocused);
        }
    };
}

TEST(CallbackSystems, TweenCallbacksRunAtTheFlushWithTheWorldInReach)
{
    FCallbackWorld Fixture;

    bool bCalled = false;
    bool bReachedWorld = false;
    Fixture.Tweens().Create().Call([&]
    {
        bCalled = true;
        bReachedWorld = CreatesAtOnce(*Fixture.Registry);
    });

    Fixture.Tick();
    EXPECT_TRUE(bCalled) << "the deferred tick must run before the stage returns";
    EXPECT_TRUE(bReachedWorld) << "a tween callback may change structure, so it must run on the main thread outside any parallel system";
}

TEST(CallbackSystems, PausedTweensStillTickAtTheFlush)
{
    FCallbackWorld Fixture;

    float Value = 0.0f;
    Fixture.Tweens().Create().To(0.0f, 1.0f, 1.0f, [&Value](float V) { Value = V; }).SetIgnoreTimeScale(true);

    Fixture.Tick(EUpdateStage::Paused);
    EXPECT_GT(Value, 0.0f) << "a tween that ignores time scale keeps moving in a pause menu";
}

TEST(CallbackSystems, CoroutinesResumeAtTheFlushWithTheWorldInReach)
{
    FCallbackWorld Fixture;

    TVector<FString> Log;
    bool bReachedWorld = false;
    TCoroutine<> Coroutine = ResumeNextTick(Fixture.World, *Fixture.Registry, Log, bReachedWorld);

    Fixture.Tick();
    ASSERT_EQ(Log.Num(), 1) << "the deferred tick must resume the coroutine before the stage returns";
    EXPECT_TRUE(bReachedWorld);
}

TEST(CallbackSystems, InputReachesListenerScriptsAtTheFlush)
{
    FCallbackWorld Fixture;
    FFocusedViewport Focus(Fixture.World);

    TVector<FString> Log;
    const ECS::FEntity Listener = Fixture.Registry->Create();
    Fixture.Registry->Emplace<SInputComponent>(Listener);
    CEntityScriptTest* Script = static_cast<CEntityScriptTest*>(EntityScripts::Attach(*Fixture.Registry, Listener, CEntityScriptTest::StaticClass()));
    ASSERT_NE(Script, nullptr);
    Script->EventLog = &Log;
    Script->Label = "Listener";

    FKeyPressedEvent Event(EKey::W, false, false, false, false);
    Focus.Viewport.GetContext().OnEvent(Event);

    Log.Reset();
    Fixture.Tick();
    EXPECT_TRUE(Algo::Contains(Log, FString("I:Listener"))) << "the deferred dispatch must hand the frame's key event to the listener";
}

TEST(CallbackSystems, CallbackRunnersKeepTheirOrderWithinAFrame)
{
    FCallbackWorld Fixture;
    FFocusedViewport Focus(Fixture.World);

    TVector<FString> Log;
    bool bReachedWorld = false;
    TCoroutine<> Coroutine = ResumeNextTick(Fixture.World, *Fixture.Registry, Log, bReachedWorld);
    Fixture.Tweens().Create().Call([&Log] { Log.Add("T"); });

    const ECS::FEntity Listener = Fixture.Registry->Create();
    Fixture.Registry->Emplace<SInputComponent>(Listener);
    CEntityScriptTest* Script = static_cast<CEntityScriptTest*>(EntityScripts::Attach(*Fixture.Registry, Listener, CEntityScriptTest::StaticClass()));
    ASSERT_NE(Script, nullptr);
    Script->Label = "Listener";

    FKeyPressedEvent Event(EKey::W, false, false, false, false);
    Focus.Viewport.GetContext().OnEvent(Event);
    Script->EventLog = &Log;

    Fixture.Tick();
    const TVector<FString> Expected = { "C", "I:Listener", "T" };
    EXPECT_EQ(Log, Expected) << "coroutines, then input, then tweens, every frame, so work one hands the next lands the same frame each time";
}

TEST(CallbackSystems, SequencePlayersAdvanceAtTheFlush)
{
    FCallbackWorld Fixture;

    const ECS::FEntity Entity = Fixture.Registry->Create();
    Fixture.Registry->Emplace<SSequencePlayerComponent>(Entity).bPlaying = true;

    Fixture.Tick(EUpdateStage::PrePhysics);
    EXPECT_FALSE(Fixture.Registry->Get<SSequencePlayerComponent>(Entity).bPlaying) << "the deferred walk must stop a player with no sequence before the stage returns";
}
