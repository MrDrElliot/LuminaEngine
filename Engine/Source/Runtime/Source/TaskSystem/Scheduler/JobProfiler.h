#pragma once

#include "Core/LuminaMacros.h"

// Editor-only collector for the Task System profiler tool. Records per-worker job/idle spans and
// fiber-pool counters while actively profiling (CVar task.Profiler.Enabled). Compiled out entirely in
// non-editor builds. Hooks live in JobScheduler.cpp; the editor tool reads GetLatest()/history.
#if USING(WITH_EDITOR)

#include "Containers/Array.h"
#include "Core/Threading/Thread.h"
#include "Core/Threading/Atomic.h"
#include "Memory/SmartPtr.h"
#include "ModuleAPI.h"

namespace Lumina
{
    enum class EJobSpanKind : uint8 { Ran = 0, RanThenParked = 1, Idle = 2 };

    struct FJobProfSpan
    {
        const char* Name    = nullptr;
        uint16      Worker  = 0;
        uint16      Fiber   = 0xFFFF;
        double      StartMs = 0.0;
        double      EndMs   = 0.0;
        uint8       Kind    = 0; // EJobSpanKind
    };

    struct FJobProfFrame
    {
        TVector<FJobProfSpan> Spans;
        double FrameStartMs = 0.0;
        double FrameEndMs   = 0.0;
        uint32 JobsRun      = 0;
        uint32 Parks        = 0;
        uint32 Migrations   = 0;

        // Scheduling-advisor inputs (the data behind the "would work-stealing help?" verdict).
        uint32 WorkerJobs   = 0;     // jobs submitted from a worker thread (fork-join children)
        uint32 ExternalJobs = 0;     // jobs submitted from a non-worker thread (main/render/...)
        uint32 Resumes      = 0;     // parked-fiber resumes (Migrations is the migrated subset)
        uint32 AffinityOpps = 0;     // resumes whose owner worker was idle (affinity would be a free win)
        uint32 Starvations  = 0;     // fiber-pool starvation episodes (all fibers busy, jobs pending)
        float  AvgPoppers   = 0.0f;  // mean workers dequeuing the shared queue at once (contention proxy)
        uint32 MaxPoppers   = 0;     // peak simultaneous poppers in the frame
    };

    class RUNTIME_API FJobProfiler
    {
    public:

        static constexpr uint32 HistorySize = 240;

        static FJobProfiler& Get();
        static double        NowMs();

        bool IsEnabled() const { return bEnabled.load(std::memory_order_relaxed); }

        void Shutdown();

        // Driven from the engine loop (next to FCPUProfiler::Begin/EndFrame).
        void BeginFrame();
        void EndFrame();

        // Scheduler hooks; callers pre-check IsEnabled() for span events.
        void SliceBegin(uint32 Worker, uint16 Fiber, const char* Name, double Ms);
        void SliceEnd(uint32 Worker, bool Parked, double Ms);
        void IdleBegin(uint32 Worker, double Ms);
        void IdleEnd(uint32 Worker, double Ms);
        void NotePark()                { Parks.fetch_add(1, std::memory_order_relaxed); }
        void NoteResume(bool Migrated) { Resumes.fetch_add(1, std::memory_order_relaxed); if (Migrated) Migrations.fetch_add(1, std::memory_order_relaxed); }

        // Advisor hooks. Cheap relaxed atomics; callers gate on IsEnabled() so off-state cost is nil.
        void NoteSubmit(uint32 Count, bool ByWorker)
        {
            (ByWorker ? WorkerJobs : ExternalJobs).fetch_add(Count, std::memory_order_relaxed);
        }
        void NotePop(uint32 Concurrency) // workers simultaneously inside the shared-queue dequeue
        {
            PopConcSum.fetch_add(Concurrency, std::memory_order_relaxed);
            PopConcSamples.fetch_add(1, std::memory_order_relaxed);
            uint32 Prev = PopConcMax.load(std::memory_order_relaxed);
            while (Concurrency > Prev && !PopConcMax.compare_exchange_weak(Prev, Concurrency, std::memory_order_relaxed)) {}
        }
        void NoteStarvation() { Starvations.fetch_add(1, std::memory_order_relaxed); }
        void NoteAffinityOpportunity() { AffinityOpps.fetch_add(1, std::memory_order_relaxed); }

        const FJobProfFrame&  GetLatest() const          { return Latest; }
        const TVector<float>& JobsHistory() const        { return JobsHist; }
        const TVector<float>& FibersInUseHistory() const { return FibersHist; }
        const TVector<float>& MigrationsHistory() const  { return MigHist; }
        const TVector<float>& PoppersHistory() const     { return PoppersHist; }

    private:

        struct FLane
        {
            FMutex                Lock;
            TVector<FJobProfSpan> Spans;
            bool                  HasOpen   = false;
            FJobProfSpan          Open;
            bool                  HasIdle   = false;
            double                IdleStart = 0.0;
        };

        void EnsureSized();
        static void PushHistory(TVector<float>& H, float V);

        TVector<TUniquePtr<FLane>> Lanes;       // per thread-slot (FMutex is non-movable → boxed)
        FJobProfFrame              Latest;
        TAtomic<bool>              bEnabled{false};
        TAtomic<uint32>            JobsRunCtr{0};
        TAtomic<uint32>            Parks{0};
        TAtomic<uint32>            Migrations{0};
        TAtomic<uint32>            WorkerJobs{0};
        TAtomic<uint32>            ExternalJobs{0};
        TAtomic<uint32>            Resumes{0};
        TAtomic<uint32>            AffinityOpps{0};
        TAtomic<uint32>            Starvations{0};
        TAtomic<uint64>            PopConcSum{0};
        TAtomic<uint32>            PopConcSamples{0};
        TAtomic<uint32>            PopConcMax{0};
        double                     FrameStartMs = 0.0;
        TVector<float>             JobsHist, FibersHist, MigHist, PoppersHist;
    };
}

#endif
