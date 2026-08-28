#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "EntityScriptSystem.h"

#include "Core/Math/Math.h"
#include "Core/Profiler/Profile.h"
#include "Scripting/EntityScript.h"
#include "World/World.h"
#include "World/Subsystems/WorldSettings.h"
#include "World/Entity/Systems/SystemContext.h"
#include "World/Entity/Systems/SystemSingletons.h"

namespace Lumina
{
    namespace
    {
        // OnFixedUpdate is a fixed 1/PhysicsHz step, dispatched 0..MaxPhysicsSteps times per frame.
        void DispatchFixedUpdates(ECS::FRegistry& Registry, float DeltaSeconds)
        {
            float FixedDt  = 1.0f / 60.0f;
            int32 MaxSteps = 8;

            // find, not get, because a bare registry has no world and the defaults above stand.
            if (CWorld** WorldPtr = Registry.Ctx().Find<CWorld*>(); WorldPtr != nullptr && *WorldPtr != nullptr)
            {
                const SDefaultWorldSettings& Settings = (*WorldPtr)->GetDefaultWorldSettings();
                FixedDt  = 1.0f / Math::Max(10.0f, Settings.PhysicsHz);
                MaxSteps = (int32)Settings.MaxPhysicsSteps;
            }

            if (MaxSteps <= 0)
            {
                return;
            }

            auto& Ctx = Registry.Ctx();
            FScriptFixedUpdateState* StatePtr = Ctx.Find<FScriptFixedUpdateState>();
            FScriptFixedUpdateState& FixedState = StatePtr ? *StatePtr : Ctx.Emplace<FScriptFixedUpdateState>();

            // Clamped as a spiral-of-death guard, so a long hitch cannot queue a hundred steps.
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
                // Re-walked per step so a script that detaches itself is not dispatched in a later step.
                EntityScripts::TickFixed(Registry, FixedDt);
            }
        }
    }

    void SEntityScriptSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = Context.GetRegistry();

        if (Context.GetUpdateStage() == EUpdateStage::PrePhysics)
        {
            EntityScripts::Tick(Registry, (float)Context.GetDeltaTime(), EScriptUpdatePhase::PrePhysics);
        }
        else
        {
            // The frame delta feeds the accumulator, so each frame is counted exactly once.
            DispatchFixedUpdates(Registry, (float)Context.GetDeltaTime());

            // After the step, so an [UpdatePhase(PostPhysics)] script reads settled transforms.
            EntityScripts::Tick(Registry, (float)Context.GetDeltaTime(), EScriptUpdatePhase::PostPhysics);
        }
    }
}
