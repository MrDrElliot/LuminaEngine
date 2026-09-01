#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Delegates/Delegate.h"
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

    DECLARE_MULTICAST_DELEGATE(FWindowKeyDelegate, FWindow*, const FKeyInput&);
    DECLARE_MULTICAST_DELEGATE(FWindowMouseButtonDelegate, FWindow*, const FMouseButtonInput&);
    DECLARE_MULTICAST_DELEGATE(FWindowMouseMoveDelegate, FWindow*, const FMouseMoveInput&);
    DECLARE_MULTICAST_DELEGATE(FWindowScrollDelegate, FWindow*, const FMouseScrollInput&);
    DECLARE_MULTICAST_DELEGATE(FWindowFileDropDelegate, FWindow*, const TVector<FFixedString>&, float, float);
    DECLARE_MULTICAST_DELEGATE(FWindowCloseDelegate, FWindow*);
}
