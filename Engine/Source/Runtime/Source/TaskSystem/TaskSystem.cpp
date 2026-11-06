#include "TaskSystem.h"

#include "Core/Profiler/Profile.h"
#include "Core/Diagnostics/HangWatchdog.h"
#include "Core/Threading/Sync.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
#include "Platform/Process/PlatformProcess.h"
#include "Platform/Time/PlatformTime.h"
#if USING(WITH_EDITOR)
#include "Scheduler/JobProfiler.h"
#endif

#include <cstdlib>
#include <cstring>

namespace Lumina::Jobs
{
    namespace TaskSystemInternal
    {
        using ParallelDetail::FRangeThunk;

        constexpr uint32 kMaxChunks         = 256;
        constexpr uint32 kCostSlots         = 256;
        constexpr uint64 kSerialBudgetNanos = 25'000;

        struct alignas(64) FCostEntry
        {
            std::atomic<uint64> Key{0};
            std::atomic<uint64> NanosPerItem{0};
        };

        FCostEntry GCostTable[kCostSlots];

        FORCEINLINE FCostEntry& CostSlot(FRangeThunk Thunk)
        {
            const uint64 Hash = (reinterpret_cast<uint64>(Thunk) >> 4) * 0x9E3779B97F4A7C15ull;
            return GCostTable[(Hash >> 56) % kCostSlots];
        }

        FORCEINLINE uint64 EstimatedNanosPerItem(FRangeThunk Thunk)
        {
            const FCostEntry& Slot = CostSlot(Thunk);
            return Slot.Key.load(std::memory_order_relaxed) == reinterpret_cast<uint64>(Thunk)
                 ? Slot.NanosPerItem.load(std::memory_order_relaxed) : 0;
        }

        void RecordItemCost(FRangeThunk Thunk, uint32 Items, double Seconds)
        {
            if (Items == 0 || Seconds <= 0.0)
            {
                return;
            }
            const uint64 Sample = static_cast<uint64>((Seconds * 1'000'000'000.0) / Items);
            FCostEntry&  Slot   = CostSlot(Thunk);
            const uint64 Key    = reinterpret_cast<uint64>(Thunk);
            if (Slot.Key.load(std::memory_order_relaxed) != Key)
            {
                Slot.Key.store(Key, std::memory_order_relaxed);
                Slot.NanosPerItem.store(Sample, std::memory_order_relaxed);
                return;
            }
            const uint64 Prior = Slot.NanosPerItem.load(std::memory_order_relaxed);
            Slot.NanosPerItem.store(Prior == 0 ? Sample : (Prior * 3 + Sample) / 4, std::memory_order_relaxed);
        }

        void RunTimedRange(FRangeThunk Thunk, void* Context, uint32 Begin, uint32 End)
        {
            const double Start = PlatformTime::Seconds();
            Thunk(Context, Begin, End, GetThreadSlot());
            RecordItemCost(Thunk, End - Begin, PlatformTime::Seconds() - Start);
        }

        // Victims nearest first: an SMT sibling, then the same cache group, then everyone else, external slots last.
        void BuildTopologyStealOrder(uint32 NumWorkers, uint32 NumSlots, uint16 (*Order)[kMaxThreadSlots])
        {
            constexpr uint32 kMaxDescribed = 256;
            Threading::FCpuTopology Topology[kMaxDescribed];
            const uint32 NumCpus   = Threading::GetNumThreads();
            const uint32 Described = Threading::GetCpuTopology(Topology, NumCpus < kMaxDescribed ? NumCpus : kMaxDescribed);
            const auto CpuOf = [NumCpus](uint32 Worker) { return NumCpus > 1 ? (Worker + 1) % NumCpus : 0u; };

            for (uint32 Self = 0; Self < NumSlots; ++Self)
            {
                uint32 Fill = 0;
                if (Self < NumWorkers && Described != 0)
                {
                    for (uint32 Tier = 0; Tier < 3; ++Tier)
                    {
                        for (uint32 Offset = 1; Offset < NumWorkers; ++Offset)
                        {
                            const uint32 Victim = (Self + Offset) % NumWorkers;
                            const uint32 A = CpuOf(Self);
                            const uint32 B = CpuOf(Victim);
                            const bool bKnown     = A < Described && B < Described;
                            const bool bSameCore  = bKnown && Topology[A].PhysicalCore == Topology[B].PhysicalCore;
                            const bool bSameCache = bKnown && Topology[A].CacheGroup == Topology[B].CacheGroup;
                            if ((bSameCore ? 0u : (bSameCache ? 1u : 2u)) == Tier)
                            {
                                Order[Self][Fill++] = static_cast<uint16>(Victim);
                            }
                        }
                    }
                    for (uint32 External = NumWorkers; External < NumSlots; ++External)
                    {
                        Order[Self][Fill++] = static_cast<uint16>(External);
                    }
                    continue;
                }
                for (uint32 Offset = 1; Offset < NumSlots; ++Offset)
                {
                    Order[Self][Fill++] = static_cast<uint16>((Self + Offset) % NumSlots);
                }
            }
        }

#if USING(WITH_EDITOR)
        void OnSegmentBegin(uint32 Slot, bool bInjected)
        {
            if (FJobProfiler::Get().IsEnabled())
            {
                FJobProfiler::Get().JobBegin(Slot, bInjected ? "task" : "stolen", false);
            }
        }

        void OnSegmentEnd(uint32 Slot)
        {
            if (FJobProfiler::Get().IsEnabled())
            {
                FJobProfiler::Get().JobEnd(Slot);
            }
        }

        void OnIdleBegin(uint32 Slot)
        {
            if (FJobProfiler::Get().IsEnabled())
            {
                FJobProfiler::Get().IdleBegin(Slot);
            }
        }

        void OnIdleEnd(uint32 Slot)
        {
            if (FJobProfiler::Get().IsEnabled())
            {
                FJobProfiler::Get().IdleEnd(Slot);
            }
        }
#endif

        void ReportHang()
        {
            FRuntimeStats Stats;
            GetRuntimeStats(Stats);
            LOG_ERROR("Job system: {} workers, {} sleeping, {} detached task(s) live, queued roots {}/{}/{}/{}, forks {}/{}/{}/{} (H/N/L/Bg).",
                      GetNumWorkers(), Stats.Sleepers, Stats.LiveRoots,
                      Stats.QueuedRoots[0], Stats.QueuedRoots[1], Stats.QueuedRoots[2], Stats.QueuedRoots[3],
                      Stats.QueuedForks[0], Stats.QueuedForks[1], Stats.QueuedForks[2], Stats.QueuedForks[3]);
        }
    }

    namespace ParallelDetail
    {
        uint32 ComputeGrain(uint32 Num, uint32 MinRange)
        {
            const uint32 MaxChunks = Math::Min(Math::Max(GetNumThreadSlots() * 4u, 1u), TaskSystemInternal::kMaxChunks);
            const uint32 Balanced  = (Num + MaxChunks - 1) / MaxChunks;
            return Math::Max(MinRange == 0 ? 1u : MinRange, Balanced);
        }

        // Forks the right half and keeps the left, so a range costs one frame per level.
        TTask<void> Split(FRangeThunk Thunk, void* Context, uint32 Begin, uint32 End, uint32 Grain, const char* CallSite)
        {
            while (End - Begin > Grain)
            {
                const uint32 Mid = Begin + (End - Begin) / 2;
                co_await Fork(Split(Thunk, Context, Mid, End, Grain, CallSite));
                End = Mid;
            }
            {
                LUMINA_PROFILE_SECTION_NAMED(CallSite != nullptr ? CallSite : "Jobs::ParallelFor");
                TaskSystemInternal::RunTimedRange(Thunk, Context, Begin, End);
            }
            co_await Join();
        }

        void Run(uint32 Num, uint32 MinRange, EJobPriority Band, FRangeThunk Thunk, void* Context, const char* CallSite)
        {
            const uint32 Grain        = ComputeGrain(Num, MinRange);
            const uint64 NanosPerItem = TaskSystemInternal::EstimatedNanosPerItem(Thunk);
            if (Num <= Grain || (NanosPerItem != 0 && NanosPerItem * Num <= TaskSystemInternal::kSerialBudgetNanos))
            {
                TaskSystemInternal::RunTimedRange(Thunk, Context, 0, Num);
                return;
            }
            SyncWait(Band, Split, Thunk, Context, 0u, Num, Grain, CallSite);
        }

        const char* CallSiteName(const char* Signature)
        {
            static FMutex Mutex;
            static TVector<FString> Names;

            FString Signed(Signature);
            int32 Begin = Signed.Find("ParallelFor<");
            if (Begin != Constants::kIndexNone)
            {
                Begin += 12;
            }
            else if ((Begin = Signed.Find("TFunc = ")) != Constants::kIndexNone)
            {
                Begin += 8;
            }

            FString Name = Signed;
            if (Begin != Constants::kIndexNone)
            {
                for (const char* Keyword : { "class ", "struct " })
                {
                    if (Signed.Find(Keyword, Begin) == Begin)
                    {
                        Begin += FStringView(Keyword).Len();
                    }
                }

                int32 End = Constants::kIndexNone;
                for (const int32 Marker : { Signed.Find("::<lambda", Begin), Signed.Find('(', Begin), Signed.Find('>', Begin) })
                {
                    if (Marker != Constants::kIndexNone && (End == Constants::kIndexNone || Marker < End))
                    {
                        End = Marker;
                    }
                }
                if (End != Constants::kIndexNone && End > Begin)
                {
                    Name = Signed.Mid(Begin, End - Begin);
                }
            }

            for (const char* Noise : { "`anonymous namespace'::", "(anonymous namespace)::", "Lumina::" })
            {
                for (int32 At = Name.Find(Noise); At != Constants::kIndexNone; At = Name.Find(Noise))
                {
                    Name.RemoveAt(At, IntCastChecked<int32>(strlen(Noise)));
                }
            }

            FScopeLock Lock(Mutex);
            for (const FString& Existing : Names)
            {
                if (Existing == Name)
                {
                    return *Existing;
                }
            }
            if (Names.Max() == 0)
            {
                Names.Reserve(1024);
            }
            if (Names.Num() == Names.Max())
            {
                return "Jobs::ParallelFor";
            }
            Names.Add(Move(Name));
            return *Names.Last();
        }
    }

    void Initialize()
    {
        using namespace TaskSystemInternal;

        FRuntimeConfig Config;
        const FString WorkersEnv = ::Lumina::Platform::GetEnvVariable("LUMINA_JOB_WORKERS");
        if (!WorkersEnv.IsEmpty() && std::atoi(*WorkersEnv) > 0)
        {
            Config.NumWorkers = static_cast<uint32>(std::atoi(*WorkersEnv));
        }
        Config.BuildStealOrder = &BuildTopologyStealOrder;
#if USING(WITH_EDITOR)
        Config.Hooks.SegmentBegin = &OnSegmentBegin;
        Config.Hooks.SegmentEnd   = &OnSegmentEnd;
        Config.Hooks.IdleBegin    = &OnIdleBegin;
        Config.Hooks.IdleEnd      = &OnIdleEnd;
#endif
        StartRuntime(Config);
        (void)GetThreadSlot();

        static bool bReporterRegistered = false;
        if (!bReporterRegistered)
        {
            HangWatchdog::RegisterReporter(&ReportHang);
            bReporterRegistered = true;
        }
        LOG_DISPLAY("Job system online: {} workers, {} thread slots, coroutine fork-join.", GetNumWorkers(), GetNumThreadSlots());
    }

    void Shutdown()
    {
        if (!IsRuntimeRunning())
        {
            return;
        }
        WaitForSpawned();
#if USING(WITH_EDITOR)
        FJobProfiler::Get().Shutdown();
#endif
        ReleaseThreadSlot();
        StopRuntime();
    }
}
