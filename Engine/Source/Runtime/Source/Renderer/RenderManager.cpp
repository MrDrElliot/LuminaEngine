#include "RuntimePCH.h"
#include "RenderManager.h"

#include "Tools/UI/ImGui/Vulkan/VulkanImGuiRender.h"

#include "ShaderCompiler.h"
#include "ShaderLibrary.h"
#include "RHI.h"
#include "RHICore.h"
#include "MeshletHeaderSlab.h"
#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Core/Application/Application.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Console/ConsoleVariable.h"
#include "Config/EngineSettings.h"
#include "Config/GameUserSettings.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Containers/HashPrimitives.h"
#include <thread>
#include "TaskSystem/TaskSystem.h"
#include "Core/Engine/Engine.h"
#include "Core/Windows/Window.h"
#include "Core/Profiler/Profile.h"
#include "Tools/UI/ImGui/ImGuiRenderer.h"
#include "UI/RmlUiBridge.h"
#include "World/World.h"
#include "World/WorldManager.h"
#include "World/Scene/RenderScene/RenderScene.h"
#include "Platform/Time/PlatformTime.h"

namespace Lumina
{
    TMulticastDelegate<void, FVector2> FRenderManager::OnSwapchainResized;

    // Not exported, so exactly one place can be wrong about whether a renderer exists.
    static FRenderManager* GRenderManager = nullptr;

    // Under 3 GiB so a Steam Deck (3 GiB UMA heap) still passes whatever its driver reserves off the heap.
    static constexpr uint32 kMinimumVRAMMiB     = 2816;
    static constexpr uint32 kRecommendedVRAMMiB = 6144;

    // Polled, since Windows sends no message GLFW forwards when the desktop switches HDR on or off.
    static constexpr double kHDRDisplayPollSeconds  = 1.0;
    static constexpr float  kFallbackPaperWhiteNits = 200.0f;
    static constexpr float  kFallbackPeakNits       = 1000.0f;

    // Mirrors FHDROutputArgs in Includes/HDROutput.slang.
    struct FHDROutputArgs
    {
        uint32 Composite;
        float  GainStops;
        float  PaperWhiteNits;
        float  PeakNits;
    };
    static_assert(sizeof(FHDROutputArgs) == 16, "FHDROutputArgs must match Includes/HDROutput.slang.");

    static void WarnIfBelowRecommendedVRAM()
    {
        const RHI::FGPUDeviceInfo Info = RHI::GetDeviceInfo();
        const uint64 MiB = Info.DeviceLocalMemoryBytes >> 20;

        if (MiB == 0 || MiB >= kRecommendedVRAMMiB)
        {
            return;
        }

        LOG_WARN("'{}' has {} MiB of graphics memory, under the {} MiB this renderer is tuned for. "
                 "Lower Streaming.Texture.PoolSizeMB and the viewport resolution if frames corrupt or stall.",
                 Info.Name, MiB, kRecommendedVRAMMiB);
    }

    void Internal::SetRenderManager(FRenderManager* Manager)
    {
        GRenderManager = Manager;
    }

    FRenderManager& Render()
    {
        ASSERT(GRenderManager != nullptr, "Render() with no renderer; this path is running headless, use TryRender()");
        return *GRenderManager;
    }

    // Beside the user settings, which is writable in an installed game where the shader cache is not.
    static FString PipelineCachePath()
    {
        if (Global::GCommandLine != nullptr && Global::GCommandLine->Has("nopipelinecache"))
        {
            return {};
        }
        const FString Settings = CGameUserSettings::GetSettingsFilePath();
        FString Path = Paths::Parent(FStringView(*Settings, Settings.Len()), true);

        // A packaged game creates the device before it knows its project name, which would share one cache across games.
        if (Global::GEngine == nullptr || (Global::GEngine->GetProjectPath().IsEmpty() && Global::GEngine->GetProjectName().IsEmpty()))
        {
            Path = Paths::Parent(FStringView(*Path, Path.Len()), true);
            Path += "/";
            #if PLATFORM_TCHAR_IS_WIDE
            Path += StringUtils::FromWideString(Platform::ExecutableName());
            #else
            Path += Platform::ExecutableName();
            #endif
        }

        Path += "/PipelineCache.bin";
        return Path;
    }

    FRenderManager* TryRender()
    {
        return GRenderManager;
    }

    namespace
    {
        // Half the cores, since a driver compile pins a whole thread and the frame's own jobs still need workers.
        uint32 MaxConcurrentPipelineBuilds()
        {
            const uint32 Cores = Math::Max(1u, std::thread::hardware_concurrency());
            return Math::Clamp(Cores / 2u, 1u, 8u);
        }

        // Saved after a burst rather than per pipeline, since the whole cache is rewritten each time.
        constexpr uint32 kBuildsPerCacheSave = 16;

        // Prewarm builds never take more than this share of the workers, so a pipeline a frame is waiting on still finds one.
        uint32 MaxConcurrentPrewarmBuilds(uint32 MaxInFlight)
        {
            return Math::Max(1u, MaxInFlight / 2u);
        }

        // Runs a record has stayed unbuildable before it is dropped, since its material may be gone for good.
        constexpr uint32 kPrewarmMaxAge = 8;

        constexpr uint32 kPrewarmMagic   = 0x4D525750u;
        constexpr uint32 kPrewarmVersion = 1u;

        struct FPrewarmFileHeader
        {
            uint32 Magic   = kPrewarmMagic;
            uint32 Version = kPrewarmVersion;
            uint32 Count   = 0;
            uint32 RecordSize = sizeof(FPipelinePrewarmRecord);
        };

        struct FPrewarmFileEntry
        {
            FPipelinePrewarmRecord Record;
            uint32                 Age  = 0;
            uint32                 _Pad = 0;
        };
        static_assert(sizeof(FPrewarmFileEntry) == sizeof(FPipelinePrewarmRecord) + 8, "Written as raw bytes, so it must hold no padding.");

        uint64 HashPrewarmRecord(const FPipelinePrewarmRecord& Record)
        {
            return Containers::HashBytes(&Record, sizeof(Record));
        }

        enum EPendingState : uint8
        {
            PendingQueued  = 0,
            PendingRunning = 1,
            PendingDone    = 2,
        };
    }

    struct FScenePipelineCache::FPendingBuild
    {
        uint64                Hash = 0;
        FShaderH              Shaders[3];
        FPipelineBuild        Build;
        std::atomic<uint8>    State{ PendingQueued };
        Jobs::FTaskEvent      Done;
    };

    RHI::FPipelineH FScenePipelineCache::Find(uint64 Hash)
    {
        FReadScopeLock Lock(Mutex);
        auto It = Entries.Find(Hash);
        return It != nullptr ? It->Pipeline : RHI::FPipelineH{};
    }

    bool FScenePipelineCache::IsPending(uint64 Hash)
    {
        FReadScopeLock Lock(Mutex);
        return PendingBuilds.Contains(Hash);
    }

    TSharedPtr<FScenePipelineCache::FPendingBuild> FScenePipelineCache::AddPending(uint64 Hash, const FShaderH (&Shaders)[3], FPipelineBuild&& Build, bool bQueued)
    {
        TSharedPtr<FPendingBuild> Pending = MakeShared<FPendingBuild>();
        Pending->Hash = Hash;
        for (uint32 i = 0; i < 3; ++i)
        {
            Pending->Shaders[i] = Shaders[i];
        }
        Pending->Build = Move(Build);
        Pending->State.store(bQueued ? PendingQueued : PendingRunning, std::memory_order_relaxed);
        PendingBuilds.TryAdd(Hash, Pending);
        return Pending;
    }

    RHI::FPipelineH FScenePipelineCache::RunBuild(uint64 Hash, FPendingBuild& Pending)
    {
        const RHI::FPipelineH Pipeline = Pending.Build();
        Pending.Build = {};

        {
            FWriteScopeLock Lock(Mutex);
            if (Pipeline)
            {
                Entries.TryAdd(Hash, FEntry{ Pipeline, { Pending.Shaders[0], Pending.Shaders[1], Pending.Shaders[2] } });
            }
            if (auto It = PendingBuilds.Find(Hash); It != nullptr && It->get() == &Pending)
            {
                PendingBuilds.Remove(Hash);
            }
        }

        Pending.State.store(PendingDone, std::memory_order_release);
        Pending.Done.Signal();
        return Pipeline;
    }

    RHI::FPipelineH FScenePipelineCache::Request(uint64 Hash, const FShaderH (&Shaders)[3], FPipelineBuild&& Build, bool bPrewarm)
    {
        {
            FWriteScopeLock Lock(Mutex);
            if (auto It = Entries.Find(Hash); It != nullptr)
            {
                return It->Pipeline;
            }
            if (PendingBuilds.Contains(Hash) || !Build)
            {
                return {};
            }

            TSharedPtr<FPendingBuild> Pending = AddPending(Hash, Shaders, Move(Build), true);
            FScopeLock QueueLock(QueueMutex);
            (bPrewarm ? PrewarmQueue : Queue).Add(Move(Pending));
        }

        Pump();
        return {};
    }

    RHI::FPipelineH FScenePipelineCache::BuildNow(uint64 Hash, const FShaderH (&Shaders)[3], FPipelineBuild&& Build)
    {
        TSharedPtr<FPendingBuild> Pending;
        bool bOwn = false;
        {
            FWriteScopeLock Lock(Mutex);
            if (auto It = Entries.Find(Hash); It != nullptr)
            {
                return It->Pipeline;
            }

            if (auto It = PendingBuilds.Find(Hash); It != nullptr)
            {
                Pending = (*It);
                // A build still waiting for a worker is taken over here rather than waited behind the queue.
                uint8 Expected = PendingQueued;
                bOwn = Pending->State.compare_exchange_strong(Expected, PendingRunning, std::memory_order_acq_rel);
            }
            else if (Build)
            {
                Pending = AddPending(Hash, Shaders, Move(Build), false);
                bOwn = true;
            }
            else
            {
                return {};
            }
        }

        if (bOwn)
        {
            return RunBuild(Hash, *Pending);
        }

        Pending->Done.Wait();
        return Find(Hash);
    }

    struct FScenePipelineCache::FPumpJob
    {
        static Jobs::TTask<void> Run(FScenePipelineCache* Cache, TSharedPtr<FPendingBuild> Pending)
        {
            Cache->RunQueued(*Pending);
            co_return;
        }
    };

    void FScenePipelineCache::RunQueued(FPendingBuild& Pending)
    {
        uint8 Expected = PendingQueued;
        if (Pending.State.compare_exchange_strong(Expected, PendingRunning, std::memory_order_acq_rel))
        {
            RunBuild(Pending.Hash, Pending);
            BuiltSinceSave.fetch_add(1, std::memory_order_relaxed);
        }

        bool bDrained = false;
        {
            FScopeLock Lock(QueueMutex);
            --InFlight;
            bDrained = InFlight == 0 && Queue.IsEmpty() && PrewarmQueue.IsEmpty();
        }

        if (bDrained && BuiltSinceSave.load(std::memory_order_relaxed) >= kBuildsPerCacheSave)
        {
            BuiltSinceSave.store(0, std::memory_order_relaxed);
            RHI::SavePipelineCache();
            SavePrewarm();
        }
        Pump();
    }

    void FScenePipelineCache::Pump()
    {
        static const uint32 MaxInFlight = MaxConcurrentPipelineBuilds();

        for (;;)
        {
            TSharedPtr<FPendingBuild> Next;
            {
                FScopeLock Lock(QueueMutex);
                if (InFlight >= MaxInFlight)
                {
                    return;
                }
                if (!Queue.IsEmpty())
                {
                    Next = Move(Queue[0]);
                    Queue.RemoveAt(0);
                }
                else if (!PrewarmQueue.IsEmpty() && InFlight < MaxConcurrentPrewarmBuilds(MaxInFlight))
                {
                    Next = PrewarmQueue.Pop();
                }
                else
                {
                    return;
                }
                ++InFlight;
            }

            // Background, so a thread assist-waiting on frame work never inherits a driver compile.
            Jobs::Spawn(Jobs::EJobPriority::Background, &FPumpJob::Run, this, Move(Next));
        }
    }

    void FScenePipelineCache::WaitForBuilds()
    {
        {
            FScopeLock Lock(QueueMutex);
            for (TVector<TSharedPtr<FPendingBuild>>* Waiting : { &Queue, &PrewarmQueue })
            {
                for (TSharedPtr<FPendingBuild>& Pending : *Waiting)
                {
                    uint8 Expected = PendingQueued;
                    if (Pending->State.compare_exchange_strong(Expected, PendingDone, std::memory_order_acq_rel))
                    {
                        Pending->Done.Signal();
                    }
                }
                Waiting->Reset();
            }
        }

        // A worker still compiling writes into Entries and needs the device alive until it returns.
        for (;;)
        {
            {
                FScopeLock Lock(QueueMutex);
                if (InFlight == 0)
                {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        FWriteScopeLock Lock(Mutex);
        PendingBuilds.Reset();
    }

    void FScenePipelineCache::PurgeStale()
    {
        FWriteScopeLock Lock(Mutex);
        for (auto It = Entries.begin(); It != Entries.end();)
        {
            bool bStale = false;
            for (const FShaderH& Shader : It->Value.Shaders)
            {
                bStale |= Shader != nullptr && FShaderLibrary::Resolve(Shader) == nullptr;
            }

            if (bStale)
            {
                RHI::Retire(It->Value.Pipeline);
                It = Entries.Remove(It);
            }
            else
            {
                ++It;
            }
        }
    }

    void FScenePipelineCache::ReleaseAll()
    {
        WaitForBuilds();
        SavePrewarm();
        FWriteScopeLock Lock(Mutex);
        for (auto& [Hash, Entry] : Entries)
        {
            RHI::Retire(Entry.Pipeline);
        }
        Entries.Reset();
    }

    void FScenePipelineCache::LoadPrewarm(const FString& Path)
    {
        FScopeLock Lock(PrewarmMutex);
        PrewarmPath = Path;
        PrewarmRecords.Reset();
        PrewarmPending.Reset();
        PrewarmCursor = 0;

        const FStringView PathView(*Path, Path.Len());
        TVector<uint8> Data;
        if (Path.IsEmpty() || !Paths::Exists(PathView) || !FileHelper::LoadFileToArray(Data, PathView) || Data.Num() < (int32)sizeof(FPrewarmFileHeader))
        {
            return;
        }

        FPrewarmFileHeader Header;
        Memory::Memcpy(&Header, Data.GetData(), sizeof(Header));
        const size_t Expected = sizeof(FPrewarmFileHeader) + (size_t)Header.Count * sizeof(FPrewarmFileEntry);
        if (Header.Magic != kPrewarmMagic || Header.Version != kPrewarmVersion || Header.RecordSize != sizeof(FPipelinePrewarmRecord) || (size_t)Data.Num() != Expected)
        {
            LOG_WARN("Pipeline prewarm list at {} is from another version and was ignored.", Path);
            return;
        }

        const uint8* Cursor = Data.GetData() + sizeof(FPrewarmFileHeader);
        for (uint32 i = 0; i < Header.Count; ++i, Cursor += sizeof(FPrewarmFileEntry))
        {
            FPrewarmFileEntry Entry;
            Memory::Memcpy(&Entry, Cursor, sizeof(Entry));
            if (Entry.Age + 1u > kPrewarmMaxAge)
            {
                continue;
            }
            const uint64 Hash = HashPrewarmRecord(Entry.Record);
            if (PrewarmRecords.TryAdd(Hash, FPrewarmEntry{ Entry.Record, Entry.Age + 1u }))
            {
                PrewarmPending.Add(Hash);
            }
        }
        LOG_INFO("Pipeline prewarm list holds {} pipelines.", PrewarmPending.Num());
    }

    void FScenePipelineCache::SavePrewarm()
    {
        FScopeLock Lock(PrewarmMutex);
        if (PrewarmPath.IsEmpty())
        {
            return;
        }

        FPrewarmFileHeader Header;
        Header.Count = (uint32)PrewarmRecords.Num();
        TVector<uint8> Data;
        Data.SetNum((int32)(sizeof(Header) + Header.Count * sizeof(FPrewarmFileEntry)));
        Memory::Memcpy(Data.GetData(), &Header, sizeof(Header));
        uint8* Cursor = Data.GetData() + sizeof(Header);
        for (const auto& [Hash, Recorded] : PrewarmRecords)
        {
            FPrewarmFileEntry Entry;
            Entry.Record = Recorded.Record;
            Entry.Age    = Recorded.Age;
            Memory::Memcpy(Cursor, &Entry, sizeof(Entry));
            Cursor += sizeof(Entry);
        }

        const FString Parent = Paths::Parent(FStringView(*PrewarmPath, PrewarmPath.Len()), true);
        if (!Parent.IsEmpty())
        {
            Paths::CreateDirectories(FStringView(*Parent, Parent.Len()));
        }
        if (!FileHelper::SaveArrayToFile(Data, FStringView(*PrewarmPath, PrewarmPath.Len())))
        {
            LOG_WARN("Could not write the pipeline prewarm list to {}.", PrewarmPath);
        }
    }

    void FScenePipelineCache::RecordPrewarm(const FPipelinePrewarmRecord& Record)
    {
        const uint64 Hash = HashPrewarmRecord(Record);
        FScopeLock Lock(PrewarmMutex);
        if (PrewarmPath.IsEmpty())
        {
            return;
        }
        if (FPrewarmEntry* Existing = PrewarmRecords.Find(Hash))
        {
            Existing->Age = 0;
            return;
        }
        PrewarmRecords.TryAdd(Hash, FPrewarmEntry{ Record, 0u });
    }

    void FScenePipelineCache::PrewarmRecorded(uint32 Max, TFunctionRef<bool(const FPipelinePrewarmRecord&)> Try)
    {
        TInlineVector<uint64, 32> Hashes;
        TInlineVector<FPipelinePrewarmRecord, 32> Batch;
        {
            FScopeLock Lock(PrewarmMutex);
            const uint32 Pending = (uint32)PrewarmPending.Num();
            const uint32 Take = Math::Min(Max, Pending);
            for (uint32 i = 0; i < Take; ++i)
            {
                const uint64 Hash = PrewarmPending[(PrewarmCursor + i) % Pending];
                if (const FPrewarmEntry* Entry = PrewarmRecords.Find(Hash))
                {
                    Hashes.Add(Hash);
                    Batch.Add(Entry->Record);
                }
            }
            PrewarmCursor = Pending > 0 ? (PrewarmCursor + Take) % Pending : 0;
        }

        TInlineVector<uint64, 32> Requested;
        for (int32 i = 0; i < Batch.Num(); ++i)
        {
            if (Try(Batch[i]))
            {
                Requested.Add(Hashes[i]);
            }
        }
        if (Requested.IsEmpty())
        {
            return;
        }

        FScopeLock Lock(PrewarmMutex);
        for (uint64 Hash : Requested)
        {
            PrewarmPending.RemoveSwap(Hash);
            if (FPrewarmEntry* Entry = PrewarmRecords.Find(Hash))
            {
                Entry->Age = 0;
            }
        }
        PrewarmCursor = PrewarmPending.IsEmpty() ? 0 : PrewarmCursor % (uint32)PrewarmPending.Num();
        if (PrewarmPending.IsEmpty())
        {
            LOG_INFO("Pipeline prewarm requested every recorded pipeline.");
        }
    }

    FRenderManager::FRenderManager()
    {
    }

    FRenderManager::~FRenderManager()
    {
        // Detach from resize events before teardown so a late resize can't enqueue work.
        FWindow::OnWindowResized.Remove(WindowResizedHandle);

        #if USING(WITH_DEV_UI)
        ImGuiRenderer->Deinitialize();
        Memory::Delete(ImGuiRenderer);
        ImGuiRenderer = nullptr;
        #endif

        // There is no next frame to clear the extract gate, and slot writes need the manager alive.
        ReleaseQueue.FlushAll();

        MaterialManager   = nullptr;
        CollectionManager = nullptr;

        if (SharedRenderResources.bInitialized)
        {
            // Release retires the storage slot it handed out, and freeing it here recycles the index early.
            RHI::Textures::Release(SharedRenderResources.BRDFLut);
            RHI::Textures::Release(SharedRenderResources.SMAAArea);
            RHI::Textures::Release(SharedRenderResources.SMAASearch);
            #if WITH_EDITOR
            for (RHI::FManagedTexture& Icon : SharedRenderResources.EditorIcons)
            {
                RHI::Textures::Release(Icon);
            }
            #endif
        }
        SharedRenderResources.Reset();
        ScenePipelineCache.ReleaseAll();

        Global::GShaderCompiler = nullptr;
        if (ShaderCompiler != nullptr)
        {
            ShaderCompiler->Shutdown();
            Memory::Delete(ShaderCompiler);
            ShaderCompiler = nullptr;
        }
        Global::GShaderLibrary = nullptr;
        if (ShaderLibrary != nullptr)
        {
            Memory::Delete(ShaderLibrary);
            ShaderLibrary = nullptr;
        }

        // Before Shutdown, which drains the retire queues the slab hands its allocation to.
        MeshletHeaderSlab::Shutdown();

        ReleaseHDRTargets();
        RHI::Retire(HDREncodePipeline);
        HDREncodePipeline = {};

        SwapchainTarget.Shutdown();
        RHI::FreeDevice();
    }

    void FRenderManager::Initialize()
    {
        #if defined(LUMINA_WITH_VALIDATION)
        bool bValidation = true;
        #else
        bool bValidation = false;
        #endif

        #if defined(LE_SHIPPING)
        constexpr bool bDebugUtils = false;
        #else
        constexpr bool bDebugUtils = true;
        #endif
        
        if (Global::GCommandLine != nullptr)
        {
            if (Global::GCommandLine->Has("validation"))
            {
                bValidation = true;
            }
            if (Global::GCommandLine->Has("novalidation"))
            {
                bValidation = false;
            }
        }
        
        uint32 MinVRAMMiB = kMinimumVRAMMiB;
        if (Global::GCommandLine != nullptr)
        {
            if (Global::GCommandLine->Has("ignoreminspec"))
            {
                LOG_WARN("-ignoreminspec, so the {} MiB graphics memory minimum is not enforced. "
                         "Expect corrupt rendering and device loss on a card below it.", kMinimumVRAMMiB);
                MinVRAMMiB = 0;
            }
            // Exercises the rejection dialog on hardware that would otherwise pass.
            else if (const TOptional<int> Override = Global::GCommandLine->GetInt("minvram"))
            {
                MinVRAMMiB = (uint32)Math::Max(*Override, 0);
                LOG_WARN("-minvram, so the graphics memory minimum is {} MiB rather than {} MiB.",
                         MinVRAMMiB, kMinimumVRAMMiB);
            }
        }

        const bool bRenderBootTimings = Global::GCommandLine != nullptr && Global::GCommandLine->Has("boottimings");
        double RenderBootLast = PlatformTime::Seconds();
        auto RenderBootMark = [&RenderBootLast, bRenderBootTimings](const char* Name)
        {
            const double Now = PlatformTime::Seconds();
            if (bRenderBootTimings)
            {
                LOG_DISPLAY("[boot]     {} {} ms", Name, (Now - RenderBootLast) * 1000.0);
            }
            RenderBootLast = Now;
        };

        const FString CachePath = PipelineCachePath();
        RHI::CreateDevice(RHI::FDeviceDesc
        {
            .bValidation = bValidation,
            .bDebugUtils = bDebugUtils,
            .bHeadless   = false,
            // Meshlets draw through the vertex path without mesh shading, and -nomeshshaders forces that path.
            .DisabledFeatures = Global::GCommandLine != nullptr && Global::GCommandLine->Has("nomeshshaders") ? RHI::EDeviceFeature::MeshShading : RHI::EDeviceFeature::None,
            .MinDeviceLocalMemoryMiB = MinVRAMMiB,
            .PipelineCachePath = CachePath,
        });
        RenderBootMark("RHI::CreateDevice");

        // Kept beside the driver's cache, so -nopipelinecache turns both off.
        ScenePipelineCache.LoadPrewarm(CachePath.IsEmpty() ? FString() : Paths::Parent(FStringView(*CachePath, CachePath.Len()), true) + "/PipelinePrewarm.bin");

        WarnIfBelowRecommendedVRAM();

        ShaderLibrary   = Memory::New<FShaderLibrary>();
        Global::GShaderLibrary  = ShaderLibrary;
        ShaderCompiler  = Memory::New<FSpirVShaderCompiler>();
        Global::GShaderCompiler = ShaderCompiler;
        ShaderCompiler->Initialize();
        RenderBootMark("ShaderCompiler::Initialize");

        FWindow* Window = Windowing::GetPrimaryWindowHandle();
        SwapchainTarget.Initialize(RHI::CreateSurface(Window->GetWindow()), Window->GetExtent());
        RenderBootMark("Swapchain");

        WindowResizedHandle = FWindow::OnWindowResized.AddMember(this, &FRenderManager::OnWindowResized);

        MaterialManager   = MakeUnique<RHI::FMaterialManager>();
        CollectionManager = MakeUnique<RHI::FMaterialCollectionManager>();

        RenderBootMark("Material managers");

#if USING(WITH_DEV_UI)
        // A game draws its developer overlay over the game image, while the editor's UI owns the whole window.
        ImGuiRenderer = Memory::New<FVulkanImGuiRender>(!WITH_EDITOR);
        ImGuiRenderer->Initialize();
#endif
        RenderBootMark("ImGuiRenderer");
    }
    
    void FRenderManager::WaitForFrameSlot()
    {
        LUMINA_PROFILE_SECTION_COLORED("Frame Fence (GPU)", tracy::Color::Crimson);

        if (GetDefault<CRendererSettings>()->bLowLatencyMode)
        {
            const uint8 PreviousFrameIndex = (CurrentFrameIndex + RHI::kFramesInFlight - 1) % RHI::kFramesInFlight;
            RHI::WaitForSlotGPU(PreviousFrameIndex);
            PlotInputToGPUComplete(PreviousFrameIndex);
        }

        // FrameEnd records into CurrentFrameIndex and advances afterwards, so this is that same slot.
        RHI::BeginFrame(CurrentFrameIndex);
        PlotInputToGPUComplete(CurrentFrameIndex);
        bFrameSlotWaited = true;
    }

    void FRenderManager::MarkInputRead()
    {
        InputReadSeconds = PlatformTime::Seconds();
    }

    static void PlotMilliseconds(const char* PlotName, double StartSeconds, double EndSeconds)
    {
        TracyPlot(PlotName, (EndSeconds - StartSeconds) * 1000.0);
    }

    void FRenderManager::PlotInputToGPUComplete(uint8 FrameIndex)
    {
        if (SlotInputReadSeconds[FrameIndex] != 0.0)
        {
            PlotMilliseconds("Latency/InputToGPUComplete (ms)", SlotInputReadSeconds[FrameIndex], PlatformTime::Seconds());
            SlotInputReadSeconds[FrameIndex] = 0.0;
        }
    }

    void FRenderManager::FrameStart(const FUpdateContext& UpdateContext)
    {
        LUMINA_PROFILE_SCOPE();

        #if USING(WITH_DEV_UI)
        ImGuiRenderer->StartFrame(UpdateContext);
        #endif

        // Before the worlds sync, so a resource that landed is resolved and drawn this frame.
        PublishPendingMeshes();
    }

    void FRenderManager::WatchMeshPublish(CMesh* Mesh)
    {
        FScopeLock Lock(MeshPublishMutex);
        MeshesAwaitingPublish.AddUnique(TWeakObjectPtr<CMesh>(Mesh));
    }

    void FRenderManager::PublishPendingMeshes()
    {
        FScopeLock Lock(MeshPublishMutex);
        for (int32 i = 0; i < MeshesAwaitingPublish.Num(); )
        {
            CMesh* Mesh = MeshesAwaitingPublish[i].Get();
            if (Mesh == nullptr || Mesh->PublishPendingMeshResource())
            {
                MeshesAwaitingPublish.RemoveAtSwap(i);
            }
            else
            {
                ++i;
            }
        }
    }
    
    void FRenderManager::FrameEnd()
    {
        LUMINA_PROFILE_SCOPE();

        const uint8 ThisFrameIndex = CurrentFrameIndex;
        CurrentFrameIndex = (CurrentFrameIndex + 1) % RHI::kFramesInFlight;

        // Cleared here so a frame that never presents leaves no stale read behind for a later frame to plot.
        const double FrameInputReadSeconds = InputReadSeconds;
        const double ExtractedSeconds      = PlatformTime::Seconds();
        InputReadSeconds = 0.0;

        [[maybe_unused]] ImDrawData* ImGuiDrawData = nullptr;
        #if USING(WITH_DEV_UI)
        ImGuiDrawData = ImGuiRenderer->BuildFrame();
        #endif

        {
            // Covers a caller driving FrameEnd directly and a frame whose slot advanced past an early-out above.
            if (!bFrameSlotWaited)
            {
                LUMINA_PROFILE_SECTION_COLORED("Frame Fence (GPU)", tracy::Color::Crimson);
                RHI::BeginFrame(ThisFrameIndex);
                PlotInputToGPUComplete(ThisFrameIndex);
            }
            bFrameSlotWaited = false;

            // Before the scene renders, so last frame's present blit reaches the queue ahead of this frame's work.
            SwapchainTarget.FinishPresent();

            ResolveWindowCapture(ThisFrameIndex);

            ApplyPendingResize();

            UpdateHDROutput();

            // Read every frame like the other renderer settings, and a no-op unless the value moved.
            RHI::SetMaxAnisotropy((float)GetDefault<CRendererSettings>()->MaxAnisotropy);

            Global::GWorldManager->RenderWorlds(ThisFrameIndex);

            RHI::FTextureH SwapImage;
            {
                LUMINA_PROFILE_SECTION_COLORED("Acquire Swapchain", tracy::Color::Orange3);
                SwapImage = SwapchainTarget.Acquire();
            }
            if (!RHI::IsValid(SwapImage))
            {
                return;   // no drawable area this frame; Acquire already armed the retry
            }

            const FUIntVector2 Extent = SwapchainTarget.GetExtent();

            // An HDR10 swapchain is reached through the SDR composite, which carries the scene's highlight gain to the encode.
            const bool bHDRFrame = SwapchainTarget.IsHDR();
            const RHI::FTextureH WindowImage = bHDRFrame ? AcquireHDRComposite(Extent) : SwapImage;

            RHI::FCmdListH CL = RHI::OpenCommandList();
            RHI::CmdSetTextureHeap(CL, RHI::GetGlobalHeap());
            RHI::CmdBeginMarker(CL, "Present Pass");
            SwapchainTarget.BarrierToRender(CL);

            #if WITH_EDITOR
            {
                LUMINA_PROFILE_SECTION_COLORED("Editor UI", tracy::Color::SlateBlue1);
                RHI::CmdBeginMarker(CL, "Editor UI");
                RmlUi::RenderEditorContexts(CL);
                RHI::CmdEndMarker(CL);
            }
            #endif

            #if !WITH_EDITOR
            {
                LUMINA_PROFILE_SECTION_COLORED("Game Composite", tracy::Color::ForestGreen);

                IRenderScene* Scene = nullptr;
                if (FWorldContext* GameContext = Global::GWorldManager->GetPrimaryGameContext())
                {
                    if (CWorld* GameWorld = GameContext->World.Get())
                    {
                        Scene = GameWorld->GetRenderer();
                    }
                }

                const RHI::FTextureH Source = Scene ? Scene->GetDisplayTexture() : RHI::FTextureH{};
                if (Scene != nullptr)
                {
                    Scene->SetDisplayHeadroom(HDROutput.GetHeadroomStops());
                }
                if (RHI::IsValid(Source))
                {
                    RHI::CmdBlitTexture(CL, Source, RHI::FTextureSlice{}, WindowImage, RHI::FTextureSlice{}, RHI::EFilter::Linear);
                }
                else if (bHDRFrame)
                {
                    const float Black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                    RHI::CmdClearTexture(CL, WindowImage, Black);
                }
                RHI::CmdBarrier(CL,
                    RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                    RHI::EStageFlags::RasterColorOut, RHI::EAccessFlags::ColorRead | RHI::EAccessFlags::ColorWrite);
            }
            #endif

            // After the game composite, so a game's developer overlay lands on top of the image it covers.
            #if USING(WITH_DEV_UI)
            {
                LUMINA_PROFILE_SECTION_COLORED("ImGui Record", tracy::Color::SlateBlue3);
                RHI::CmdBeginMarker(CL, "ImGui");
                ImGuiRenderer->OnEndFrame(CL, WindowImage, Extent, ImGuiDrawData);
                RHI::CmdEndMarker(CL);
            }
            #endif

            RecordWindowCapture(CL, WindowImage, Extent, ThisFrameIndex);

            if (bHDRFrame)
            {
                EncodeHDRComposite(CL, SwapImage);
            }

            RHI::CmdEndMarker(CL);

            {
                LUMINA_PROFILE_SECTION_COLORED("Present", tracy::Color::Orange4);
                if (FrameInputReadSeconds != 0.0)
                {
                    PlotMilliseconds("Latency/InputToExtract (ms)", FrameInputReadSeconds, ExtractedSeconds);
                    PlotMilliseconds("Latency/InputToPresentSubmit (ms)", FrameInputReadSeconds, PlatformTime::Seconds());
                    SlotInputReadSeconds[ThisFrameIndex] = FrameInputReadSeconds;
                }
                // A secondary viewport's present waits this one out, so only a frame with tool windows torn off serializes.
                SwapchainTarget.PresentAsync(CL);
            }

            #if USING(WITH_DEV_UI)
            {
                // Renders and presents each dragged-out tool window into its own swapchain.
                LUMINA_PROFILE_SECTION_COLORED("ImGui Secondary Viewports", tracy::Color::SlateBlue4);
                ImGuiRenderer->RenderSecondaryViewports();
            }
            #endif
        }
    }

    uint64 FRenderManager::RequestWindowCapture()
    {
        FScopeLock Lock(WindowCaptureMutex);
        RequestedWindowCapture = NextWindowCaptureTicket++;
        return RequestedWindowCapture;
    }

    bool FRenderManager::TakeWindowCapture(uint64 Ticket, FWindowCapture& Out)
    {
        FScopeLock Lock(WindowCaptureMutex);
        if (Ticket == 0 || CompletedWindowCaptureTicket != Ticket)
        {
            return false;
        }

        Out = Move(CompletedWindowCapture);
        CompletedWindowCapture = {};
        CompletedWindowCaptureTicket = 0;
        return true;
    }

    void FRenderManager::RecordWindowCapture(RHI::FCmdListH CL, RHI::FTextureH WindowImage, const FUIntVector2& Extent, uint8 FrameIndex)
    {
        FScopeLock Lock(WindowCaptureMutex);
        if (RequestedWindowCapture == 0 || InFlightWindowCapture.Ticket != 0 || Extent.x == 0 || Extent.y == 0)
        {
            return;
        }

        const uint64 Bytes = (uint64)Extent.x * Extent.y * 4u;
        const RHI::FGPUAllocation Readback = RHI::Malloc(Bytes, RHI::kDefaultAlign, RHI::EMemoryType::CPURead);
        if (Readback.Gpu == 0)
        {
            return;
        }
        RHI::SetDebugName(Readback.Gpu, "Readback.WindowCapture");

        // Recorded after every UI pass and ahead of the present transition the submit appends.
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::Transfer, RHI::EAccessFlags::ColorWrite | RHI::EAccessFlags::TransferWrite,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead);

        // Readers expect the BGRA8 bytes an SDR swapchain holds, so the half-float HDR composite is converted first.
        RHI::FTextureH Readable = WindowImage;
        if (RHI::GetTextureDesc(WindowImage).Format != EFormat::BGRA8_UNORM)
        {
            if (!HDRCaptureScratch.IsValid() || RHI::GetTextureDesc(HDRCaptureScratch.Texture).Dimension.x != Extent.x
                || RHI::GetTextureDesc(HDRCaptureScratch.Texture).Dimension.y != Extent.y)
            {
                RHI::Textures::Release(HDRCaptureScratch);
                HDRCaptureScratch = RHI::Textures::Create(RHI::FTexture2DDesc
                {
                    .Width     = Extent.x,
                    .Height    = Extent.y,
                    .Format    = EFormat::BGRA8_UNORM,
                    .DebugName = "Window.HDRCaptureScratch",
                });
            }
            RHI::CmdBlitTexture(CL, WindowImage, RHI::FTextureSlice{}, HDRCaptureScratch.Texture, RHI::FTextureSlice{}, RHI::EFilter::Nearest);
            RHI::CmdBarrier(CL,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
                RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferRead);
            Readable = HDRCaptureScratch.Texture;
        }
        RHI::CmdCopyTextureToMemory(CL, Readable, RHI::FTextureSlice{}, Readback, Extent.x);
        RHI::CmdBarrier(CL,
            RHI::EStageFlags::Transfer, RHI::EAccessFlags::TransferWrite,
            RHI::EStageFlags::Host, RHI::EAccessFlags::HostRead);

        InFlightWindowCapture = { RequestedWindowCapture, Readback, Extent, FrameIndex };
        RequestedWindowCapture = 0;
    }

    void FRenderManager::ResolveWindowCapture(uint8 FrameIndex)
    {
        FScopeLock Lock(WindowCaptureMutex);
        if (InFlightWindowCapture.Ticket == 0 || InFlightWindowCapture.FrameIndex != FrameIndex)
        {
            return;
        }

        const FUIntVector2 Extent = InFlightWindowCapture.Extent;
        const uint8* Mapped = InFlightWindowCapture.Readback.CpuAs<const uint8>();
        if (Mapped != nullptr)
        {
            CompletedWindowCapture.Width  = Extent.x;
            CompletedWindowCapture.Height = Extent.y;
            CompletedWindowCapture.Pixels.Reset();
            CompletedWindowCapture.Pixels.Append(Mapped, Mapped + (size_t)Extent.x * Extent.y * 4u);
            CompletedWindowCaptureTicket = InFlightWindowCapture.Ticket;
        }

        RHI::Retire(InFlightWindowCapture.Readback);
        InFlightWindowCapture = {};
    }

    void FRenderManager::SwapchainResized(FVector2 NewSize)
    {
        OnSwapchainResized.Broadcast(NewSize);
    }

    void FRenderManager::RecreatePrimarySwapchain()
    {
        SwapchainTarget.Recreate();
    }

    void FRenderManager::SetUserHDROutput(EHDROutput Mode, float PaperWhiteNits)
    {
        bUserHDROutput     = true;
        UserHDRMode        = Mode;
        UserPaperWhiteNits = PaperWhiteNits;
    }

    void FRenderManager::UpdateHDROutput()
    {
        const CRendererSettings* Settings = GetDefault<CRendererSettings>();
        const EHDROutput Mode = bUserHDROutput ? UserHDRMode : Settings->HDROutput;

        FWindow* Window = Windowing::TryGetPrimaryWindowHandle();
        const double Now = PlatformTime::Seconds();
        if (Mode != EHDROutput::Off && Window != nullptr && Now >= NextHDRDisplayQuery)
        {
            NextHDRDisplayQuery = Now + kHDRDisplayPollSeconds;
            int X = 0;
            int Y = 0;
            Window->GetWindowPosition(X, Y);
            const FUIntVector2 Size = Window->GetExtent();
            const PlatformDisplay::FHDRDisplayInfo Previous = HDRDisplay;
            HDRDisplay = PlatformDisplay::QueryHDRDisplay(X + (int32)(Size.x / 2), Y + (int32)(Size.y / 2));
            if (HDRDisplay.bHDREnabled != Previous.bHDREnabled || HDRDisplay.PeakNits != Previous.PeakNits
                || HDRDisplay.SDRWhiteNits != Previous.SDRWhiteNits)
            {
                LOG_INFO("Display is in {} mode, peak {:.0f} nits, SDR white {:.0f} nits.", HDRDisplay.bHDREnabled ? "HDR" : "SDR",
                         HDRDisplay.PeakNits, HDRDisplay.SDRWhiteNits);
            }
        }

        SwapchainTarget.SetHDR(Mode == EHDROutput::On || (Mode == EHDROutput::Auto && HDRDisplay.bHDREnabled));

        const float PaperWhite = bUserHDROutput && UserPaperWhiteNits > 0.0f ? UserPaperWhiteNits : Settings->HDRPaperWhiteNits;
        const float Peak       = Settings->HDRPeakNits > 0.0f ? Settings->HDRPeakNits : HDRDisplay.PeakNits;

        const bool bWasActive = HDROutput.bActive;
        HDROutput.bActive        = SwapchainTarget.IsHDR();
        HDROutput.PaperWhiteNits = PaperWhite > 0.0f ? PaperWhite : (HDRDisplay.SDRWhiteNits > 0.0f ? HDRDisplay.SDRWhiteNits : kFallbackPaperWhiteNits);
        HDROutput.PeakNits       = Math::Max(Peak > 0.0f ? Peak : kFallbackPeakNits, HDROutput.PaperWhiteNits);

        if (HDROutput.bActive != bWasActive)
        {
            LOG_INFO("Presenting {}, paper white {:.0f} nits, peak {:.0f} nits.", HDROutput.bActive ? "HDR10" : "SDR",
                     HDROutput.PaperWhiteNits, HDROutput.PeakNits);
        }
        if (!HDROutput.bActive)
        {
            ReleaseHDRTargets();
        }
    }

    RHI::FTextureH FRenderManager::AcquireHDRComposite(const FUIntVector2& Extent)
    {
        if (HDRComposite.IsValid())
        {
            const FUIntVector3 Built = RHI::GetTextureDesc(HDRComposite.Texture).Dimension;
            if (Built.x == Extent.x && Built.y == Extent.y)
            {
                return HDRComposite.Texture;
            }
            RHI::Textures::Release(HDRComposite);
        }

        // Half floats, so the gain code in alpha keeps the precision 8 bits would band.
        HDRComposite = RHI::Textures::Create(RHI::FTexture2DDesc
        {
            .Width         = Extent.x,
            .Height        = Extent.y,
            .Format        = EFormat::RGBA16_FLOAT,
            .bRenderTarget = true,
            .DebugName     = "Window.HDRComposite",
        });
        return HDRComposite.Texture;
    }

    void FRenderManager::EncodeHDRComposite(RHI::FCmdListH CL, RHI::FTextureH SwapImage)
    {
        if (!HDREncodePipeline)
        {
            const RHI::FColorTarget ColorTarget { .Format = EFormat::R10G10B10A2_UNORM };
            RHI::FRasterDesc RasterDesc;
            RasterDesc.Topology     = RHI::ETopology::TriangleList;
            RasterDesc.ColorTargets = TSpan<const RHI::FColorTarget>(&ColorTarget, 1);
            HDREncodePipeline = RHI::CreateGraphicsPipeline("FullscreenQuad.slang", "HDRCompositeEncode.slang", RasterDesc);
            if (!HDREncodePipeline)
            {
                return;
            }
        }

        RHI::CmdBarrier(CL,
            RHI::EStageFlags::RasterColorOut | RHI::EStageFlags::Transfer, RHI::EAccessFlags::ColorWrite | RHI::EAccessFlags::TransferWrite,
            RHI::EStageFlags::PixelShader, RHI::EAccessFlags::ShaderRead);

        RHI::FRenderAttachment Color;
        Color.Texture = SwapImage;
        Color.LoadOp  = RHI::ELoadOp::Undefined;
        Color.StoreOp = RHI::EStoreOp::Store;

        RHI::FRenderPassDesc Pass;
        Pass.ColorAttachments = TSpan<const RHI::FRenderAttachment>(&Color, 1);
        Pass.RenderArea       = SwapchainTarget.GetExtent();

        RHI::CmdBeginMarker(CL, "HDR Encode");
        RHI::CmdBeginRenderPass(CL, Pass);
        RHI::CmdSetDepthStencil(CL, RHI::FDepthStencilDesc{});
        RHI::CmdSetCullMode(CL, RHI::ECullMode::None);
        RHI::CmdSetPipeline(CL, HDREncodePipeline);

        const FHDROutputArgs Args
        {
            .Composite      = HDRComposite.ResourceID(),
            .GainStops      = HDROutput.GetHeadroomStops(),
            .PaperWhiteNits = HDROutput.PaperWhiteNits,
            .PeakNits       = HDROutput.PeakNits,
        };
        RHI::CmdDraw(CL, RHI::CopyTransient(Args), 3, 1, 0, 0);
        RHI::CmdEndRenderPass(CL);
        RHI::CmdEndMarker(CL);
    }

    void FRenderManager::ReleaseHDRTargets()
    {
        if (HDRComposite.IsValid())
        {
            RHI::Textures::Release(HDRComposite);
        }
        if (HDRCaptureScratch.IsValid())
        {
            RHI::Textures::Release(HDRCaptureScratch);
        }
    }

    void FRenderManager::OnWindowResized(FWindow* Window, const FUIntVector2& Extent)
    {
        if (Window != Windowing::GetPrimaryWindowHandle() || Window->IsWindowMinimized())
        {
            return;
        }

        if (Extent.x == 0 || Extent.y == 0)
        {
            return;
        }
        
        PendingResizeExtent.store(((uint64)Extent.x << 32) | (uint64)Extent.y, std::memory_order_relaxed);
    }

    void FRenderManager::ApplyPendingResize()
    {
        const uint64 Packed = PendingResizeExtent.exchange(0, std::memory_order_acquire);
        if (Packed == 0)
        {
            return;
        }

        const FUIntVector2 Extent((uint32)(Packed >> 32), (uint32)(Packed & 0xFFFFFFFFull));

        // Against what the views were told, since a rebuild for another reason can already have taken the new size.
        if (Extent == BroadcastExtent)
        {
            return;
        }

        LUMINA_PROFILE_SCOPE();

        SwapchainTarget.Resize(Extent);
        BroadcastExtent = Extent;
        OnSwapchainResized.Broadcast(FVector2(Extent));
    }
}
