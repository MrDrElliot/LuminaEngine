#include "RuntimePCH.h"
#include "InputSystem.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/InputComponent.h"
#include "Input/InputContext.h"
#include "Input/InputQuery.h"
#include "Scripting/EntityScript.h"
#include "World/World.h"

namespace Lumina
{
    // Exclusive like STimerSystem, since dispatch runs user script code and calls into managed.
    void SInputSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart, EUpdatePriority::Highest);
    }

    void SInputSystem::OnUpdate()
    {
        const FSystemContext& Context = GetContext();

        LUMINA_PROFILE_SCOPE();

        ECS::FRegistry& Registry = Context.GetRegistry();
        
        CWorld** WorldPtr = Registry.Ctx().Find<CWorld*>();
        CWorld* World = WorldPtr != nullptr ? *WorldPtr : nullptr;
        
        const FInputContext* Ctx = Input::GetReceivingContext(World);
        if (Ctx == nullptr)
        {
            return;
        }

        const TVector<FInputActionState>& States = Ctx->GetActionStates();
        const TVector<SInputEvent>& Events = Ctx->GetFrameEvents();

        static thread_local TVector<int32> ChangedActionIndices;
        ChangedActionIndices.clear();
        for (int32 i = 0; i < (int32)States.size(); ++i)
        {
            const FInputActionState& State = States[i];
            if (State.IsPressed() || State.IsReleased() || State.X != 0.0f || State.Y != 0.0f)
            {
                ChangedActionIndices.push_back(i);
            }
        }

        // Snapshot first, since a callback spawning an entity mutates the storage a live view walks.
        static thread_local TVector<ECS::FEntity> Entities;
        Entities.clear();

        auto View = Registry.View<SInputComponent>();
        Entities.reserve(View.Num());
        for (ECS::FEntity Entity : View)
        {
            Entities.push_back(Entity);
        }

        for (ECS::FEntity Entity : Entities)
        {
            if (!Registry.IsValid(Entity))
            {
                continue;
            }

            // Re-resolved per entity, since an earlier callback may have removed or disabled it.
            const SInputComponent* InputComp = Registry.TryGet<SInputComponent>(Entity);
            if (InputComp == nullptr || !InputComp->bEnabled)
            {
                continue;
            }

            for (const SInputEvent& Event : Events)
            {
                EntityScripts::DispatchInput(Registry, Entity, Event);
            }

            EntityScripts::DispatchActions(Registry, Entity, States.data(), (int32)States.size(), ChangedActionIndices);
        }
    }
}
