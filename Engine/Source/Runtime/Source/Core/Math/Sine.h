#pragma once
#include "Core/Object/ObjectMacros.h"
#include "Sine.generated.h"


namespace Lumina
{
    REFLECT()
    enum class ESineWaveType : uint8
    {
        Sine,           // sin(x)
        Cosine,         // cos(x) - 90 degree phase shift
        Triangle,       // Linear up/down
        Square,         // Snap between min/max
        Sawtooth,       // Linear rise, instant drop
        Bounce,         // Bouncing motion
        SmoothStep      // Smoothed square wave
    };
}
