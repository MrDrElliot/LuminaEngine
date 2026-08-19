#include "Platform/Time/PlatformTime.h"
#include "RuntimePCH.h"
#include "CPUProfiler.h"


#include "Core/Console/ConsoleVariable.h"
#include "World/World.h"
#include "World/WorldContext.h"
#include "World/WorldManager.h"

namespace Lumina
{
    static TConsoleVar<bool> CVarCPUProfilingEnabled(
        "cpu.Profiler.Enabled",
        false,
        "Enable hierarchical CPU scene profiling. Off by default; scope macros are no-ops when disabled.");

    static double CpuProfilerNowMs()
    {
        return PlatformTime::Seconds() * 1000.0;
    }

    void FCPUProfileFrame::Reset()
    {
        Scopes.clear();
        ScopeStack.clear();
        FrameNumber = 0;
        FrameStartMs = 0.0;
        TotalTimeMs = 0.0;
    }

    FCPUProfiler& FCPUProfiler::Get()
    {
        static FCPUProfiler Instance;
        return Instance;
    }

    bool FCPUProfiler::IsEnabled() const
    {
        return CVarCPUProfilingEnabled.GetValue();
    }

    FCPUProfileTarget* FCPUProfiler::ActiveTarget() const
    {
        if (TargetStack.empty())
        {
            return nullptr;
        }
        return TargetStack.back();
    }

    FCPUProfileTarget* FCPUProfiler::FindOrCreate(void* Key, const char* Name, ECPUTargetKind Kind)
    {
        for (const TUniquePtr<FCPUProfileTarget>& Target : Targets)
        {
            if (Target->Key == Key)
            {
                return Target.get();
            }
        }

        TUniquePtr<FCPUProfileTarget> NewTarget = MakeUnique<FCPUProfileTarget>();
        NewTarget->Key  = Key;
        NewTarget->Name = Name;
        NewTarget->Kind = Kind;

        FCPUProfileTarget* Raw = NewTarget.get();
        Targets.push_back(Move(NewTarget));
        return Raw;
    }

    const FCPUProfileTarget* FCPUProfiler::FindTargetByKey(void* Key) const
    {
        for (const TUniquePtr<FCPUProfileTarget>& Target : Targets)
        {
            if (Target->Key == Key)
            {
                return Target.get();
            }
        }
        return nullptr;
    }

    void FCPUProfiler::BeginFrame()
    {
        ++FrameCounter;
        TargetStack.clear();

        if (!IsEnabled())
        {
            return;
        }

        const double Now = CpuProfilerNowMs();

        // Prune targets that haven't pushed scopes recently.
        for (auto It = Targets.begin(); It != Targets.end();)
        {
            if ((*It)->LastActiveFrame + StaleTargetFrameCount < FrameCounter && FrameCounter > StaleTargetFrameCount)
            {
                It = Targets.erase(It);
            }
            else
            {
                ++It;
            }
        }

        for (const TUniquePtr<FCPUProfileTarget>& Target : Targets)
        {
            Target->Current.Reset();
            Target->Current.FrameNumber = FrameCounter;
            Target->Current.FrameStartMs = Now;
        }
    }

    void FCPUProfiler::EndFrame()
    {
        TargetStack.clear();

        if (!IsEnabled())
        {
            return;
        }

        for (const TUniquePtr<FCPUProfileTarget>& Target : Targets)
        {
            if (Target->LastActiveFrame == FrameCounter)
            {
                FinalizeFrame(*Target);
            }
        }
    }

    void FCPUProfiler::FinalizeFrame(FCPUProfileTarget& Target)
    {
        FCPUProfileFrame& Frame = Target.Current;

        // Close scopes left open by mis-paired push/pop.
        const double Now = CpuProfilerNowMs();
        while (!Frame.ScopeStack.empty())
        {
            const int32 Idx = Frame.ScopeStack.back();
            Frame.ScopeStack.pop_back();
            if (Idx >= 0 && Idx < (int32)Frame.Scopes.size())
            {
                Frame.Scopes[Idx].EndMs = Now;
            }
        }

        double Total = 0.0;
        for (const FCPUProfileScope& Scope : Frame.Scopes)
        {
            if (Scope.ParentIndex < 0)
            {
                Total += Scope.DurationMs();
            }
        }
        Frame.TotalTimeMs = Total;

        Target.Latest = Frame;
        Target.bHasResolvedFrame = true;

        Target.FrameTimeHistory.push_back((float)Total);
        if (Target.FrameTimeHistory.size() > FrameHistorySize)
        {
            Target.FrameTimeHistory.erase(Target.FrameTimeHistory.begin());
        }
    }

    void FCPUProfiler::PushTarget(void* Key, const char* Name, ECPUTargetKind Kind)
    {
        if (!IsEnabled() || Key == nullptr)
        {
            TargetStack.push_back(nullptr);
            return;
        }

        FCPUProfileTarget* Target = FindOrCreate(Key, Name, Kind);

        // Seed frame header on first push of this frame.
        if (Target->LastActiveFrame != FrameCounter)
        {
            Target->Current.Reset();
            Target->Current.FrameNumber = FrameCounter;
            Target->Current.FrameStartMs = CpuProfilerNowMs();
        }
        Target->LastActiveFrame = FrameCounter;

        TargetStack.push_back(Target);
    }

    void FCPUProfiler::PushWorldTarget(CWorld* World)
    {
        if (World == nullptr)
        {
            TargetStack.push_back(nullptr);
            return;
        }

        if (!IsEnabled())
        {
            TargetStack.push_back(nullptr);
            return;
        }

        FString NameStr = World->GetName().c_str();
        FCPUProfileTarget* Target = FindOrCreate(World, NameStr.c_str(), ECPUTargetKind::World);

        // Refresh metadata each push; context can be reassigned across PIE/duplication.
        if (GWorldManager != nullptr)
        {
            if (FWorldContext* Ctx = GWorldManager->FindContext(World))
            {
                Target->WorldType = Ctx->Type;
                Target->NetMode   = Ctx->NetMode;
                Target->bPIE      = Ctx->bPIE;
            }
        }

        if (Target->LastActiveFrame != FrameCounter)
        {
            Target->Current.Reset();
            Target->Current.FrameNumber = FrameCounter;
            Target->Current.FrameStartMs = CpuProfilerNowMs();
        }
        Target->LastActiveFrame = FrameCounter;

        TargetStack.push_back(Target);
    }

    void FCPUProfiler::PopTarget()
    {
        if (TargetStack.empty())
        {
            return;
        }
        TargetStack.pop_back();
    }

    void FCPUProfiler::BeginScope(const char* Name, const FColor& Color)
    {
        FCPUProfileTarget* Target = ActiveTarget();
        if (Target == nullptr)
        {
            return;
        }

        FCPUProfileFrame& Frame = Target->Current;

        FCPUProfileScope Scope;
        Scope.Name          = Name;
        Scope.Color         = Color;
        Scope.ParentIndex   = Frame.ScopeStack.empty() ? -1 : Frame.ScopeStack.back();
        Scope.Depth         = (int32)Frame.ScopeStack.size();
        Scope.StartMs       = CpuProfilerNowMs();

        const int32 Index = (int32)Frame.Scopes.size();
        Frame.Scopes.push_back(Scope);
        Frame.ScopeStack.push_back(Index);
    }

    void FCPUProfiler::EndScope()
    {
        FCPUProfileTarget* Target = ActiveTarget();
        if (Target == nullptr)
        {
            return;
        }

        FCPUProfileFrame& Frame = Target->Current;
        if (Frame.ScopeStack.empty())
        {
            return;
        }

        const int32 Idx = Frame.ScopeStack.back();
        Frame.ScopeStack.pop_back();

        if (Idx >= 0 && Idx < (int32)Frame.Scopes.size())
        {
            Frame.Scopes[Idx].EndMs = CpuProfilerNowMs();
        }
    }

    FCPUProfileScopeRAII::FCPUProfileScopeRAII(const char* Name, const FColor& Color)
        : bActive(false)
    {
        FCPUProfiler& Profiler = FCPUProfiler::Get();
        if (Profiler.IsEnabled())
        {
            Profiler.BeginScope(Name, Color);
            bActive = true;
        }
    }

    FCPUProfileScopeRAII::~FCPUProfileScopeRAII()
    {
        if (bActive)
        {
            FCPUProfiler::Get().EndScope();
        }
    }
}
