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

        // The world comes from the registry rather than a back-pointer on each component, which is what the
        // component's World field existed to carry.
        CWorld** WorldPtr = Registry.ctx().find<CWorld*>();
        CWorld* World = WorldPtr != nullptr ? *WorldPtr : nullptr;

        // One gate for the whole world, shared with the Input:: query surface. Resolved once per frame
        // instead of once per entity.
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

            // Discrete events reach this entity's scripts in either language: the same virtual serves a C++
            // and a C# script.
            for (const SInputEvent& Event : Ctx->GetFrameEvents())
            {
                EntityScripts::DispatchInput(Registry, Entity, Event);
            }

            // Declarative bindings run off the same gate, so a script cannot see a Pressed for a frame its
            // entity was not receiving input.
            EntityScripts::PollInputBindings(Registry, Entity, States.data(), (int32)States.size(),
                Serial, DeltaSeconds);
        });
    }
}
