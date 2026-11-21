#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Scalar.h"
#include "HDROutput.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EHDROutput : uint8
    {
        // Always SDR, which an HDR desktop maps at its SDR brightness.
        Off,

        // HDR10 whenever the window's monitor is in HDR mode, else SDR.
        Auto,

        // HDR10 wherever the surface offers it, even on a desktop the OS runs in SDR.
        On,
    };

    // What the primary window presents with this frame.
    struct FHDROutputState
    {
        bool  bActive        = false;
        float PaperWhiteNits = 200.0f;
        float PeakNits       = 1000.0f;

        // Range above paper white the highlights may use, 0 when presenting SDR.
        float GetHeadroomStops() const
        {
            return bActive && PeakNits > PaperWhiteNits ? Math::Log2(PeakNits / PaperWhiteNits) : 0.0f;
        }
    };
}
