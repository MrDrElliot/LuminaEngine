#include <gtest/gtest.h>

#include "Core/Object/ObjectCore.h"
#include "Scripting/ScriptableTest.h"
#include "World/World.h"
#include "World/Subsystems/WorldSettings.h"
#include "WorldTestAccess.h"

using namespace Lumina;

namespace
{
    constexpr float kFixedDt = 1.0f / 60.0f;

    struct FFixedUpdateWorld
    {
        CWorld*                       World  = nullptr;
        CEntitySystemFixedUpdateTest* System = nullptr;

        FFixedUpdateWorld()
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            System = NewObject<CEntitySystemFixedUpdateTest>(nullptr, NAME_None, FGuid::New(), OF_Transient);
            System->SetOwningWorld(World);
            System->ConfigureSystem();
            World->Systems.Add(System);
            FWorldTestAccess::RebuildSystemSchedule(*World);
        }

        int32 Run(std::initializer_list<double> FrameDeltas)
        {
            for (double Delta : FrameDeltas)
            {
                FWorldTestAccess::TickFixedUpdate(*World, Delta);
            }
            return System->TickCount;
        }
    };
}

TEST(FixedUpdateStage, AnySystemCanJoinIt)
{
    FFixedUpdateWorld Fixture;
    EXPECT_TRUE(Fixture.System->RunsInStage(EUpdateStage::FixedUpdate));
    EXPECT_STREQ(GUpdateStageNames[(uint8)EUpdateStage::FixedUpdate], "FixedUpdate");
}

TEST(FixedUpdateStage, TicksAtTheFixedRateWhateverTheFrameDelta)
{
    // Both cover 0.51 seconds, in frames short enough that the hitch clamp never drops time.
    FFixedUpdateWorld Even;
    FFixedUpdateWorld Ragged;

    const int32 EvenTicks   = Even.Run({ 0.102, 0.102, 0.102, 0.102, 0.102 });
    const int32 RaggedTicks = Ragged.Run({ 0.003, 0.047, 0.11, 0.004, 0.09, 0.006, 0.1, 0.05, 0.1 });

    EXPECT_EQ(EvenTicks, 30);
    EXPECT_EQ(RaggedTicks, EvenTicks) << "the tick count must depend on elapsed time, not on how frames slice it";

    EXPECT_EQ(Ragged.System->LastStage, EUpdateStage::FixedUpdate);
    EXPECT_FLOAT_EQ(Ragged.System->LastDeltaTime, kFixedDt) << "each step hands the system the fixed delta";
}

TEST(FixedUpdateStage, AShortFrameCarriesItsTimeForward)
{
    FFixedUpdateWorld Fixture;

    EXPECT_EQ(Fixture.Run({ 0.01 }), 0) << "less than one step of time must not tick";
    EXPECT_EQ(Fixture.Run({ 0.01 }), 1) << "the leftover from the last frame must count toward this one";
}

TEST(FixedUpdateStage, AHitchIsClampedToMaxSteps)
{
    FFixedUpdateWorld Fixture;
    const int32 MaxSteps = (int32)Fixture.World->GetDefaultWorldSettings().MaxPhysicsSteps;

    EXPECT_EQ(Fixture.Run({ 5.0 }), MaxSteps);
    EXPECT_EQ(Fixture.Run({ 0.0 }), MaxSteps) << "the clamped hitch must not leave a backlog for later frames";
}

// A world without a physics scene dispatches OnFixedUpdate through this shared stage rather than its own accumulator.
TEST(FixedUpdateStage, EntityScriptsWithoutAPhysicsSceneTickThroughIt)
{
    RegisterPendingTypes();
    CWorld* World = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    FWorldTestAccess::KeepOnlySystems(*World, { FName("CEntityScriptSystem") });

    ASSERT_EQ(World->GetPhysicsScene(), nullptr);

    const ECS::FEntity Entity = World->CreateEntity();
    CEntityScriptTest* Script = static_cast<CEntityScriptTest*>(
        EntityScripts::Attach(World->GetEntityRegistry(), Entity, CEntityScriptTest::StaticClass()));
    ASSERT_NE(Script, nullptr);

    // OnFixedUpdate only runs on a readied script, so the first PrePhysics pass is what makes it eligible.
    FWorldTestAccess::TickStage(*World, EUpdateStage::PrePhysics, 0.1);

    FWorldTestAccess::TickFixedUpdate(*World, 0.102);
    FWorldTestAccess::TickFixedUpdate(*World, 0.102);

    EXPECT_EQ(Script->FixedUpdateCount, 12) << "0.204s at 60Hz is 12 steps, exactly as many times as TickFixed ran";
}
