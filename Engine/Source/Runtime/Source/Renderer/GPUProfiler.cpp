#include "RuntimePCH.h"
#include "GPUProfiler.h"

#if defined(LUMINA_WITH_GPU_PROFILING)

#include "Core/Console/ConsoleVariable.h"
#include "RHICore.h"

namespace Lumina
{
    namespace
    {
        // The console and the tool checkbox drive one flag, so neither can disagree with the other.
        bool GGPUProfilerEnabled = false;

        void OnGPUProfilerCVarChanged(const CVarValueType& Value)
        {
            GGPUProfilerEnabled = Containers::Get<int32>(Value) != 0;
        }

        TConsoleVar<int32> CVarGPUProfilerEnabled(
            "r.GPUProfiler.Enabled",
            0,
            "Collect per-pass GPU timings from the scene's debug markers and show them in the Profiler tool.",
            &OnGPUProfilerCVarChanged);

        // Two timestamps per scope, so the pool has to hold both ends of every marker.
        constexpr uint32 kQueriesPerScope = 2;

        constexpr uint32 BeginQuery(uint32 ScopeIndex) { return ScopeIndex * kQueriesPerScope; }
        constexpr uint32 EndQuery(uint32 ScopeIndex)   { return ScopeIndex * kQueriesPerScope + 1; }

        // A list of an aliased queue runs on the queue it aliases, so its scopes belong in that lane.
        RHI::EQueueType HardwareQueueOf(RHI::FCmdListH CL)
        {
            const RHI::EQueueType Queue = RHI::GetCommandListQueue(CL);
            if ((Queue == RHI::EQueueType::Compute && !RHI::SupportsAsyncCompute())
                || (Queue == RHI::EQueueType::Transfer && !RHI::SupportsAsyncTransfer()))
            {
                return RHI::EQueueType::Graphics;
            }
            return Queue;
        }

        struct FInterval
        {
            uint64 Begin;
            uint64 End;
        };

        // Expects intervals sorted by Begin, and merges them in place.
        void MergeIntervals(TVector<FInterval>& Intervals)
        {
            int32 Write = 0;
            for (int32 Read = 0; Read < Intervals.Num(); ++Read)
            {
                if (Write > 0 && Intervals[Read].Begin <= Intervals[Write - 1].End)
                {
                    Intervals[Write - 1].End = Math::Max(Intervals[Write - 1].End, Intervals[Read].End);
                }
                else
                {
                    Intervals[Write++] = Intervals[Read];
                }
            }
            Intervals.SetNum(Write);
        }

        uint64 TotalLength(const TVector<FInterval>& Merged)
        {
            uint64 Sum = 0;
            for (const FInterval& Interval : Merged)
            {
                Sum += Interval.End - Interval.Begin;
            }
            return Sum;
        }

        uint64 IntersectionLength(const TVector<FInterval>& A, const TVector<FInterval>& B)
        {
            uint64 Sum = 0;
            int32 i = 0;
            int32 j = 0;
            while (i < A.Num() && j < B.Num())
            {
                const uint64 Begin = Math::Max(A[i].Begin, B[j].Begin);
                const uint64 End   = Math::Min(A[i].End, B[j].End);
                if (End > Begin)
                {
                    Sum += End - Begin;
                }
                (A[i].End < B[j].End) ? ++i : ++j;
            }
            return Sum;
        }
    }

    const char* GPUProfileQueueName(RHI::EQueueType Queue)
    {
        switch (Queue)
        {
        case RHI::EQueueType::Graphics: return "Graphics";
        case RHI::EQueueType::Compute:  return "Async compute";
        case RHI::EQueueType::Transfer: return "Transfer";
        }
        return "Unknown";
    }

    FGPUProfiler& FGPUProfiler::Get()
    {
        static FGPUProfiler Instance;
        return Instance;
    }

    bool FGPUProfiler::IsEnabled() const
    {
        return GGPUProfilerEnabled && RHI::SupportsTimestamps();
    }

    void FGPUProfiler::SetEnabled(bool bEnabled)
    {
        GGPUProfilerEnabled = bEnabled;
    }

    void FGPUProfiler::Shutdown()
    {
        bInitialized.store(false, std::memory_order_release);

        for (FSlot& Slot : Slots)
        {
            if (RHI::IsValid(Slot.Pool))
            {
                // Disabling the cvar calls this mid-frame, with earlier frames still naming the pool.
                RHI::Retire(Slot.Pool);
                Slot.Pool = {};
            }
            Slot.NumClaimed.store(0, std::memory_order_relaxed);
        }

        RHI::SetTimestampCollection(false);
        bHasResolvedFrame = false;
    }

    void FGPUProfiler::BeginFrame(uint32 SlotIndex)
    {
        const bool bEnabled = IsEnabled();

        // Arms the RHI so a marker on a disabled frame costs one relaxed load rather than a query write.
        RHI::SetTimestampCollection(bEnabled);

        if (!bEnabled)
        {
            if (bInitialized.load(std::memory_order_relaxed))
            {
                Shutdown();
            }
            return;
        }

        const uint32 Index = SlotIndex % RHI::kFramesInFlight;
        FSlot& Slot = Slots[Index];

        if (!RHI::IsValid(Slot.Pool))
        {
            Slot.Pool = RHI::CreateTimestampPool(MaxScopesPerFrame * kQueriesPerScope);
            if (!RHI::IsValid(Slot.Pool))
            {
                return;
            }
            Slot.Scopes.SetNum(MaxScopesPerFrame);
            Slot.NumClaimed.store(0, std::memory_order_relaxed);
        }

        // BeginFrame already waited this slot's timeline on every queue, so last cycle's writes are readable now.
        if (Slot.NumClaimed.load(std::memory_order_relaxed) != 0)
        {
            Resolve(Slot);
        }

        Slot.NumClaimed.store(0, std::memory_order_relaxed);
        Slot.FrameNumber = ++FrameCounter;

        // After the resolve read last cycle's values, and outside any render pass, which a scope may open inside.
        Slot.bHostReset = RHI::ResetTimestamps(Slot.Pool, 0, MaxScopesPerFrame * kQueriesPerScope);

        CurrentSlot.store(Index, std::memory_order_release);
        bInitialized.store(true, std::memory_order_release);
    }

    int32 FGPUProfiler::BeginScope(RHI::FCmdListH CL, const char* Name, int32 ParentIndex, int32 Depth)
    {
        if (!bInitialized.load(std::memory_order_acquire) || Name == nullptr)
        {
            return Constants::kIndexNone;
        }

        FSlot& Slot = Slots[CurrentSlot.load(std::memory_order_acquire)];
        const uint32 Index = Slot.NumClaimed.fetch_add(1, std::memory_order_relaxed);
        if (Index >= MaxScopesPerFrame)
        {
            return Constants::kIndexNone;
        }

        FPendingScope& Scope = Slot.Scopes[Index];
        Scope.Name        = Name;
        Scope.ParentIndex = ParentIndex;
        Scope.Depth       = Depth;
        Scope.Queue       = HardwareQueueOf(CL);

        // Without hostQueryReset, reset immediately before the write, so ordering inside this list is the only guarantee needed.
        if (!Slot.bHostReset)
        {
            RHI::CmdResetTimestamps(CL, Slot.Pool, BeginQuery(Index), kQueriesPerScope);
        }
        RHI::CmdWriteTimestamp(CL, Slot.Pool, BeginQuery(Index), RHI::EStageFlags::AllCommands);

        return (int32)Index;
    }

    void FGPUProfiler::EndScope(RHI::FCmdListH CL, int32 ScopeIndex)
    {
        if (ScopeIndex == Constants::kIndexNone || !bInitialized.load(std::memory_order_acquire))
        {
            return;
        }

        const FSlot& Slot = Slots[CurrentSlot.load(std::memory_order_acquire)];
        RHI::CmdWriteTimestamp(CL, Slot.Pool, EndQuery((uint32)ScopeIndex), RHI::EStageFlags::AllCommands);
    }

    void FGPUProfiler::Resolve(FSlot& Slot)
    {
        const uint32 Claimed   = Slot.NumClaimed.load(std::memory_order_relaxed);
        const uint32 NumScopes = Math::Min(Claimed, MaxScopesPerFrame);
        const double PeriodNs  = RHI::GetTimestampPeriodNs();
        if (NumScopes == 0 || PeriodNs <= 0.0)
        {
            return;
        }

        TVector<uint64> Ticks;
        Ticks.SetNum(NumScopes * kQueriesPerScope);

        // A list that was discarded or a queue that cannot timestamp leaves its queries unavailable, which drops only those scopes.
        RHI::ReadTimestamps(Slot.Pool, 0, NumScopes * kQueriesPerScope, TSpan<uint64>(Ticks.GetData(), Ticks.Num()));

        // A child is claimed after its parent on the same thread, so one forward pass sees every parent first.
        TVector<bool> bValid;
        bValid.SetNum(NumScopes);
        uint64 Origin = UINT64_MAX;
        for (uint32 i = 0; i < NumScopes; ++i)
        {
            const FPendingScope& Pending = Slot.Scopes[i];
            const uint64 Begin = Ticks[BeginQuery(i)];
            const uint64 End   = Ticks[EndQuery(i)];
            const bool bParentValid = Pending.ParentIndex == Constants::kIndexNone
                || ((uint32)Pending.ParentIndex < i && bValid[Pending.ParentIndex]);

            bValid[i] = bParentValid && Begin != RHI::kTimestampUnavailable && End != RHI::kTimestampUnavailable && End >= Begin;
            if (bValid[i])
            {
                Origin = Math::Min(Origin, Begin);
            }
        }

        TVector<uint32> Order;
        Order.Reserve(NumScopes);
        for (uint32 i = 0; i < NumScopes; ++i)
        {
            if (bValid[i])
            {
                Order.Add(i);
            }
        }

        if (Order.IsEmpty())
        {
            return;
        }

        Algo::Sort(Order, [&](uint32 A, uint32 B)
        {
            const uint32 QueueA = (uint32)Slot.Scopes[A].Queue;
            const uint32 QueueB = (uint32)Slot.Scopes[B].Queue;
            if (QueueA != QueueB)
            {
                return QueueA < QueueB;
            }
            const uint64 BeginA = Ticks[BeginQuery(A)];
            const uint64 BeginB = Ticks[BeginQuery(B)];
            return BeginA != BeginB ? BeginA < BeginB : A < B;
        });

        TVector<int32> NewIndex;
        NewIndex.SetNum(NumScopes);
        for (int32 i = 0; i < Order.Num(); ++i)
        {
            NewIndex[Order[i]] = i;
        }

        const double ToMs = PeriodNs / 1000000.0;

        Latest.Scopes.Reset();
        Latest.Scopes.Reserve(Order.Num());
        Latest.FrameNumber   = Slot.FrameNumber;
        Latest.DroppedScopes = Claimed - (uint32)Order.Num();

        TVector<FInterval> RootIntervals[kGPUProfileQueueCount];
        uint64 FrameEnd = Origin;
        for (uint32 Old : Order)
        {
            const FPendingScope& Pending = Slot.Scopes[Old];
            const uint64 Begin = Ticks[BeginQuery(Old)];
            const uint64 End   = Ticks[EndQuery(Old)];

            FGPUProfileScope& Out = Latest.Scopes.EmplaceGetRef();
            Out.Name        = Pending.Name;
            Out.ParentIndex = Pending.ParentIndex == Constants::kIndexNone ? Constants::kIndexNone : NewIndex[Pending.ParentIndex];
            Out.Depth       = Pending.Depth;
            Out.Queue       = Pending.Queue;
            Out.StartMs     = (double)(Begin - Origin) * ToMs;
            Out.EndMs       = (double)(End - Origin) * ToMs;

            FrameEnd = Math::Max(FrameEnd, End);
            if (Pending.ParentIndex == Constants::kIndexNone)
            {
                RootIntervals[(uint32)Pending.Queue].Add(FInterval{ Begin, End });
            }
        }

        for (uint32 Queue = 0; Queue < kGPUProfileQueueCount; ++Queue)
        {
            MergeIntervals(RootIntervals[Queue]);
            Latest.QueueBusyMs[Queue] = (double)TotalLength(RootIntervals[Queue]) * ToMs;
        }
        Latest.AsyncOverlapMs = (double)IntersectionLength(RootIntervals[(uint32)RHI::EQueueType::Graphics],
                                                            RootIntervals[(uint32)RHI::EQueueType::Compute]) * ToMs;
        Latest.TotalMs = (double)(FrameEnd - Origin) * ToMs;
        bHasResolvedFrame = true;

        FrameTimeHistory.Add((float)Latest.TotalMs);
        if (static_cast<uint32>(FrameTimeHistory.Num()) > FrameHistorySize)
        {
            FrameTimeHistory.RemoveAt(0);
        }
    }
}

#endif
