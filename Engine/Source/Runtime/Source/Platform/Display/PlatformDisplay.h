#pragma once

#include "Platform/GenericPlatform.h"
#include "Platform/Platform.h"

namespace Lumina::PlatformDisplay
{
    struct FHDRDisplayInfo
    {
        // The OS drives this monitor with an HDR10 signal, so an HDR swapchain reaches it unconverted.
        bool  bHDREnabled  = false;
        float PeakNits     = 0.0f;   // zero when the display reports none
        float SDRWhiteNits = 0.0f;   // where the OS places SDR white, zero when it cannot say
    };

    // The monitor holding the screen point, or the nearest one. Cheap enough to poll about once a second.
    NODISCARD RUNTIME_API FHDRDisplayInfo QueryHDRDisplay(int32 ScreenX, int32 ScreenY);
}
