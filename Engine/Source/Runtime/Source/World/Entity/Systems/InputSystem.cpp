#include "RuntimePCH.h"
#include "InputSystem.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/InputComponent.h"
#include "Input/InputContext.h"
#include "Input/InputQuery.h"
#include "Input/InputActionMap.h"
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

        const TVector<SInputAction>& Actions = FInputActionMap::Get().GetAllActions();

        // Snapshot first, since a listener spawning an entity mutates the storage a live view walks.
        for (ECS::FEntity Entity : Listeners.Collect())
        {
            // Resolved again after any broadcast that reports its delegate moved, since a listener may relocate or remove the component.
            auto Receiving = [&Registry, Entity]() -> SInputComponent*
            {
                SInputComponent* Input = Registry.IsValid(Entity) ? Registry.TryGet<SInputComponent>(Entity) : nullptr;
                return Input != nullptr && Input->bEnabled ? Input : nullptr;
            };

            SInputComponent* Input = Receiving();
            for (int32 Index = 0; Input != nullptr && Index < Events.Num(); ++Index)
            {
                if (!Input->OnInput.Broadcast(Events[Index]))
                {
                    Input = Receiving();
                }
            }

            for (int32 Index : ChangedActionIndices)
            {
                if (Input == nullptr)
                {
                    break;
                }
                if (Index < Actions.Num() && !Input->OnAction.Broadcast(Actions[Index].Name, States[Index]))
                {
                    Input = Receiving();
                }
            }
        }
    }
}
