#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"

#include "MCPProfilerTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SProfilerCaptureParams
    {
        GENERATED_BODY()

        // How long to record while the editor keeps running.
        PROPERTY()
        float Seconds = 5.0f;

        // How many of the costliest CPU zones and GPU passes to report.
        PROPERTY()
        int32 Top = 20;

        // Only zones whose name contains this, case-insensitive. Empty keeps them all.
        PROPERTY()
        FString Filter;

        // Time spent in each zone minus its children, which finds the zone doing the work rather than the one calling it.
        PROPERTY()
        bool bSelfTime = false;

        // Lifts the frame rate cap for the capture and puts it back after, so the frame time shows the real cost rather than the cap.
        PROPERTY()
        bool bUncapFrameRate = true;
    };

    REFLECT()
    struct MCPEDITOR_API SProfilerStartParams
    {
        GENERATED_BODY()

        PROPERTY()
        float Seconds = 5.0f;

        PROPERTY()
        bool bUncapFrameRate = true;
    };

    REFLECT()
    struct MCPEDITOR_API SProfilerStartResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString CaptureFile;
    };

    REFLECT()
    struct MCPEDITOR_API SProfilerReportParams
    {
        GENERATED_BODY()

        PROPERTY()
        int32 Top = 20;

        PROPERTY()
        FString Filter;

        PROPERTY()
        bool bSelfTime = false;
    };

    REFLECT()
    struct MCPEDITOR_API SProfilerZone
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        // Average cost per captured frame, which is what adds up to the frame time.
        PROPERTY()
        float MsPerFrame = 0.0f;

        PROPERTY()
        int32 Count = 0;

        PROPERTY()
        float MeanUs = 0.0f;

        PROPERTY()
        float MaxUs = 0.0f;
    };

    REFLECT()
    struct MCPEDITOR_API SProfilerCaptureResult
    {
        GENERATED_BODY()

        // The saved trace, which the Tracy profiler opens for the full timeline.
        PROPERTY()
        FString CaptureFile;

        PROPERTY()
        int32 Frames = 0;

        // Wall-clock time from one frame to the next, which is what the frame rate follows.
        PROPERTY()
        float PeriodMs = 0.0f;

        // Time inside the frame zone, which leaves out the GPU fence wait that runs between frames.
        PROPERTY()
        float FrameMs = 0.0f;

        PROPERTY()
        float MaxFrameMs = 0.0f;

        // Time per frame the frame-rate limiter spent waiting, which is not work.
        PROPERTY()
        float LimiterMs = 0.0f;

        // Time per frame the CPU sat on the GPU fence, which marks a GPU-bound frame.
        PROPERTY()
        float GpuWaitMs = 0.0f;

        PROPERTY()
        TVector<SProfilerZone> Cpu;

        PROPERTY()
        TVector<SProfilerZone> Gpu;
    };

    REFLECT()
    struct MCPEDITOR_API SProfilerHitchParams
    {
        GENERATED_BODY()

        // The trace to read. Empty reads the one the last profiler capture wrote.
        PROPERTY()
        FString CaptureFile;

        // Frames longer than this count as hitches. Zero picks twice the median frame, and at least 8 ms over it.
        PROPERTY()
        float ThresholdMs = 0.0f;

        PROPERTY()
        int32 MaxHitches = 8;

        // How many zones to list per hitch, ranked by the time spent in each zone itself.
        PROPERTY()
        int32 TopZones = 10;
    };

    REFLECT()
    struct MCPEDITOR_API SProfilerHitchZone
    {
        GENERATED_BODY()

        PROPERTY()
        FString Name;

        // Time inside this zone minus its children, summed over the hitch frame.
        PROPERTY()
        float SelfMs = 0.0f;

        PROPERTY()
        int32 Count = 0;
    };

    REFLECT()
    struct MCPEDITOR_API SProfilerHitch
    {
        GENERATED_BODY()

        // Seconds from the start of the capture.
        PROPERTY()
        float AtSeconds = 0.0f;

        PROPERTY()
        float FrameMs = 0.0f;

        // Zones on the thread that runs the frame.
        PROPERTY()
        TVector<SProfilerHitchZone> GameThread;

        // Zones on every other thread during the same frame, merged by name.
        PROPERTY()
        TVector<SProfilerHitchZone> Workers;
    };

    REFLECT()
    struct MCPEDITOR_API SProfilerHitchResult
    {
        GENERATED_BODY()

        PROPERTY()
        FString CaptureFile;

        PROPERTY()
        int32 Frames = 0;

        PROPERTY()
        float MedianFrameMs = 0.0f;

        PROPERTY()
        float P99FrameMs = 0.0f;

        PROPERTY()
        float ThresholdMs = 0.0f;

        // Every frame over the threshold, though only the worst MaxHitches are broken down.
        PROPERTY()
        int32 HitchCount = 0;

        PROPERTY()
        TVector<SProfilerHitch> Hitches;
    };

    namespace MCP
    {
        // Records Tracy captures of the running editor and reports where the frame goes.
        void RegisterProfilerTools(FStringView Owner);
    }
}
