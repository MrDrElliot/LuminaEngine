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

        // Lifts Core.MaxFPS for the capture and puts it back after, so the frame time shows the real cost rather than the cap.
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

    namespace MCP
    {
        // Records Tracy captures of the running editor and reports where the frame goes.
        void RegisterProfilerTools(FStringView Owner);
    }
}
