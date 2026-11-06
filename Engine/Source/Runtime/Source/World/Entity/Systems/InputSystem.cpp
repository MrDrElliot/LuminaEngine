#include "RuntimePCH.h"
#include "InputSystem.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/InputComponent.h"
#include "Input/InputContext.h"
#include "Input/InputQuery.h"
#include "Scripting/EntityScript.h"
#include "World/World.h"
#include "CoroutineSystem.h"

namespace Lumina
{
    void CInputSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);
        RunsAfter<CCoroutineSystem>();
    }

    void CInputSystem::OnUpdate(const SSystemContext& Context)
    {
        Context.Defer([this](const SExclusiveContext& X) { Dispatch(X); });
    }

    void CInputSystem::Dispatch(const SExclusiveContext& Context)
    {
        const FInputContext* Ctx = Input::GetReceivingContext(Context.GetWorld());
        if (Ctx == nullptr)
        {
            return;
        }

        ECS::FRegistry& Registry = Context.GetRegistry();
        const TVector<SInputActionState>& States = Ctx->GetActionStates();
        const TVector<SInputEvent>& Events = Ctx->GetFrameEvents();

        ChangedActionIndices.Reset();
        for (int32 i = 0; i < States.Num(); ++i)
        {
            const SInputActionState& State = States[i];
            if (State.IsPressed() || State.IsReleased() || State.X != 0.0f || State.Y != 0.0f)
            {
                ChangedActionIndices.Add(i);
            }
        }

        // Snapshot first, since a callback spawning an entity mutates the storage a live view walks.
        for (ECS::FEntity Entity : Listeners.Collect())
        {
            if (!Registry.IsValid(Entity))
            {
                continue;
            }

            // Re-resolved per entity, since an earlier callback may have removed or disabled it.
            const SInputComponent* InputComp = Listeners.TryGet<SInputComponent>(Entity);
            if (InputComp == nullptr || !InputComp->bEnabled)
            {
                continue;
            }

            for (const SInputEvent& Event : Events)
            {
                EntityScripts::DispatchInput(Registry, Entity, Event);
            }

            EntityScripts::DispatchActions(Registry, Entity, States.GetData(), States.Num(), ChangedActionIndices);
        }
    }
}
