#pragma once

#include "Core/LuminaMacros.h"

#if defined(LUMINA_WITH_GPU_PROFILING)

#include "Containers/StaticArray.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Threading/Thread.h"
#include "ModuleAPI.h"
#include "RHI.h"

namespace Lumina
{
    // Times are milliseconds from the frame's first timestamp, so a scope tree lays out as a flame graph.
    struct FGPUProfileScope
    {
        FFixedString Name;
        int32        ParentIndex = INDEX_NONE;
        int32        Depth       = 0;
        double       StartMs     = 0.0;
        double       EndMs       = 0.0;

        NODISCARD double DurationMs() const { return EndMs - StartMs; }
    };

    struct FGPUProfileFrame
    {
        TVector<FGPUProfileScope> Scopes;
        uint64                    FrameNumber = 0;
        double                    TotalMs     = 0.0;
    };

    /** Per-pass GPU timings, bracketing the debug markers the scene renderer already emits. */
    class RUNTIME_API FGPUProfiler
    {
    public:

        static constexpr uint32 MaxScopesPerFrame = 256;
        static constexpr uint32 FrameHistorySize  = 240;

        static FGPUProfiler& Get();

        FGPUProfiler() = default;
        FGPUProfiler(const FGPUProfiler&) = delete;
        FGPUProfiler& operator = (const FGPUProfiler&) = delete;

        NODISCARD bool IsEnabled() const;
        void SetEnabled(bool bEnabled);

        /** Releases the query pools. Called before the device goes away. */
        void Shutdown();

        /** Resolves the slot's previous frame, then arms it for recording. */
        void BeginFrame(uint32 Slot);

        //~ Driven by RHI::CmdBeginMarker / CmdEndMarker, so every existing marker is a timed scope.
        void BeginScope(RHI::FCmdListH CL, const char* Name);
        void EndScope(RHI::FCmdListH CL);

        NODISCARD const FGPUProfileFrame& GetLatest() const { return Latest; }
        NODISCARD TSpan<const float> GetFrameHistory() const { return FrameTimeHistory; }
        NODISCARD bool HasResolvedFrame() const { return bHasResolvedFrame; }

    private:

        // A scope while it is being recorded, before its timestamps are readable.
        struct FPendingScope
        {
            FFixedString Name;
            int32        ParentIndex = INDEX_NONE;
            int32        Depth       = 0;
            uint32       BeginQuery  = 0;
            uint32       EndQuery    = 0;
        };

        struct FSlot
        {
            RHI::FQueryPoolH       Pool;
            TVector<FPendingScope> Scopes;
            TVector<int32>         Stack;
            uint32                 QueryCursor = 0;
            uint64                 FrameNumber = 0;
            bool                   bPendingResolve = false;
        };

        void Resolve(FSlot& Slot);

        TArray<FSlot, RHI::kFramesInFlight> Slots;
        uint32                              CurrentSlot = 0;
        uint64                              FrameCounter = 0;

        FGPUProfileFrame                    Latest;
        TVector<float>                      FrameTimeHistory;
        bool                                bHasResolvedFrame = false;
        bool                                bInitialized = false;

        // Markers record from whichever thread owns the command list, so the stack is shared state.
        FMutex                              Mutex;
    };
}

#endif
