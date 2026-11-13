#pragma once

#include "Containers/Function.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Delegates/Delegate.h"
#include "Core/Math/Math.h"
#include "Events/KeyCodes.h"
#include "Events/MouseCodes.h"

namespace Lumina
{
    class FWindow;

    // Raw device input as the window saw it, so windowing never has to know an application exists.

    struct FKeyInput
    {
        EKey Key      = EKey::Space;
        bool bPressed = false;
        bool bRepeat  = false;
        bool bCtrl    = false;
        bool bShift   = false;
        bool bAlt     = false;
        bool bSuper   = false;
    };

    struct FMouseButtonInput
    {
        EMouseKey Button   = EMouseKey::Button0;
        bool      bPressed = false;
        float     X        = 0.0f;
        float     Y        = 0.0f;
    };

    struct FMouseMoveInput
    {
        float X      = 0.0f;
        float Y      = 0.0f;
        float DeltaX = 0.0f;
        float DeltaY = 0.0f;
    };

    struct FMouseScrollInput
    {
        float Delta = 0.0f;
    };

    // Window events pass through this, so whatever a mid-frame pump reads waits for the next frame's pump.
    class FWindowEventGate
    {
    public:

        // Runs Event now, or holds it while a hold is open and adds MouseDelta to the motion held so far.
        RUNTIME_API void Dispatch(TFunction<void()>&& Event, FVector2 MouseDelta = FVector2(0.0f));

        void BeginHold() { bHolding = true; }
        void EndHold()   { bHolding = false; }

        // Runs the held events in arrival order and clears them along with the held motion.
        RUNTIME_API void Release();

        FVector2 GetHeldMouseDelta() const { return HeldMouseDelta; }

    private:

        TVector<TFunction<void()>> Held;
        FVector2                   HeldMouseDelta = FVector2(0.0f);
        bool                       bHolding = false;
    };

    DECLARE_MULTICAST_DELEGATE(FWindowKeyDelegate, FWindow*, const FKeyInput&);
    // A typed character as a Unicode code point, after the keyboard layout and dead keys are applied.
    DECLARE_MULTICAST_DELEGATE(FWindowCharDelegate, FWindow*, uint32);
    DECLARE_MULTICAST_DELEGATE(FWindowMouseButtonDelegate, FWindow*, const FMouseButtonInput&);
    DECLARE_MULTICAST_DELEGATE(FWindowMouseMoveDelegate, FWindow*, const FMouseMoveInput&);
    DECLARE_MULTICAST_DELEGATE(FWindowScrollDelegate, FWindow*, const FMouseScrollInput&);
    DECLARE_MULTICAST_DELEGATE(FWindowFileDropDelegate, FWindow*, const TVector<FFixedString>&, float, float);
    DECLARE_MULTICAST_DELEGATE(FWindowCloseDelegate, FWindow*);
}
