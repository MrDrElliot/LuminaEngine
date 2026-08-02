#include "RuntimePCH.h"
#include "InputSystem.h"
#include "World/Entity/Components/InputComponent.h"
#include "Input/InputViewport.h"

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

        Context.GetRegistry().view<SInputComponent>().each([&](SInputComponent& Input)
        {
            const FInputViewport* V = Reg.FindViewportForWorld(Input.World);
            if (V == nullptr)
            {
                Input.ResetSnapshot();
                return;
            }
            Input.SnapshotFrom(V->GetContext(), bGameFocused && V == Active);
        });
    }
}
