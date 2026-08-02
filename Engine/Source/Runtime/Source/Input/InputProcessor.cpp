#include "RuntimePCH.h"
#include "InputProcessor.h"

#include "Input/InputContext.h"
#include "Input/InputViewport.h"
#if WITH_EDITOR
#include "imgui.h"
#endif

namespace Lumina
{
    FInputProcessor& FInputProcessor::Get()
    {
        static FInputProcessor Instance;
        return Instance;
    }

    FInputContext* FInputProcessor::GetActiveContext() const
    {
        FInputViewport* Active = FInputViewportRegistry::Get().GetActiveViewport();
        return Active ? &Active->GetContext() : nullptr;
    }

    double FInputProcessor::GetMouseX() const      { auto* C = GetActiveContext(); return C ? C->GetMouseX() : 0.0; }
    double FInputProcessor::GetMouseY() const      { auto* C = GetActiveContext(); return C ? C->GetMouseY() : 0.0; }
    double FInputProcessor::GetMouseZ() const      { auto* C = GetActiveContext(); return C ? C->GetMouseZ() : 0.0; }
    double FInputProcessor::GetMouseDeltaX() const { auto* C = GetActiveContext(); return C ? C->GetMouseDeltaX() : 0.0; }
    double FInputProcessor::GetMouseDeltaY() const { auto* C = GetActiveContext(); return C ? C->GetMouseDeltaY() : 0.0; }

    Input::EKeyState FInputProcessor::GetKeyState(EKey K) const
    {
        auto* C = GetActiveContext();
        return C ? C->GetKeyState(K) : Input::EKeyState::Up;
    }

    Input::EMouseState FInputProcessor::GetMouseButtonState(EMouseKey M) const
    {
        auto* C = GetActiveContext();
        return C ? C->GetMouseButtonState(M) : Input::EMouseState::Up;
    }

    bool FInputProcessor::IsKeyDown(EKey K) const     { auto* C = GetActiveContext(); return C ? C->IsKeyDown(K) : false; }
    bool FInputProcessor::IsKeyUp(EKey K) const       { auto* C = GetActiveContext(); return C ? C->IsKeyUp(K) : true; }
    bool FInputProcessor::IsKeyPressed(EKey K) const  { auto* C = GetActiveContext(); return C ? C->IsKeyPressed(K) : false; }
    bool FInputProcessor::IsKeyReleased(EKey K) const { auto* C = GetActiveContext(); return C ? C->IsKeyReleased(K) : false; }
    bool FInputProcessor::IsKeyRepeated(EKey K) const { auto* C = GetActiveContext(); return C ? C->IsKeyRepeated(K) : false; }

    bool FInputProcessor::IsMouseButtonDown(EMouseKey M) const     { auto* C = GetActiveContext(); return C ? C->IsMouseButtonDown(M) : false; }
    bool FInputProcessor::IsMouseButtonUp(EMouseKey M) const       { auto* C = GetActiveContext(); return C ? C->IsMouseButtonUp(M) : true; }
    bool FInputProcessor::IsMouseButtonPressed(EMouseKey M) const  { auto* C = GetActiveContext(); return C ? C->IsMouseButtonPressed(M) : false; }
    bool FInputProcessor::IsMouseButtonReleased(EMouseKey M) const { auto* C = GetActiveContext(); return C ? C->IsMouseButtonReleased(M) : false; }
    float FInputProcessor::GetMouseButtonHeldTime(EMouseKey M) const { auto* C = GetActiveContext(); return C ? C->GetMouseButtonHeldTime(M) : -1.0f; }

    void FInputProcessor::SetMouseMode(EMouseMode Mode, CWorld* CallerWorld)
    {
        FInputViewportRegistry& Reg = FInputViewportRegistry::Get();

        // Target the CALLER's own viewport when known (per-window capture); else the active one. This is
        // what makes multiple game-preview windows behave: each player's script sets only its own window's
        // mode, so an inactive player's release can't wipe the focused window's capture.
        FInputViewport* Target = (CallerWorld != nullptr) ? Reg.FindViewportForWorld(CallerWorld) : nullptr;
        if (Target == nullptr)
        {
            Target = Reg.GetActiveViewport();
        }
        if (Target == nullptr)
        {
            return;
        }
        Target->GetContext().SetMouseMode(Mode);
        Reg.ReapplyActiveCursorMode();

        // Block ImGui's invisible-cursor warp from fighting our capture.
        #if WITH_EDITOR
        FInputViewport* Active = Reg.GetActiveViewport();
        const EMouseMode ActiveMode = Active ? Active->GetContext().GetMouseMode() : EMouseMode::Normal;
        ImGuiIO& IO = ImGui::GetIO();
        if (ActiveMode == EMouseMode::Captured || ActiveMode == EMouseMode::Hidden)
        {
            IO.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        }
        else
        {
            IO.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        }
        #endif
    }

    void FInputProcessor::SetInputMode(EInputMode Mode)
    {
        if (FInputContext* C = GetActiveContext())
        {
            C->SetInputMode(Mode);
        }
    }

    EInputMode FInputProcessor::GetInputMode() const
    {
        FInputContext* C = GetActiveContext();
        return C ? C->GetInputMode() : EInputMode::Game;
    }
}
