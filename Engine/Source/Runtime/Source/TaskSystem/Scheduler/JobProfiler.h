#pragma once

#include "Core/LuminaMacros.h"

#if USING(WITH_EDITOR)

#include "Containers/Vector.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Sync.h"
#include "Memory/SmartPtr.h"
#include "ModuleAPI.h"

namespace Lumina
{
    enum class EJobSpanKind : uint8
    {
        Ran,
        Idle,
    };

    struct FJobProfSpan
    {
        const char*  Name    = nullptr;
        uint16       Worker  = 0;
        uint16       Depth   = 0;
        double       StartMs = 0.0;
        double       EndMs   = 0.0;
        EJobSpanKind Kind    = EJobSpanKind::Ran;
    };

    struct FJobProfFrame
    {
        TVector<FJobProfSpan> Spans;
        double FrameStartMs = 0.0;
        double FrameEndMs   = 0.0;
        uint32 JobsRun      = 0;
        uint32 JobsHelped   = 0;
        uint32 WorkerJobs   = 0;
        uint32 ExternalJobs = 0;
    };

    // Per-thread job spans for the editor's task view; recording only while task.Profiler.Enabled is on.
    class RUNTIME_API FJobProfiler
    {
    public:

        static constexpr uint32 HistorySize = 240;

        static FJobProfiler& Get();
        static double NowMs();

        NODISCARD bool IsEnabled() const { return bEnabled.load(std::memory_order_relaxed); }

        void Shutdown();
        void BeginFrame();
        void EndFrame();

        void JobBegin(uint32 Slot, const char* Name, bool bHelping);
        void JobEnd(uint32 Slot);
        void IdleBegin(uint32 Slot);
        void IdleEnd(uint32 Slot);
        void NoteSubmit(uint32 Count, bool bByWorker) { (bByWorker ? WorkerJobs : ExternalJobs).fetch_add(Count, std::memory_order_relaxed); }

        NODISCARD const FJobProfFrame&  GetLatest() const      { return Latest; }
        NODISCARD const TVector<float>& JobsHistory() const    { return JobsHist; }
        NODISCARD const TVector<float>& HelpedHistory() const  { return HelpedHist; }

    private:

        static constexpr uint32 kMaxDepth = 32;

        struct FLane
        {
            FMutex                Lock;
            TVector<FJobProfSpan> Spans;
            const char*           Names[kMaxDepth] = {};
            double                OpenMs = 0.0;
            uint32                Depth  = 0;
            uint32                Overflow = 0; // Nesting past kMaxDepth, popped before Depth so the two stay matched.
            double                IdleStartMs = 0.0;
            bool                  bIdle  = false;
        };

        void CloseOpen(FLane& Lane, uint32 Slot, double Now);
        void EnsureSized();

        TVector<TUniquePtr<FLane>> Lanes;
        FJobProfFrame              Latest;
        TAtomic<bool>              bEnabled{false};
        TAtomic<uint32>            JobsRun{0};
        TAtomic<uint32>            JobsHelped{0};
        TAtomic<uint32>            WorkerJobs{0};
        TAtomic<uint32>            ExternalJobs{0};
        double                     FrameStartMs = 0.0;
        TVector<float>             JobsHist;
        TVector<float>             HelpedHist;
    };
}

#endif
