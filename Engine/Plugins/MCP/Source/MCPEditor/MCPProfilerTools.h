#pragma once

#include "Containers/HashTable.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Threading/Sync.h"
#include "Core/Threading/Thread.h"
#include "Memory/SmartPtr.h"

#include "MCPProfilerTools.generated.h"

namespace Lumina
{
    REFLECT()
    struct MCPEDITOR_API SProfilerCaptureParams
    {
        GENERATED_BODY()

        // How long to record while the editor keeps running, up to 60.
        PROPERTY()
        float Seconds = 5.0f;

        // Lifts the frame rate cap while recording, so the frame time shows the real cost rather than the cap.
        PROPERTY()
        bool bUncapFrameRate = true;

        // False returns at once with only CaptureFile set; read the results later with profiler.report.
        PROPERTY()
        bool bWait = true;

        // How many of the costliest CPU zones and GPU passes to report.
        PROPERTY()
        int32 Top = 20;

        // Only zones whose name contains this, case-insensitive.
        PROPERTY()
        FString Filter;

        // Rank by time in each zone minus its children, which finds the zone doing the work rather than its caller.
        PROPERTY()
        bool bSelfTime = false;
    };

    REFLECT()
    enum class EProfilerReportMode : uint8
    {
        Zones,
        Hitches,
    };

    REFLECT()
    struct MCPEDITOR_API SProfilerReportParams
    {
        GENERATED_BODY()

        PROPERTY()
        EProfilerReportMode Mode = EProfilerReportMode::Zones;

        // Hitches only. The trace to read. Empty reads the last capture.
        PROPERTY()
        FString CaptureFile;

        // Zones only, as are Filter and bSelfTime. See profiler.capture.
        PROPERTY()
        int32 Top = 20;

        PROPERTY()
        FString Filter;

        PROPERTY()
        bool bSelfTime = false;

        // Hitches only. Frames longer than this are hitches. Zero picks twice the median, and at least 8 ms over it.
        PROPERTY()
        float ThresholdMs = 0.0f;

        // Hitches only.
        PROPERTY()
        int32 MaxHitches = 8;

        // Hitches only. Zones to list per hitch, ranked by self time.
        PROPERTY()
        int32 TopZones = 10;
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

    REFLECT()
    struct MCPEDITOR_API SProfilerReportResult
    {
        GENERATED_BODY()

        // Filled in Zones mode.
        PROPERTY()
        SProfilerCaptureResult Zones;

        // Filled in Hitches mode.
        PROPERTY()
        SProfilerHitchResult Hitches;
    };

    namespace MCP
    {
        // One capture at a time runs on its own thread, so the scene it measures can still be driven through other tools.
        struct FProfilerCaptureJob
        {
            FString File;
            FString Failure;
            int32 Seconds = 0;
            bool bCaptured = false;
            FThread Worker;
        };

        // Owned by FMCPEditorModule, which shuts it down before the DLL unloads.
        struct FProfilerToolsState
        {
            FMutex Mutex;
            TUniquePtr<FProfilerCaptureJob> Job;
            FString LastCaptureFile;

            void Shutdown();
        };

        struct FProfilerZoneTotals
        {
            double TotalNs = 0.0;
            double MaxNs = 0.0;
            int64 Count = 0;
        };

        MCPEDITOR_API TVector<FStringView> SplitProfilerFields(FStringView Line);

        // Adds one tracy-csvexport line to the totals, skipping the line while bHeaderPending is set and clearing it.
        MCPEDITOR_API void AddTracyZoneLine(FStringView Line, bool bGpuRows, bool& bHeaderPending, THashMap<FString, FProfilerZoneTotals>& Out);

        MCPEDITOR_API TVector<SProfilerZone> RankProfilerZones(const THashMap<FString, FProfilerZoneTotals>& Zones, int32 Frames, int32 Top, const FString& Filter);

        void RegisterProfilerTools(FStringView Owner);
    }
}
