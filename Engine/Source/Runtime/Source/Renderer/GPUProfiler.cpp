#include "RuntimePCH.h"
#include "GPUProfiler.h"

#if defined(LUMINA_WITH_GPU_PROFILING)

#include "Core/Console/ConsoleVariable.h"

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
        FScopeLock Lock(Mutex);

        for (FSlot& Slot : Slots)
        {
            if (RHI::IsValid(Slot.Pool))
            {
                RHI::FreeH(Slot.Pool);
                Slot.Pool = {};
            }
            Slot.Scopes.clear();
            Slot.Stack.clear();
            Slot.bPendingResolve = false;
        }

        RHI::SetTimestampCollection(false);
        bInitialized = false;
        bHasResolvedFrame = false;
    }

    void FGPUProfiler::BeginFrame(uint32 SlotIndex)
    {
        const bool bEnabled = IsEnabled();

        // Arms the RHI so a marker on a disabled frame costs one relaxed load rather than a query write.
        RHI::SetTimestampCollection(bEnabled);

        if (!bEnabled)
        {
            if (bInitialized)
            {
                Shutdown();
            }
            return;
        }

        FScopeLock Lock(Mutex);

        CurrentSlot = SlotIndex % RHI::kFramesInFlight;
        FSlot& Slot = Slots[CurrentSlot];

        if (!RHI::IsValid(Slot.Pool))
        {
            Slot.Pool = RHI::CreateTimestampPool(MaxScopesPerFrame * kQueriesPerScope);
            if (!RHI::IsValid(Slot.Pool))
            {
                return;
            }
        }

        bInitialized = true;

        // Core::BeginFrame already waited this slot's timeline, so last cycle's writes are readable now.
        if (Slot.bPendingResolve)
        {
            Resolve(Slot);
            Slot.bPendingResolve = false;
        }

        Slot.Scopes.clear();
        Slot.Stack.clear();
        Slot.QueryCursor = 0;
        Slot.FrameNumber = ++FrameCounter;
    }

    void FGPUProfiler::BeginScope(RHI::FCmdListH CL, const char* Name)
    {
        if (!bInitialized || Name == nullptr)
        {
            return;
        }

        FScopeLock Lock(Mutex);

        FSlot& Slot = Slots[CurrentSlot];
        if (!RHI::IsValid(Slot.Pool) || Slot.Scopes.size() >= MaxScopesPerFrame)
        {
            // A depth entry is still pushed so the matching EndScope stays balanced.
            Slot.Stack.push_back(INDEX_NONE);
            return;
        }

        FPendingScope& Scope = Slot.Scopes.emplace_back();
        Scope.Name        = Name;
        Scope.ParentIndex = Slot.Stack.empty() ? INDEX_NONE : Slot.Stack.back();
        Scope.Depth       = (int32)Slot.Stack.size();
        Scope.BeginQuery  = Slot.QueryCursor;
        Scope.EndQuery    = Slot.QueryCursor + 1;
        Slot.QueryCursor += kQueriesPerScope;

        // Reset immediately before the write, so ordering inside this list is the only guarantee needed.
        RHI::CmdResetTimestamps(CL, Slot.Pool, Scope.BeginQuery, kQueriesPerScope);
        RHI::CmdWriteTimestamp(CL, Slot.Pool, Scope.BeginQuery, RHI::EStageFlags::AllCommands);

        Slot.Stack.push_back((int32)Slot.Scopes.size() - 1);
    }

    void FGPUProfiler::EndScope(RHI::FCmdListH CL)
    {
        if (!bInitialized)
        {
            return;
        }

        FScopeLock Lock(Mutex);

        FSlot& Slot = Slots[CurrentSlot];
        if (Slot.Stack.empty())
        {
            return;
        }

        const int32 Index = Slot.Stack.back();
        Slot.Stack.pop_back();

        if (Index == INDEX_NONE || !RHI::IsValid(Slot.Pool))
        {
            return;
        }

        RHI::CmdWriteTimestamp(CL, Slot.Pool, Slot.Scopes[Index].EndQuery, RHI::EStageFlags::AllCommands);
        Slot.bPendingResolve = true;
    }

    void FGPUProfiler::Resolve(FSlot& Slot)
    {
        if (Slot.Scopes.empty() || Slot.QueryCursor == 0)
        {
            return;
        }

        TVector<uint64> Ticks;
        Ticks.resize(Slot.QueryCursor);

        // A frame whose markers never reached the GPU is dropped rather than reported as zero-length work.
        if (!RHI::ReadTimestamps(Slot.Pool, 0, Slot.QueryCursor, TSpan<uint64>(Ticks.data(), Ticks.size())))
        {
            return;
        }

        const double PeriodNs = RHI::GetTimestampPeriodNs();
        if (PeriodNs <= 0.0)
        {
            return;
        }

        uint64 Origin = Ticks[0];
        for (const FPendingScope& Pending : Slot.Scopes)
        {
            Origin = Math::Min(Origin, Ticks[Pending.BeginQuery]);
        }

        const double ToMs = PeriodNs / 1000000.0;

        Latest.Scopes.clear();
        Latest.Scopes.reserve(Slot.Scopes.size());
        Latest.FrameNumber = Slot.FrameNumber;

        double FrameEndMs = 0.0;
        for (const FPendingScope& Pending : Slot.Scopes)
        {
            FGPUProfileScope& Out = Latest.Scopes.emplace_back();
            Out.Name        = Pending.Name;
            Out.ParentIndex = Pending.ParentIndex;
            Out.Depth       = Pending.Depth;
            Out.StartMs     = (double)(Ticks[Pending.BeginQuery] - Origin) * ToMs;
            Out.EndMs       = (double)(Ticks[Pending.EndQuery] - Origin) * ToMs;
            FrameEndMs      = Math::Max(FrameEndMs, Out.EndMs);
        }

        Latest.TotalMs = FrameEndMs;
        bHasResolvedFrame = true;

        FrameTimeHistory.push_back((float)Latest.TotalMs);
        if (FrameTimeHistory.size() > FrameHistorySize)
        {
            FrameTimeHistory.erase(FrameTimeHistory.begin());
        }
    }
}

#endif
