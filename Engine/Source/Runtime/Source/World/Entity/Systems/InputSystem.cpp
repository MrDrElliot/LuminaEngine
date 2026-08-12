#include "RuntimePCH.h"
#include "InputSystem.h"
#include "World/Entity/Components/InputComponent.h"
#include "Input/InputViewport.h"
#include "Input/InputContext.h"
#include "Scripting/EntityScript.h"

namespace Lumina
{
    FSystemAccess SInputSystem::Access = FSystemAccess{}
        .Write<SInputComponent>();

    void SInputSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        FInputViewportRegistry& Reg = FInputViewportRegistry::Get();
        const FInputViewport* Active = Reg.GetActiveViewport();

        // A world receives game input only when its viewport is the active one AND the editor has handed
        // input to the game (always true in a packaged build). Both conditions are global, so exactly one
        // PIE world is driven at a time and Shift+F1 reliably stops all of them.
        const bool bGameFocused = Reg.IsGameInputFocused();

        FEntityRegistry& Registry = Context.GetRegistry();

        Registry.view<SInputComponent>().each([&](entt::entity Entity, SInputComponent& Input)
        {
            const FInputViewport* V = Reg.FindViewportForWorld(Input.World);
            if (V == nullptr)
            {
                Input.ResetSnapshot();
                return;
            }

            const bool bReceiving = bGameFocused && V == Active;
            Input.SnapshotFrom(V->GetContext(), bReceiving);

            // Discrete events go to this entity's scripts, in either language -- the same virtual serves a
            // C++ and a C# script. Gated on the same focus condition as the snapshot, so a script cannot see
            // events the polling API says the entity never received.
            if (bReceiving)
            {
                for (const SInputEvent& Event : V->GetContext().GetFrameEvents())
                {
                    EntityScripts::DispatchInput(Registry, Entity, Event);
                }
            }
        });
    }
}
