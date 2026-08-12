#include "RuntimePCH.h"
#include "EntityScriptSystem.h"

#include "Core/Math/Math.h"
#include "Core/Profiler/Profile.h"
#include "Scripting/EntityScript.h"
#include "World/World.h"
#include "World/Subsystems/WorldSettings.h"
#include "World/Entity/Registry/EntityRegistry.h"
#include "World/Entity/Systems/SystemContext.h"
#include "World/Entity/Systems/SystemSingletons.h"

namespace Lumina
{
    namespace
    {
        // OnFixedUpdate is contractually a FIXED step: the delta a script receives is 1/PhysicsHz, and it is
        // dispatched however many times this frame's elapsed time accounts for -- 0..MaxPhysicsSteps. Handing
        // it the frame delta once per frame instead would make every script written against it (integrating a
        // velocity, stepping a controller) behave differently at 30 fps than at 60, which is the one thing a
        // fixed step exists to prevent.
        //
        // A game-thread accumulator of its own, mirroring JoltPhysicsScene::Update's: same Hz, same cap, so it
        // runs the same number of steps per frame as the physics scene without a cross-thread query.
        void DispatchFixedUpdates(FEntityRegistry& Registry, float DeltaSeconds)
        {
            float FixedDt  = 1.0f / 60.0f;
            int32 MaxSteps = 8;

            // find, not get: a bare registry (a test, a tool) has no world, and the defaults above stand.
            if (CWorld** WorldPtr = Registry.ctx().find<CWorld*>(); WorldPtr != nullptr && *WorldPtr != nullptr)
            {
                const SDefaultWorldSettings& Settings = (*WorldPtr)->GetDefaultWorldSettings();
                FixedDt  = 1.0f / Math::Max(10.0f, Settings.PhysicsHz);
                MaxSteps = (int32)Settings.MaxPhysicsSteps;
            }

            if (MaxSteps <= 0)
            {
                return;
            }

            auto& Ctx = Registry.ctx();
            FScriptFixedUpdateState* StatePtr = Ctx.find<FScriptFixedUpdateState>();
            FScriptFixedUpdateState& FixedState = StatePtr ? *StatePtr : Ctx.emplace<FScriptFixedUpdateState>();

            // Accumulate + clamp (spiral-of-death guard): a long hitch must not queue up a hundred steps that
            // then cost more than the hitch did.
            FixedState.Accumulator = Math::Min(FixedState.Accumulator + DeltaSeconds, (float)MaxSteps * FixedDt);

            const int32 Steps = (FixedState.Accumulator >= FixedDt)
                ? Math::Min(MaxSteps, (int32)(FixedState.Accumulator / FixedDt))
                : 0;
            if (Steps <= 0)
            {
                return;
            }

            FixedState.Accumulator -= (float)Steps * FixedDt;

            for (int32 Step = 0; Step < Steps; ++Step)
            {
                // Re-walked per step rather than dispatched off one gathered list: a script that detaches
                // itself (or spawns another) in OnFixedUpdate must not be dispatched again in a later step.
                EntityScripts::TickFixed(Registry, FixedDt);
            }
        }
    }

    void SEntityScriptSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        FEntityRegistry& Registry = Context.GetRegistry();

        if (Context.GetUpdateStage() == EUpdateStage::PrePhysics)
        {
            EntityScripts::Tick(Registry, (float)Context.GetDeltaTime());
        }
        else
        {
            // The frame delta, not a fixed one -- it is what feeds the accumulator. Both stages are dispatched
            // once per frame with the same delta, so driving the accumulator from here counts each frame once.
            DispatchFixedUpdates(Registry, (float)Context.GetDeltaTime());
        }
    }
}
