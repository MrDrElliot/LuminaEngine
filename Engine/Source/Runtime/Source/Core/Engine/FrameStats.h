#pragma once

#include "Containers/Vector.h"
#include "Platform/GenericPlatform.h"

// Wall time of every frame since the last reset, cheap enough to leave on so a long session can be checked for hitches.
namespace Lumina::FrameStats
{
    struct FHitch
    {
        double TimeSeconds = 0.0;
        float  FrameMs = 0.0f;
    };

    struct FSummary
    {
        uint64 Frames = 0;
        double WindowSeconds = 0.0;
        float  MeanMs = 0.0f;
        float  P50Ms = 0.0f;
        float  P95Ms = 0.0f;
        float  P99Ms = 0.0f;
        float  MaxMs = 0.0f;
        uint32 Over33Ms = 0;
        uint32 Over50Ms = 0;
        uint32 Over100Ms = 0;
        uint32 Over250Ms = 0;
        TVector<FHitch> Worst;
    };

    // Called once at the top of every frame with the loop clock.
    RUNTIME_API void MarkFrame(double NowSeconds);

    RUNTIME_API void Reset();

    // SinceSeconds limits it to frames that ended at or after that engine time, which lets one level be summarized on its own.
    RUNTIME_API FSummary Summarize(uint32 WorstCount, double SinceSeconds = 0.0);
}
