#include "Platform/Time/PlatformTime.h"
#include "RuntimePCH.h"
#include "JobProfiler.h"

#if USING(WITH_EDITOR)

#include "JobScheduler.h"
#include "Core/Console/ConsoleVariable.h"


namespace Lumina
{
    namespace
    {
        TConsoleVar<bool> CVarTaskProfilerEnabled("task.Profiler.Enabled", false,
            "Record per-worker fiber/job spans for the Task System profiler tool. Costs a little per job.");

        double SteadyNowMs()
        {
            return PlatformTime::Seconds() * 1000.0;
        }
    }

    FJobProfiler& FJobProfiler::Get()
    {
        static FJobProfiler Instance;
        return Instance;
    }

    double FJobProfiler::NowMs() { return SteadyNowMs(); }

    void FJobProfiler::EnsureSized()
    {
        const uint32 Slots = Jobs::GetNumThreadSlots();
        if (Lanes.size() == Slots)
        {
            return;
        }
        Lanes.clear();
        Lanes.reserve(Slots);
        for (uint32 i = 0; i < Slots; ++i)
        {
            Lanes.emplace_back(MakeUnique<FLane>());
        }
    }

    void FJobProfiler::Shutdown()
    {
        bEnabled.store(false, std::memory_order_relaxed);
        Lanes.clear();
        Latest = FJobProfFrame{};
    }

    void FJobProfiler::PushHistory(TVector<float>& H, float V)
    {
        H.push_back(V);
        if (H.size() > HistorySize)
        {
            H.erase(H.begin());
        }
    }

    void FJobProfiler::BeginFrame()
    {
        const bool Want = CVarTaskProfilerEnabled.GetValue() && Jobs::IsInitialized();
        bEnabled.store(Want, std::memory_order_relaxed);
        if (!Want)
        {
            return;
        }

        EnsureSized();

        FrameStartMs = NowMs();
        JobsRunCtr.store(0, std::memory_order_relaxed);
        Parks.store(0, std::memory_order_relaxed);
        Migrations.store(0, std::memory_order_relaxed);
        WorkerJobs.store(0, std::memory_order_relaxed);
        ExternalJobs.store(0, std::memory_order_relaxed);
        Resumes.store(0, std::memory_order_relaxed);
        AffinityOpps.store(0, std::memory_order_relaxed);
        Starvations.store(0, std::memory_order_relaxed);
        PopConcSum.store(0, std::memory_order_relaxed);
        PopConcSamples.store(0, std::memory_order_relaxed);
        PopConcMax.store(0, std::memory_order_relaxed);

        for (TUniquePtr<FLane>& LP : Lanes)
        {
            FScopeLock Lock(LP->Lock);
            LP->Spans.clear(); // keeps capacity
        }
    }

    void FJobProfiler::EndFrame()
    {
        if (!bEnabled.load(std::memory_order_relaxed))
        {
            return;
        }

        const double End = NowMs();

        Latest.Spans.clear();
        for (TUniquePtr<FLane>& LP : Lanes)
        {
            FScopeLock Lock(LP->Lock);
            for (const FJobProfSpan& S : LP->Spans)
            {
                Latest.Spans.push_back(S);
            }
        }
        Latest.FrameStartMs = FrameStartMs;
        Latest.FrameEndMs   = End;
        Latest.JobsRun      = JobsRunCtr.load(std::memory_order_relaxed);
        Latest.Parks        = Parks.load(std::memory_order_relaxed);
        Latest.Migrations   = Migrations.load(std::memory_order_relaxed);
        Latest.WorkerJobs   = WorkerJobs.load(std::memory_order_relaxed);
        Latest.ExternalJobs = ExternalJobs.load(std::memory_order_relaxed);
        Latest.Resumes      = Resumes.load(std::memory_order_relaxed);
        Latest.AffinityOpps = AffinityOpps.load(std::memory_order_relaxed);
        Latest.Starvations  = Starvations.load(std::memory_order_relaxed);
        Latest.MaxPoppers   = PopConcMax.load(std::memory_order_relaxed);
        const uint32 PopSamples = PopConcSamples.load(std::memory_order_relaxed);
        Latest.AvgPoppers   = PopSamples ? static_cast<float>(static_cast<double>(PopConcSum.load(std::memory_order_relaxed)) / PopSamples) : 0.0f;

        Jobs::FJobLiveStats LS;
        Jobs::GetLiveStats(LS);

        PushHistory(JobsHist,    static_cast<float>(Latest.JobsRun));
        PushHistory(MigHist,     static_cast<float>(Latest.Migrations));
        PushHistory(FibersHist,  static_cast<float>(LS.FibersInUse));
        PushHistory(PoppersHist, Latest.AvgPoppers);
    }

    void FJobProfiler::SliceBegin(uint32 Worker, uint16 Fiber, const char* Name, double Ms)
    {
        if (Worker >= Lanes.size())
        {
            return;
        }
        FLane& L = *Lanes[Worker];
        L.Open    = FJobProfSpan{ Name ? Name : "job", static_cast<uint16>(Worker), Fiber, Ms, Ms, static_cast<uint8>(EJobSpanKind::Ran) };
        L.HasOpen = true;
        JobsRunCtr.fetch_add(1, std::memory_order_relaxed);
    }

    void FJobProfiler::SliceEnd(uint32 Worker, bool Parked, double Ms)
    {
        if (Worker >= Lanes.size())
        {
            return;
        }
        FLane& L = *Lanes[Worker];
        if (!L.HasOpen)
        {
            return;
        }
        L.Open.EndMs = Ms;
        L.Open.Kind  = static_cast<uint8>(Parked ? EJobSpanKind::RanThenParked : EJobSpanKind::Ran);
        L.HasOpen    = false;
        FScopeLock Lock(L.Lock);
        L.Spans.push_back(L.Open);
    }

    void FJobProfiler::IdleBegin(uint32 Worker, double Ms)
    {
        if (Worker >= Lanes.size())
        {
            return;
        }
        FLane& L  = *Lanes[Worker];
        L.HasIdle = true;
        L.IdleStart = Ms;
    }

    void FJobProfiler::IdleEnd(uint32 Worker, double Ms)
    {
        if (Worker >= Lanes.size())
        {
            return;
        }
        FLane& L = *Lanes[Worker];
        if (!L.HasIdle)
        {
            return;
        }
        L.HasIdle = false;
        FJobProfSpan S{ "idle", static_cast<uint16>(Worker), 0xFFFF, L.IdleStart, Ms, static_cast<uint8>(EJobSpanKind::Idle) };
        FScopeLock Lock(L.Lock);
        L.Spans.push_back(S);
    }
}

#endif
