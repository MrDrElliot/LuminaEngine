#include "RuntimePCH.h"
#include "InputSystem.h"
#include "World/Entity/Components/InputComponent.h"
#include "Input/InputContext.h"
#include "Input/InputQuery.h"
#include "Scripting/EntityScript.h"
#include "World/World.h"

namespace Lumina
{
    // Exclusive like STimerSystem, since dispatch runs user script code and calls into managed.
    FSystemAccess SInputSystem::Access = FSystemAccess::Exclusive();

    void SInputSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        FEntityRegistry& Registry = Context.GetRegistry();
        
        CWorld** WorldPtr = Registry.ctx().find<CWorld*>();
        CWorld* World = WorldPtr != nullptr ? *WorldPtr : nullptr;
        
        const FInputContext* Ctx = Input::GetReceivingContext(World);
        if (Ctx == nullptr)
        {
            return;
        }

        const TVector<FInputActionState>& States = Ctx->GetActionStates();
        const TVector<SInputEvent>& Events = Ctx->GetFrameEvents();
        const uint32 Serial = Ctx->GetActionsSerial();
        const float DeltaSeconds = (float)Context.GetDeltaTime();

        // Snapshot first, since a callback spawning an entity mutates the storage a live view walks.
        TVector<entt::entity> Entities;
        auto View = Registry.view<SInputComponent>();
        Entities.reserve(View.size_hint());
        for (entt::entity Entity : View)
        {
            Entities.push_back(Entity);
        }

        for (entt::entity Entity : Entities)
        {
            if (!Registry.valid(Entity))
            {
                continue;
            }

            // Re-resolved per entity, since an earlier callback may have removed or disabled it.
            const SInputComponent* InputComp = Registry.try_get<SInputComponent>(Entity);
            if (InputComp == nullptr || !InputComp->bEnabled)
            {
                continue;
            }

            for (const SInputEvent& Event : Events)
            {
                EntityScripts::DispatchInput(Registry, Entity, Event);
            }

            EntityScripts::PollInputBindings(Registry, Entity, States.data(), (int32)States.size(), Serial, DeltaSeconds);
        }
    }
}
