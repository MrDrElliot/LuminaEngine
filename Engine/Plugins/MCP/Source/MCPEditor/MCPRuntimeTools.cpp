#include "MCPRuntimeTools.h"

#include "Agent/AgentToolRegistry.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Engine/Engine.h"
#include "Core/Engine/EngineURL.h"
#include "Core/Engine/FrameStats.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectArray.h"
#include "Memory/MemoryTracking.h"
#include "Memory/Memory.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Containers/Algorithm.h"
#include "Platform/Process/PlatformProcess.h"
#include "Renderer/RHI.h"
#include "World/ECS/Registry.h"
#include "World/World.h"
#include "World/WorldManager.h"

namespace Lumina::MCP
{
    namespace
    {
        constexpr uint64 BytesPerMegabyte = 1024ull * 1024ull;

        const char* WorldTypeName(EWorldType Type)
        {
            switch (Type)
            {
            case EWorldType::Game:       return "Game";
            case EWorldType::Simulation: return "Simulation";
            case EWorldType::Editor:     return "Editor";
            default:                     return "None";
            }
        }

        void FillStats(const SEngineStatsParams& In, SEngineStats& Out)
        {
            const FrameStats::FSummary Summary = FrameStats::Summarize((uint32)Math::Clamp(In.Worst, 0, 100));
            Out.Frames = (int32)Summary.Frames;
            Out.WindowSeconds = (float)Summary.WindowSeconds;
            Out.MeanMs = Summary.MeanMs;
            Out.P50Ms = Summary.P50Ms;
            Out.P95Ms = Summary.P95Ms;
            Out.P99Ms = Summary.P99Ms;
            Out.MaxMs = Summary.MaxMs;
            Out.Over33Ms = (int32)Summary.Over33Ms;
            Out.Over50Ms = (int32)Summary.Over50Ms;
            Out.Over100Ms = (int32)Summary.Over100Ms;
            Out.Over250Ms = (int32)Summary.Over250Ms;
            for (const FrameStats::FHitch& Hitch : Summary.Worst)
            {
                SEngineFrame& Frame = Out.Worst.emplace_back();
                Frame.AtSeconds = Hitch.TimeSeconds;
                Frame.FrameMs = Hitch.FrameMs;
            }

            Out.ProcessMemoryMB = (int32)Platform::GetProcessMemoryUsageMegaBytes();

            RHI::FGPUMemoryStats Gpu;
            RHI::GetGPUMemoryStats(Gpu);
            Out.GpuUsageMB = (int32)(Gpu.TotalUsage / BytesPerMegabyte);
            Out.GpuBudgetMB = (int32)(Gpu.TotalBudget / BytesPerMegabyte);
            Out.GpuAllocations = (int32)Gpu.TotalAllocations;

            Out.TrackedNativeMB = (int32)(Memory::GetTrackedLiveBytes() / BytesPerMegabyte);
            DotNet::FScriptDiagnostics Managed;
            if (DotNet::GetRuntimeDiagnostics(Managed, false))
            {
                Out.ManagedHeapMB = (int32)(Managed.ManagedHeapBytes / (int64)BytesPerMegabyte);
            }

            Out.LiveObjects = GObjectArray.GetNumAliveObjects();
            Out.ManagedBindings = (int32)GetLiveManagedBindingCount();

            if (GWorldManager != nullptr)
            {
                GWorldManager->ForEachWorld([&Out](CWorld& World)
                {
                    SEngineWorldStats& Stats = Out.Worlds.emplace_back();
                    Stats.Name = World.GetName().ToString();
                    Stats.Type = WorldTypeName(World.GetWorldType());
                    Stats.Entities = (int32)ECS::GetWorldRegistry(World).NumEntities();
                });
            }

            if (In.bResetFrames)
            {
                FrameStats::Reset();
            }
        }
    }

    namespace
    {
        float BytesToMB(uint64 Bytes) { return (float)((double)Bytes / (double)BytesPerMegabyte); }

        // The tracker and allocator wrappers sit at the top of every stack, and say nothing about who allocated.
        bool IsAllocatorFrame(const FString& Frame)
        {
            return Frame.find("MemoryTracking") != FString::npos || Frame.find("Memory::Malloc") != FString::npos
                || Frame.find("Memory::Realloc") != FString::npos || Frame.find("rpmalloc") != FString::npos
                || Frame.find("operator new") != FString::npos || Frame.find("Memory::New") != FString::npos;
        }

        void FillMemoryReport(const SMemoryReportParams& In, SMemoryReport& Out)
        {
            if (In.Callstacks == "On" || In.Callstacks == "Off")
            {
                Memory::SetCaptureCallstacks(In.Callstacks == "On");
            }
            Out.bCallstacks = Memory::IsCapturingCallstacks();

            Out.ProcessMB = (int32)Platform::GetProcessMemoryUsageMegaBytes();
            Out.TrackedNativeMB = BytesToMB(Memory::GetTrackedLiveBytes());
            Out.TrackedAllocations = (int32)Memory::GetTrackedLiveCount();
            Out.TrackingOverflows = (int32)Memory::GetTrackingOverflowCount();
            if (!In.StatsFile.empty())
            {
                Memory::DumpAllocatorStatistics(In.StatsFile.c_str());
            }

            DotNet::FScriptDiagnostics Managed;
            if (DotNet::GetRuntimeDiagnostics(Managed, In.bCollectManaged))
            {
                Out.ManagedHeapMB = BytesToMB((uint64)Managed.ManagedHeapBytes);
                Out.ManagedCommittedMB = BytesToMB((uint64)Managed.CommittedBytes);
                Out.ScriptContexts = Managed.AliveScriptAlcCount;
            }

            Platform::FAddressSpaceStats Space;
            Platform::GetAddressSpaceStats(Space, In.bIncludeHeaps);
            const uint64 Rpmalloc = Memory::GetCommittedMemory();
            const uint64 Heap = Space.bHeapWalkValid ? Space.HeapCommitted : 0;
            const uint64 ManagedCommit = (uint64)Math::Max<int64>(Managed.CommittedBytes, 0);
            Out.PrivateCommittedMB = BytesToMB(Space.PrivateCommitted);
            Out.RpmallocMappedMB = BytesToMB(Memory::GetCurrentMappedMemory());
            Out.RpmallocCommittedMB = BytesToMB(Rpmalloc);
            Out.bHugePages = Memory::UsesHugePages();
            Out.RpmallocCachedMB = BytesToMB(Memory::GetCachedMemory());
            Out.HeapCommittedMB = BytesToMB(Heap);
            Out.ReservedMB = BytesToMB(Space.Reserved);
            const uint64 Explained = Rpmalloc + Heap + ManagedCommit;
            Out.UnattributedMB = BytesToMB(Space.PrivateCommitted > Explained ? Space.PrivateCommitted - Explained : 0);

            const int32 Top = Math::Clamp(In.Top, 1, 64);
            constexpr uint32 MaxCategories = 128;
            TVector<Memory::FMemoryCategoryStats> Categories(MaxCategories);
            Categories.resize(Memory::GetCategoryStats(Categories.data(), MaxCategories));
            Algo::Sort(Categories, [](const Memory::FMemoryCategoryStats& A, const Memory::FMemoryCategoryStats& B) { return A.LiveBytes > B.LiveBytes; });
            for (const Memory::FMemoryCategoryStats& Stats : Categories)
            {
                if ((int32)Out.Categories.size() >= Top || Stats.LiveBytes == 0)
                {
                    break;
                }
                SMemoryCategory& Category = Out.Categories.emplace_back();
                Category.Name = Stats.Name;
                Category.LiveMB = BytesToMB(Stats.LiveBytes);
                Category.LiveCount = (int32)Stats.LiveCount;
                Category.PeakMB = BytesToMB(Stats.PeakBytes);
            }

            if (Out.bCallstacks)
            {
                TVector<Memory::FCallSiteStat> Sites((size_t)Top);
                Sites.resize(Memory::GetTopCallSites(Sites.data(), (uint32)Top, Memory::ECallSiteSort::LiveBytes,
                    In.Category.empty() ? nullptr : In.Category.c_str()));
                for (const Memory::FCallSiteStat& Site : Sites)
                {
                    SMemoryCallSite& Entry = Out.CallSites.emplace_back();
                    Entry.Category = Site.CatName;
                    Entry.LiveKB = (float)((double)Site.LiveBytes / 1024.0);
                    Entry.LiveCount = (int32)Site.LiveCount;
                    for (uint32 Frame = 0; Frame < Site.FrameCount && Entry.Frames.size() < 8; ++Frame)
                    {
                        char Text[512];
                        Memory::ResolveSymbol(Site.Frames[Frame], Text, sizeof(Text));
                        FString Resolved(Text);
                        if (!IsAllocatorFrame(Resolved))
                        {
                            Entry.Frames.push_back(Move(Resolved));
                        }
                    }
                }
            }

            if (In.bReset)
            {
                Memory::ResetTracking();
            }
        }
    }

    void RegisterRuntimeTools(FStringView Owner)
    {
        Agent::FToolRegistry& Registry = Agent::FToolRegistry::Get();

        Registry.Register<SEngineStatsParams, SEngineStats>(
            Owner, "engine.stats",
            "Report frame times since the last reset (mean, percentiles, how many ran over 33, 50, 100 and 250 ms, and the "
            "longest), with process and GPU memory, live objects, managed delegate bindings and the entities in every world. "
            "Recording is always on, so a long session can be checked without a profiler attached.",
            Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
            [](const SEngineStatsParams& In, SEngineStats& Out)
            {
                FillStats(In, Out);
                return Agent::FToolResult::Ok(Lumina::Format(
                    "{} frames over {:.1f} s, mean {:.2f} ms, p99 {:.2f} ms, max {:.2f} ms, {} over 50 ms. {} MB process, {} MB GPU.",
                    Out.Frames, Out.WindowSeconds, Out.MeanMs, Out.P99Ms, Out.MaxMs, Out.Over50Ms, Out.ProcessMemoryMB, Out.GpuUsageMB));
            });

        Registry.Register<SMemoryReportParams, SMemoryReport>(
            Owner, "memory.report",
            "Report where memory lives: the process, the engine allocator ledger by category, the managed heap after a "
            "collection, and, with Callstacks set On, the call sites holding the most live bytes. Take one report, "
            "repeat the suspect action a few times, and compare to find what keeps growing.",
            Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
            [](const SMemoryReportParams& In, SMemoryReport& Out)
            {
                FillMemoryReport(In, Out);
                return Agent::FToolResult::Ok(Lumina::Format(
                    "{} MB process, {:.1f} MB tracked native in {} allocations, {:.1f} MB managed heap, {} script contexts.",
                    Out.ProcessMB, Out.TrackedNativeMB, Out.TrackedAllocations, Out.ManagedHeapMB, Out.ScriptContexts));
            });

        Registry.Register<SOpenLevelParams, SOpenLevelResult>(
            Owner, "world.open_level",
            "Travel the running game to another world, as gameplay does through Game.OpenLevel. The swap happens at the "
            "start of the next frame; poll editor.play_state or engine.stats to see it land.",
            Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
            [](const SOpenLevelParams& In, SOpenLevelResult& Out)
            {
                if (GEngine == nullptr || In.Url.empty())
                {
                    return Agent::FToolResult::Error("Pass the Url of a world to open.");
                }

                const FURL Url = FURL::Parse(FStringView(In.Url.c_str(), In.Url.size()));
                Out.Map = Url.Map;
                GEngine->OpenLevel(Url);
                return Agent::FToolResult::Ok(Lumina::Format("Opening {}.", Url.Map));
            });
    }
}
