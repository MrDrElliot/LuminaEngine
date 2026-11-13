#include "JobProfiler.h"

#if USING(WITH_EDITOR)

#include "TaskSystem/Fork/ForkRuntime.h"
#include "Core/Console/ConsoleVariable.h"
#include "Platform/Time/PlatformTime.h"

namespace Lumina
{
    namespace JobProfilerInternal
    {
        TConsoleVar<bool> CVarTaskProfilerEnabled("task.Profiler.Enabled", false,
            "Record per-thread job spans for the Task System profiler tool. Costs a little per job.");

        void PushHistory(TVector<float>& History, float Value)
        {
            History.Add(Value);
            if (static_cast<uint32>(History.Num()) > FJobProfiler::HistorySize)
            {
                History.RemoveAt(0);
            }
        }
    }

    FJobProfiler& FJobProfiler::Get()
    {
        static FJobProfiler Instance;
        return Instance;
    }

    double FJobProfiler::NowMs()
    {
        return PlatformTime::Seconds() * 1000.0;
    }

    void FJobProfiler::EnsureSized()
    {
        const uint32 Slots = Jobs::GetNumThreadSlots();
        if (static_cast<uint32>(Lanes.Num()) == Slots)
        {
            return;
        }
        Lanes.Reset();
        Lanes.Reserve(Slots);
        for (uint32 Slot = 0; Slot < Slots; ++Slot)
        {
            Lanes.Emplace(MakeUnique<FLane>());
        }
    }

    void FJobProfiler::Shutdown()
    {
        bEnabled.store(false, std::memory_order_relaxed);
        Lanes.Reset();
        Latest = FJobProfFrame{};
    }

    void FJobProfiler::BeginFrame()
    {
        const bool bWant = JobProfilerInternal::CVarTaskProfilerEnabled.GetValue() && Jobs::IsRuntimeRunning();
        bEnabled.store(bWant, std::memory_order_relaxed);
        if (!bWant)
        {
            return;
        }
        EnsureSized();
        FrameStartMs = NowMs();
        JobsRun.store(0, std::memory_order_relaxed);
        JobsHelped.store(0, std::memory_order_relaxed);
        WorkerJobs.store(0, std::memory_order_relaxed);
        ExternalJobs.store(0, std::memory_order_relaxed);
        for (TUniquePtr<FLane>& Lane : Lanes)
        {
            FScopeLock Lock(Lane->Lock);
            Lane->Spans.Reset();
        }
    }

    void FJobProfiler::EndFrame()
    {
        if (!bEnabled.load(std::memory_order_relaxed))
        {
            return;
        }
        Latest.Spans.Reset();
        for (TUniquePtr<FLane>& Lane : Lanes)
        {
            FScopeLock Lock(Lane->Lock);
            Latest.Spans.Append(Lane->Spans);
        }
        Latest.FrameStartMs = FrameStartMs;
        Latest.FrameEndMs   = NowMs();
        Latest.JobsRun      = JobsRun.load(std::memory_order_relaxed);
        Latest.JobsHelped   = JobsHelped.load(std::memory_order_relaxed);
        Latest.WorkerJobs   = WorkerJobs.load(std::memory_order_relaxed);
        Latest.ExternalJobs = ExternalJobs.load(std::memory_order_relaxed);
        JobProfilerInternal::PushHistory(JobsHist, static_cast<float>(Latest.JobsRun));
        JobProfilerInternal::PushHistory(HelpedHist, static_cast<float>(Latest.JobsHelped));
    }

    // A nested job closes the span of the one it interrupted, which reopens when the nested one ends.
    void FJobProfiler::CloseOpen(FLane& Lane, uint32 Slot, double Now)
    {
        if (Lane.Depth == 0)
        {
            return;
        }
        FJobProfSpan Span;
        Span.Name    = Lane.Names[Lane.Depth - 1];
        Span.Worker  = static_cast<uint16>(Slot);
        Span.Depth   = static_cast<uint16>(Lane.Depth - 1);
        Span.StartMs = Lane.OpenMs;
        Span.EndMs   = Now;
        FScopeLock Lock(Lane.Lock);
        Lane.Spans.Add(Span);
    }

    void FJobProfiler::JobBegin(uint32 Slot, const char* Name, bool bHelping)
    {
        if (Slot >= static_cast<uint32>(Lanes.Num()))
        {
            return;
        }
        FLane& Lane = *Lanes[Slot];
        const double Now = NowMs();
        CloseOpen(Lane, Slot, Now);
        if (Lane.Depth < kMaxDepth)
        {
            Lane.Names[Lane.Depth] = Name != nullptr ? Name : "job";
            ++Lane.Depth;
        }
        else
        {
            ++Lane.Overflow;
        }
        Lane.OpenMs = Now;
        JobsRun.fetch_add(1, std::memory_order_relaxed);
        if (bHelping || Lane.Depth > 1)
        {
            JobsHelped.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void FJobProfiler::JobEnd(uint32 Slot)
    {
        if (Slot >= static_cast<uint32>(Lanes.Num()))
        {
            return;
        }
        FLane& Lane = *Lanes[Slot];
        if (Lane.Depth == 0 && Lane.Overflow == 0)
        {
            return;
        }
        const double Now = NowMs();
        CloseOpen(Lane, Slot, Now);
        if (Lane.Overflow > 0)
        {
            --Lane.Overflow;
        }
        else
        {
            --Lane.Depth;
        }
        Lane.OpenMs = Now;
    }

    void FJobProfiler::IdleBegin(uint32 Slot)
    {
        if (Slot >= static_cast<uint32>(Lanes.Num()))
        {
            return;
        }
        FLane& Lane = *Lanes[Slot];
        Lane.bIdle = true;
        Lane.IdleStartMs = NowMs();
    }

    void FJobProfiler::IdleEnd(uint32 Slot)
    {
        if (Slot >= static_cast<uint32>(Lanes.Num()))
        {
            return;
        }
        FLane& Lane = *Lanes[Slot];
        if (!Lane.bIdle)
        {
            return;
        }
        Lane.bIdle = false;
        FJobProfSpan Span;
        Span.Name    = "idle";
        Span.Worker  = static_cast<uint16>(Slot);
        Span.StartMs = Lane.IdleStartMs;
        Span.EndMs   = NowMs();
        Span.Kind    = EJobSpanKind::Idle;
        FScopeLock Lock(Lane.Lock);
        Lane.Spans.Add(Span);
    }
}

#endif
