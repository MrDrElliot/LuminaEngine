#include "RuntimePCH.h"
#include "Input/InputQuery.h"

#include "Input/InputActionMap.h"
#include "Input/InputContext.h"
#include "Input/InputViewport.h"

namespace Lumina::Input
{
    const FInputContext* GetReceivingContext(const CWorld* World)
    {
        if (World == nullptr)
        {
            return nullptr;
        }

        const FInputViewportRegistry& Registry = FInputViewportRegistry::Get();
        const FInputViewport* Viewport = Registry.FindViewportForWorld(World);

        // Both conditions are global, so exactly one world is driven at a time and the editor's input
        // focus toggle reliably stops all of them.
        if (Viewport == nullptr || !Registry.IsGameInputFocused() || Viewport != Registry.GetActiveViewport())
        {
            return nullptr;
        }
        return &Viewport->GetContext();
    }

    const FInputActionState& GetActionState(const CWorld* World, const FInputActionHandle& Action)
    {
        static const FInputActionState Empty;
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr ? FInputActionMap::Get().GetActionState(Action, *Context) : Empty;
    }

    bool  IsActionDown    (const CWorld* World, const FInputActionHandle& Action) { return GetActionState(World, Action).IsDown(); }
    bool  IsActionPressed (const CWorld* World, const FInputActionHandle& Action) { return GetActionState(World, Action).IsPressed(); }
    bool  IsActionReleased(const CWorld* World, const FInputActionHandle& Action) { return GetActionState(World, Action).IsReleased(); }
    bool  IsActionHeld    (const CWorld* World, const FInputActionHandle& Action) { return GetActionState(World, Action).IsHeld(); }
    bool  WasActionTapped (const CWorld* World, const FInputActionHandle& Action) { return GetActionState(World, Action).IsTapped(); }

    float GetActionAxis   (const CWorld* World, const FInputActionHandle& Action) { return GetActionState(World, Action).X; }
    float GetActionHeldTime(const CWorld* World, const FInputActionHandle& Action) { return GetActionState(World, Action).HeldTime; }

    FVector2 GetActionAxis2D(const CWorld* World, const FInputActionHandle& Action)
    {
        const FInputActionState& State = GetActionState(World, Action);
        return FVector2(State.X, State.Y);
    }

    // Layers are pushed on the world's own context, NOT the receiving one: a world must be able to arm its
    // pause layer while the editor holds input focus, otherwise the layer would silently fail to apply.
    static FInputContext* FindOwnContext(const CWorld* World)
    {
        FInputViewport* Viewport = FInputViewportRegistry::Get().FindViewportForWorld(World);
        return Viewport != nullptr ? &Viewport->GetContext() : nullptr;
    }

    void PushLayer(const CWorld* World, FName Layer)
    {
        if (FInputContext* Context = FindOwnContext(World))
        {
            Context->PushInputLayer(Layer);
        }
    }

    bool PopLayer(const CWorld* World, FName Layer)
    {
        FInputContext* Context = FindOwnContext(World);
        return Context != nullptr && Context->PopInputLayer(Layer);
    }

    bool HasLayer(const CWorld* World, FName Layer)
    {
        const FInputContext* Context = FindOwnContext(World);
        return Context != nullptr && Context->HasInputLayer(Layer);
    }

    void ClearLayers(const CWorld* World)
    {
        if (FInputContext* Context = FindOwnContext(World))
        {
            Context->ClearInputLayers();
        }
    }

    bool IsKeyDown(const CWorld* World, EKey Key)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsKeyDown(Key);
    }

    bool IsKeyPressed(const CWorld* World, EKey Key)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsKeyPressed(Key);
    }

    bool IsKeyReleased(const CWorld* World, EKey Key)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsKeyReleased(Key);
    }

    bool IsMouseButtonDown(const CWorld* World, EMouseKey Button)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsMouseButtonDown(Button);
    }

    bool IsMouseButtonPressed(const CWorld* World, EMouseKey Button)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsMouseButtonPressed(Button);
    }

    bool IsMouseButtonReleased(const CWorld* World, EMouseKey Button)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr && Context->IsMouseButtonReleased(Button);
    }

    FVector2 GetMousePosition(const CWorld* World)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr ? FVector2((float)Context->GetMouseX(), (float)Context->GetMouseY()) : FVector2(0.0f);
    }

    FVector2 GetMouseDelta(const CWorld* World)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr ? FVector2((float)Context->GetMouseDeltaX(), (float)Context->GetMouseDeltaY()) : FVector2(0.0f);
    }

    float GetMouseWheel(const CWorld* World)
    {
        const FInputContext* Context = GetReceivingContext(World);
        return Context != nullptr ? (float)Context->GetMouseZ() : 0.0f;
    }

    float GetAxisPair(const CWorld* World, const FInputActionHandle& Positive, const FInputActionHandle& Negative)
    {
        // Resolved once and reused: going back through GetActionState would re-derive the context per side.
        const FInputContext* Context = GetReceivingContext(World);
        if (Context == nullptr)
        {
            return 0.0f;
        }

        const FInputActionMap& Map = FInputActionMap::Get();
        const float Up   = Map.GetActionState(Positive, *Context).IsDown() ? 1.0f : 0.0f;
        const float Down = Map.GetActionState(Negative, *Context).IsDown() ? 1.0f : 0.0f;
        return Up - Down;
    }
}
