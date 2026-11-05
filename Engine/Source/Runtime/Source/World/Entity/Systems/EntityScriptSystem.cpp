#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "EntityScriptSystem.h"

#include "Core/Math/Math.h"
#include "Scripting/EntityScript.h"
#include "World/World.h"
#include "World/Subsystems/WorldSettings.h"
#include "World/Entity/Systems/SystemContext.h"

namespace Lumina
{
    namespace EntityScriptSystemInternal
    {
        // Fixed 1/PhysicsHz steps for worlds without a physics scene, as a simulating one dispatches from its pre-step hook.
        void DispatchFixedUpdates(ECS::FRegistry& Registry, CWorld* World, float& Accumulator, float DeltaSeconds)
        {
            float FixedDt  = 1.0f / 60.0f;
            int32 MaxSteps = 8;

            if (World != nullptr)
            {
                const SDefaultWorldSettings& Settings = World->GetDefaultWorldSettings();
                FixedDt  = 1.0f / Math::Max(10.0f, Settings.PhysicsHz);
                MaxSteps = (int32)Settings.MaxPhysicsSteps;
            }

            if (MaxSteps <= 0)
            {
                return;
            }

            // Clamped as a spiral-of-death guard, so a long hitch cannot queue a hundred steps.
            Accumulator = Math::Min(Accumulator + DeltaSeconds, (float)MaxSteps * FixedDt);

            const int32 Steps = (Accumulator >= FixedDt)
                ? Math::Min(MaxSteps, (int32)(Accumulator / FixedDt))
                : 0;
            if (Steps <= 0)
            {
                return;
            }

            Accumulator -= (float)Steps * FixedDt;

            for (int32 Step = 0; Step < Steps; ++Step)
            {
                // Re-walked per step so a script that detaches itself is not dispatched in a later step.
                EntityScripts::TickFixed(Registry, FixedDt);
            }
        }
    }

    void SEntityScriptSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics);
        RequireUpdate(EUpdateStage::PostPhysics);
    }

    void SEntityScriptSystem::OnExclusiveUpdate(const SExclusiveContext& Context)
    {
        ECS::FRegistry& Registry = Context.GetRegistry();

        if (Context.GetUpdateStage() == EUpdateStage::PrePhysics)
        {
            EntityScripts::Tick(Registry, Context.GetDeltaTime(), EScriptUpdatePhase::PrePhysics);
        }
        else
        {
            CWorld* World = Context.GetWorld();
            if (World == nullptr || World->GetPhysicsScene() == nullptr)
            {
                // The frame delta feeds the accumulator, so each frame is counted exactly once.
                EntityScriptSystemInternal::DispatchFixedUpdates(Registry, World, FixedAccumulator, Context.GetDeltaTime());
            }

            // After the step, so an [Reflect(UpdatePhase = PostPhysics)] script reads settled transforms.
            EntityScripts::Tick(Registry, Context.GetDeltaTime(), EScriptUpdatePhase::PostPhysics);
        }
    }
}
