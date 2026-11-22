#pragma once

#include "Core/LuminaMacros.h"
#include "Lumina.h"

#if defined(LUMINA_WITH_GPU_PROFILING)

#include "Containers/StaticArray.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "Core/Threading/Atomic.h"
#include "ModuleAPI.h"
#include "RHI.h"

namespace Lumina
{
    inline constexpr uint32 kGPUProfileQueueCount = 3;

    // Times are milliseconds from the frame's first timestamp on any queue, so every queue shares one time axis.
    struct FGPUProfileScope
    {
        FFixedString    Name;
        int32           ParentIndex = Constants::kIndexNone;
        int32           Depth       = 0;
        RHI::EQueueType Queue       = RHI::EQueueType::Graphics;
        double          StartMs     = 0.0;
        double          EndMs       = 0.0;

        NODISCARD double DurationMs() const { return EndMs - StartMs; }
    };

    // Scopes are sorted by queue, then start, so each queue reads as its own lane and a pass keeps its index across frames.
    struct FGPUProfileFrame
    {
        TVector<FGPUProfileScope> Scopes;
        uint64                    FrameNumber = 0;
        // First timestamp to last on any queue.
        double                    TotalMs     = 0.0;
        // Union of each queue's root scopes, so nested and back to back passes count once.
        double                    QueueBusyMs[kGPUProfileQueueCount] = {};
        // Time the compute queue was busy while graphics was too, which is what async compute bought.
        double                    AsyncOverlapMs = 0.0;
        uint32                    DroppedScopes  = 0;

        NODISCARD bool HasQueue(RHI::EQueueType Queue) const { return QueueBusyMs[(uint32)Queue] > 0.0; }
    };

    RUNTIME_API const char* GPUProfileQueueName(RHI::EQueueType Queue);

    // Per-pass GPU timings, bracketing the debug markers the scene renderer already emits.
    class RUNTIME_API FGPUProfiler
    {
    public:

        static constexpr uint32 MaxScopesPerFrame = 512;
        static constexpr uint32 FrameHistorySize  = 240;

        static FGPUProfiler& Get();

        FGPUProfiler() = default;
        ~FGPUProfiler() = default;
        FGPUProfiler(const FGPUProfiler&) = delete;
        FGPUProfiler& operator = (const FGPUProfiler&) = delete;
        FGPUProfiler(FGPUProfiler&&) = delete;
        FGPUProfiler& operator=(FGPUProfiler&&) = delete;

        NODISCARD bool IsEnabled() const;
        void SetEnabled(bool bEnabled);

        // Releases the query pools. Called before the device goes away.
        void Shutdown();

        // Resolves the slot's previous frame, then arms it for recording.
        void BeginFrame(uint32 Slot);

        // Driven by RHI::CmdBeginMarker, which keeps the nesting per command list and passes the open parent in.
        int32 BeginScope(RHI::FCmdListH CL, const char* Name, int32 ParentIndex, int32 Depth);
        void  EndScope(RHI::FCmdListH CL, int32 ScopeIndex);

        NODISCARD const FGPUProfileFrame& GetLatest() const { return Latest; }
        NODISCARD TSpan<const float> GetFrameHistory() const { return FrameTimeHistory; }
        NODISCARD bool HasResolvedFrame() const { return bHasResolvedFrame; }

    private:

        // A scope while it is being recorded, before its timestamps are readable; its queries are 2 * index and the one after.
        struct FPendingScope
        {
            FFixedString    Name;
            int32           ParentIndex = Constants::kIndexNone;
            int32           Depth       = 0;
            RHI::EQueueType Queue       = RHI::EQueueType::Graphics;
        };

        struct FSlot
        {
            RHI::FQueryPoolH       Pool;
            TVector<FPendingScope> Scopes;
            // Claimed lock-free, since markers record from whichever thread owns the command list.
            TAtomic<uint32>        NumClaimed{0};
            uint64                 FrameNumber = 0;
            bool                   bHostReset = false;
        };

        void Resolve(FSlot& Slot);

        TArray<FSlot, RHI::kFramesInFlight> Slots;
        TAtomic<uint32>                     CurrentSlot{0};
        uint64                              FrameCounter = 0;

        FGPUProfileFrame                    Latest;
        TVector<float>                      FrameTimeHistory;
        bool                                bHasResolvedFrame = false;
        TAtomic<bool>                       bInitialized{false};
    };
}

#endif
