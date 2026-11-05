#include "MCPRuntimeTools.h"

#include "Agent/AgentObjectEdit.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "Config/Config.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Engine/FrameStats.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectArray.h"
#include "Memory/MemoryTracking.h"
#include "Memory/Memory.h"
#include "Scripting/DotNet/DotNetHost.h"
#include "Containers/Algorithm.h"
#include "Platform/Process/PlatformProcess.h"
#include "Renderer/GPUProfiler.h"
#include "Renderer/RHI.h"
#include "World/ECS/Registry.h"
#include "World/World.h"
#include "World/WorldManager.h"

namespace Lumina::MCP
{
    namespace MCPRuntimeToolsInternal
    {
        const char* EngineWorldTypeName(EWorldType Type)
        {
            switch (Type)
            {
            case EWorldType::Game:       return "Game";
            case EWorldType::Simulation: return "Simulation";
            case EWorldType::Editor:     return "Editor";
            default:                     return "None";
            }
        }

        void FillEngineStats(const SEngineStatsParams& In, SEngineStats& Out)
        {
            const FrameStats::FSummary Summary = FrameStats::Summarize(Math::Clamp(In.Worst, 0, 100));
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
                SEngineFrame& Frame = Out.Worst.EmplaceGetRef();
                Frame.AtSeconds = Hitch.TimeSeconds;
                Frame.FrameMs = Hitch.FrameMs;
            }

            Out.ProcessMemoryMB = (int32)Platform::GetProcessMemoryUsageMegaBytes();

            RHI::FGPUMemoryStats Gpu;
            RHI::GetGPUMemoryStats(Gpu);
            Out.GpuUsageMB = (int32)(Gpu.TotalUsage / Constants::kMiB);
            Out.GpuBudgetMB = (int32)(Gpu.TotalBudget / Constants::kMiB);
            Out.GpuAllocations = (int32)Gpu.TotalAllocations;

            Out.TrackedNativeMB = (int32)(Memory::GetTrackedLiveBytes() / Constants::kMiB);
            DotNet::FScriptDiagnostics Managed;
            if (DotNet::GetRuntimeDiagnostics(Managed, false))
            {
                Out.ManagedHeapMB = (int32)(Managed.ManagedHeapBytes / (int64)Constants::kMiB);
            }

            Out.LiveObjects = Object::Internal::GetNumAliveObjects();
            Out.ManagedBindings = (int32)GetLiveManagedBindingCount();

            if (GWorldManager != nullptr)
            {
                GWorldManager->ForEachWorld([&Out](CWorld& World)
                {
                    SEngineWorldStats& Stats = Out.Worlds.EmplaceGetRef();
                    Stats.Name = World.GetName().ToString();
                    Stats.Type = EngineWorldTypeName(World.GetWorldType());
                    Stats.Entities = World.GetEntityRegistry().NumEntities();
                });
            }

            if (In.bResetFrames)
            {
                FrameStats::Reset();
            }
        }

        float EngineBytesToMB(uint64 Bytes) { return (float)((double)Bytes / (double)Constants::kMiB); }

        // The tracker and allocator wrappers sit at the top of every stack, and say nothing about who allocated.
        bool IsEngineAllocatorFrame(const FString& Frame)
        {
            return Frame.Find("MemoryTracking") != Constants::kIndexNone || Frame.Find("Memory::Malloc") != Constants::kIndexNone
                || Frame.Find("Memory::Realloc") != Constants::kIndexNone || Frame.Find("rpmalloc") != Constants::kIndexNone
                || Frame.Find("operator new") != Constants::kIndexNone || Frame.Find("Memory::New") != Constants::kIndexNone;
        }

        void FillEngineMemoryReport(const SMemoryReportParams& In, SMemoryReport& Out)
        {
            if (In.Callstacks == "On" || In.Callstacks == "Off")
            {
                Memory::SetCaptureCallstacks(In.Callstacks == "On");
            }
            Out.bCallstacks = Memory::IsCapturingCallstacks();

            Out.ProcessMB = (int32)Platform::GetProcessMemoryUsageMegaBytes();
            Out.TrackedNativeMB = EngineBytesToMB(Memory::GetTrackedLiveBytes());
            Out.TrackedAllocations = (int32)Memory::GetTrackedLiveCount();
            Out.TrackingOverflows = (int32)Memory::GetTrackingOverflowCount();
            if (!In.StatsFile.IsEmpty())
            {
                Memory::DumpAllocatorStatistics(*In.StatsFile);
            }

            DotNet::FScriptDiagnostics Managed;
            if (DotNet::GetRuntimeDiagnostics(Managed, In.bCollectManaged))
            {
                Out.ManagedHeapMB = EngineBytesToMB((uint64)Managed.ManagedHeapBytes);
                Out.ManagedCommittedMB = EngineBytesToMB((uint64)Managed.CommittedBytes);
                Out.ScriptContexts = Managed.AliveScriptAlcCount;
            }

            Platform::FAddressSpaceStats Space;
            Platform::GetAddressSpaceStats(Space, In.bIncludeHeaps);
            const uint64 Rpmalloc = Memory::GetCommittedMemory();
            const uint64 Heap = Space.bHeapWalkValid ? Space.HeapCommitted : 0;
            const uint64 ManagedCommit = (uint64)Math::Max<int64>(Managed.CommittedBytes, 0);
            Out.PrivateCommittedMB = EngineBytesToMB(Space.PrivateCommitted);
            Out.RpmallocMappedMB = EngineBytesToMB(Memory::GetCurrentMappedMemory());
            Out.RpmallocCommittedMB = EngineBytesToMB(Rpmalloc);
            Out.bHugePages = Memory::UsesHugePages();
            Out.RpmallocCachedMB = EngineBytesToMB(Memory::GetCachedMemory());
            Out.HeapCommittedMB = EngineBytesToMB(Heap);
            Out.ReservedMB = EngineBytesToMB(Space.Reserved);
            const uint64 Explained = Rpmalloc + Heap + ManagedCommit;
            Out.UnattributedMB = EngineBytesToMB(Space.PrivateCommitted > Explained ? Space.PrivateCommitted - Explained : 0);

            const int32 Top = Math::Clamp(In.Top, 1, 64);
            constexpr uint32 MaxCategories = 128;
            TVector<Memory::FMemoryCategoryStats> Categories(MaxCategories);
            Categories.SetNum(Memory::GetCategoryStats(Categories.GetData(), MaxCategories));
            Algo::Sort(Categories, [](const Memory::FMemoryCategoryStats& A, const Memory::FMemoryCategoryStats& B) { return A.LiveBytes > B.LiveBytes; });
            for (const Memory::FMemoryCategoryStats& Stats : Categories)
            {
                if (Out.Categories.Num() >= Top || Stats.LiveBytes == 0)
                {
                    break;
                }
                SMemoryCategory& Category = Out.Categories.EmplaceGetRef();
                Category.Name = Stats.Name;
                Category.LiveMB = EngineBytesToMB(Stats.LiveBytes);
                Category.LiveCount = (int32)Stats.LiveCount;
                Category.PeakMB = EngineBytesToMB(Stats.PeakBytes);
            }

            if (Out.bCallstacks)
            {
                TVector<Memory::FCallSiteStat> Sites((size_t)Top);
                Sites.SetNum(Memory::GetTopCallSites(Sites.GetData(), (uint32)Top, Memory::ECallSiteSort::LiveBytes,
                    In.Category.IsEmpty() ? nullptr : *In.Category));
                for (const Memory::FCallSiteStat& Site : Sites)
                {
                    SMemoryCallSite& Entry = Out.CallSites.EmplaceGetRef();
                    Entry.Category = Site.CatName;
                    Entry.LiveKB = (float)((double)Site.LiveBytes / 1024.0);
                    Entry.LiveCount = (int32)Site.LiveCount;
                    for (uint32 Frame = 0; Frame < Site.FrameCount && Entry.Frames.Num() < 8; ++Frame)
                    {
                        char Text[512];
                        Memory::ResolveSymbol(Site.Frames[Frame], Text, sizeof(Text));
                        FString Resolved(Text);
                        if (!IsEngineAllocatorFrame(Resolved))
                        {
                            Entry.Frames.Add(Move(Resolved));
                        }
                    }
                }
            }

            RHI::FGPUMemoryStats GpuStats;
            RHI::GetGPUMemoryStats(GpuStats);
            for (const RHI::FGPUMemoryHeapStats& Heap : GpuStats.Heaps)
            {
                SMemoryGpuHeap& Row = Out.GpuHeaps.EmplaceGetRef();
                Row.Index = (int32)Heap.HeapIndex;
                Row.bDeviceLocal = Heap.bDeviceLocal;
                Row.bHostVisible = Heap.bHostVisible;
                Row.UsageMB = EngineBytesToMB(Heap.UsageBytes);
                Row.BlockMB = EngineBytesToMB(Heap.BlockBytes);
                Row.BudgetMB = EngineBytesToMB(Heap.BudgetBytes);
            }

            TVector<RHI::FGPUAllocationInfo> GpuAllocations;
            RHI::GetGPUAllocations(GpuAllocations);
            THashMap<FString, SMemoryGpuCategory> GpuByCategory;
            for (const RHI::FGPUAllocationInfo& Allocation : GpuAllocations)
            {
                const bool bTexture = Allocation.Kind == RHI::EGPUAllocationKind::Texture;
                const bool bHostVisible = !bTexture && Allocation.Memory != RHI::EMemoryType::GPUOnly;
                (bHostVisible ? Out.GpuHostVisibleMB : Out.GpuDeviceLocalMB) += EngineBytesToMB(Allocation.Size);

                const FStringView FullName(Allocation.Name);
                const int32 PrefixEnd = FullName.Find('.');
                const FStringView Prefix = FullName.IsEmpty() ? FStringView("<unnamed>") : (PrefixEnd == Constants::kIndexNone ? FullName : FullName.Left(PrefixEnd));
                const char* MemoryName = bTexture ? "Texture"
                    : Allocation.Memory == RHI::EMemoryType::CPUWrite ? "CPUWrite"
                    : Allocation.Memory == RHI::EMemoryType::CPURead ? "CPURead" : "GPUOnly";

                FString Key(Prefix);
                Key += '|';
                Key += MemoryName;
                SMemoryGpuCategory& Row = GpuByCategory.FindOrAdd(Key);
                Row.Name = FString(Prefix);
                Row.Memory = MemoryName;
                Row.MB += EngineBytesToMB(Allocation.Size);
                ++Row.Count;
            }
            for (auto& [Key, Row] : GpuByCategory)
            {
                Out.GpuCategories.Add(Move(Row));
            }
            Algo::Sort(Out.GpuCategories, [](const SMemoryGpuCategory& A, const SMemoryGpuCategory& B) { return A.MB > B.MB; });
            if (Out.GpuCategories.Num() > Top)
            {
                Out.GpuCategories.SetNum(Top);
            }

            if (In.bReset)
            {
                Memory::ResetTracking();
            }
        }

        CClass* FindEngineSettingsClass(FStringView Wanted)
        {
            CClass* Found = nullptr;
            GConfig->ForEachSettingsClass([&](CClass* Class)
            {
                const FString Name(*Class->GetName().ToString());
                const FString Display = Class->HasMeta("DisplayName") ? Class->GetMeta("DisplayName") : FString();
                const FStringView Bare = FStringView(Name).StartsWith('C') ? FStringView(Name).Mid(1) : FStringView(Name);
                if (Found == nullptr && (EqualsIgnoreCase(Wanted, FStringView(Name)) || EqualsIgnoreCase(Wanted, Bare)
                    || EqualsIgnoreCase(Wanted, FStringView(Display))))
                {
                    Found = Class;
                }
            });
            return Found;
        }

        SSettingsClassInfo DescribeEngineSettingsClass(CClass* Class)
        {
            SSettingsClassInfo Info;
            Info.Name        = FString(*Class->GetName().ToString());
            Info.DisplayName = Class->HasMeta("DisplayName") ? Class->GetMeta("DisplayName") : Info.Name;
            Info.ConfigFile  = Class->HasMeta("ConfigFile") ? Class->GetMeta("ConfigFile") : FString();
            return Info;
        }

        Agent::FToolResult GetEngineSettings(const SEngineSettingsParams& In, SEngineSettingsResult& Out)
        {
            if (In.Class.IsEmpty())
            {
                GConfig->ForEachSettingsClass([&](CClass* Class) { Out.Classes.Add(DescribeEngineSettingsClass(Class)); });
                return Agent::FToolResult::Ok(Lumina::Format("{} settings classes.", Out.Classes.Num()));
            }

            CClass* Class = FindEngineSettingsClass(FStringView(In.Class));
            if (Class == nullptr)
            {
                return Agent::FToolResult::Error(Lumina::Format("No settings class named '{}'.", In.Class));
            }

            FJsonValue Values;
            if (const Agent::FMarshalResult Written = Agent::WriteStruct(Class, Class->GetDefaultObject(), Values);
                !Written.IsValid())
            {
                return Agent::FToolResult::Error(Written.Error);
            }

            SSettingsClassInfo& Info = Out.Classes.EmplaceGetRef(DescribeEngineSettingsClass(Class));
            Info.Values = Values.ToString(2);
            return Agent::FToolResult::Ok(Lumina::Format("{} {}", Info.Name, Info.Values));
        }

        Agent::FToolResult SetEngineSetting(const SEngineSettingsParams& In, SEngineSettingsResult& Out)
        {
            CClass* Class = FindEngineSettingsClass(FStringView(In.Class));
            if (Class == nullptr)
            {
                return Agent::FToolResult::Error(Lumina::Format("No settings class named '{}'.", In.Class));
            }

            FJsonValue Value;
            if (!FJsonValue::Parse(In.Value, Value))
            {
                return Agent::FToolResult::Error("Value is not JSON. Strings and enum names need quotes.");
            }

            CObject* Defaults = Class->GetDefaultObject();
            const Agent::FObjectEditResult Edit = Agent::SetObjectProperty(
                Class, Defaults, Defaults, FStringView(In.Path), Value, "Set Setting (agent)");
            if (!Edit.IsValid())
            {
                return Agent::FToolResult::Error(Edit.Error);
            }

            GConfig->SaveSettings(Class);
            Out.Previous = Edit.Previous;
            Out.Current  = Edit.Current;
            return Agent::FToolResult::Ok(Lumina::Format("{}.{} is now {}, was {}.", Class->GetName(), In.Path, Out.Current, Out.Previous));
        }

        Agent::FToolResult ProfileEngineGpu(const SGpuProfileParams& In, SGpuProfile& Out)
        {
            FGPUProfiler& Profiler = FGPUProfiler::Get();
            if (!Profiler.IsEnabled())
            {
                Profiler.SetEnabled(true);
                return Agent::FToolResult::Ok("GPU profiler enabled; call again after a few frames.");
            }
            if (!Profiler.HasResolvedFrame())
            {
                return Agent::FToolResult::Ok("No resolved GPU frame yet; call again after a few frames.");
            }

            const FGPUProfileFrame& Frame = Profiler.GetLatest();
            Out.bReady  = true;
            Out.TotalMs = (float)Frame.TotalMs;
            for (const FGPUProfileScope& Scope : Frame.Scopes)
            {
                SGpuPassTime& Pass = Out.Passes.EmplaceGetRef();
                Pass.Name  = *Scope.Name;
                Pass.Ms    = (float)Scope.DurationMs();
                Pass.Depth = Scope.Depth;
            }
            Algo::Sort(Out.Passes, [](const SGpuPassTime& A, const SGpuPassTime& B) { return A.Ms > B.Ms; });
            Out.Passes.SetNum(Math::Min(Out.Passes.Num(), Math::Max(In.Top, 1)));

            FString Summary = Lumina::Format("GPU {:.2f} ms.", Out.TotalMs);
            for (const SGpuPassTime& Pass : Out.Passes)
            {
                Summary += Lumina::Format(" {}[{}] {:.2f}", Pass.Name, Pass.Depth, Pass.Ms);
            }
            return Agent::FToolResult::Ok(Summary);
        }

        void RegisterEngineStats(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SEngineStatsParams, SEngineStats>(
                Owner, "engine.stats",
                "Report frame time percentiles and hitch counts since the last reset, with process and GPU memory, live objects, "
                "managed bindings and per-world entity counts. Always recording, no profiler needed.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SEngineStatsParams& In, SEngineStats& Out)
                {
                    FillEngineStats(In, Out);
                    return Agent::FToolResult::Ok(Lumina::Format(
                        "{} frames over {:.1f} s, mean {:.2f} ms, p99 {:.2f} ms, max {:.2f} ms, {} over 50 ms. {} MB process, {} MB GPU.",
                        Out.Frames, Out.WindowSeconds, Out.MeanMs, Out.P99Ms, Out.MaxMs, Out.Over50Ms, Out.ProcessMemoryMB, Out.GpuUsageMB));
                });
        }

        void RegisterEngineGpuProfile(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SGpuProfileParams, SGpuProfile>(
                Owner, "engine.gpu_profile",
                "Report the costliest GPU passes of the latest resolved frame. The first call turns the profiler on, so call "
                "again a few frames later to read it.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread, &ProfileEngineGpu);
        }

        void RegisterEngineMemory(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SMemoryReportParams, SMemoryReport>(
                Owner, "engine.memory",
                "Report process, allocator-ledger (by category), managed and GPU memory, and with Callstacks On the call sites "
                "holding the most live bytes. Compare reports around a repeated action to find growth.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SMemoryReportParams& In, SMemoryReport& Out)
                {
                    FillEngineMemoryReport(In, Out);
                    return Agent::FToolResult::Ok(Lumina::Format(
                        "{} MB process, {:.1f} MB tracked native in {} allocations, {:.1f} MB managed heap, {} script contexts.",
                        Out.ProcessMB, Out.TrackedNativeMB, Out.TrackedAllocations, Out.ManagedHeapMB, Out.ScriptContexts));
                });
        }

        void RegisterEngineSettings(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SEngineSettingsParams, SEngineSettingsResult>(
                Owner, "engine.settings",
                "Get lists the developer settings classes or one class's values. Set changes one field and saves it to its "
                "config file as the Settings panel does, so it applies live and persists.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SEngineSettingsParams& In, SEngineSettingsResult& Out)
                {
                    return In.Action == EEngineSettingsAction::Set ? SetEngineSetting(In, Out) : GetEngineSettings(In, Out);
                });
        }
    }

    using namespace MCPRuntimeToolsInternal;

    void RegisterRuntimeTools(FStringView Owner)
    {
        RegisterEngineStats(Owner);
        RegisterEngineGpuProfile(Owner);
        RegisterEngineMemory(Owner);
        RegisterEngineSettings(Owner);
    }
}
