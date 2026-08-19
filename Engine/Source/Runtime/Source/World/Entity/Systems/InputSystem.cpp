#include "RuntimePCH.h"
#include "InputSystem.h"
#include "World/Entity/Components/InputComponent.h"
#include "Input/InputContext.h"
#include "Input/InputQuery.h"
#include "Scripting/EntityScript.h"
#include "World/World.h"

namespace Lumina
{
    FSystemAccess SInputSystem::Access = FSystemAccess{}
        .Read<SInputComponent>();

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
        const uint32 Serial = Ctx->GetActionsSerial();
        const float DeltaSeconds = (float)Context.GetDeltaTime();

        Registry.view<SInputComponent>().each([&](entt::entity Entity, const SInputComponent& InputComp)
        {
            if (!InputComp.bEnabled)
            {
                return;
            }
            
            for (const SInputEvent& Event : Ctx->GetFrameEvents())
            {
                EntityScripts::DispatchInput(Registry, Entity, Event);
            }
            
            EntityScripts::PollInputBindings(Registry, Entity, States.data(), (int32)States.size(), Serial, DeltaSeconds);
        });
    }
}
