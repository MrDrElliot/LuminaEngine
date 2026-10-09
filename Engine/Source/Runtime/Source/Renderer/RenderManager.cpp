#include "RuntimePCH.h"
#include "RenderManager.h"

#include "Tools/UI/ImGui/Vulkan/VulkanImGuiRender.h"

#include "ShaderCompiler.h"
#include "ShaderLibrary.h"
#include "RHI.h"
#include "RHICore.h"
#include "MeshletHeaderSlab.h"
#include "Core/Application/Application.h"
#include "Core/CommandLine/CommandLine.h"
#include "Core/Console/ConsoleVariable.h"
#include "Config/EngineSettings.h"
#include "Config/GameUserSettings.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"
#include <thread>
#include "TaskSystem/Future.h"
#include "TaskSystem/Scheduler/JobScheduler.h"
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

    // Under 4 GiB so a card sold as 4 GB still passes whatever its driver reserves off the heap.
    static constexpr uint32 kMinimumVRAMMiB     = 3840;
    static constexpr uint32 kRecommendedVRAMMiB = 6144;

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
        if (GCommandLine != nullptr && GCommandLine->Has("nopipelinecache"))
        {
            return {};
        }
        const FString Settings = CGameUserSettings::GetSettingsFilePath();
        FString Path = Paths::Parent(FStringView(Settings.c_str(), Settings.size()), true);

        // A packaged game creates the device before it knows its project name, which would share one cache across games.
        if (GEngine == nullptr || (GEngine->GetProjectPath().empty() && GEngine->GetProjectName().empty()))
        {
            Path = Paths::Parent(FStringView(Path.c_str(), Path.size()), true);
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
            const uint32 Cores = Math::Max(1u, (uint32)std::thread::hardware_concurrency());
            return Math::Clamp(Cores / 2u, 1u, 8u);
        }

        // Saved after a burst rather than per pipeline, since the whole cache is rewritten each time.
        constexpr uint32 kBuildsPerCacheSave = 16;

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
        TPromise<void>        Done;
        TFuture<void>         DoneFuture = Done.GetFuture();
    };

    RHI::FPipelineH FScenePipelineCache::Find(uint64 Hash)
    {
        FReadScopeLock Lock(Mutex);
        auto It = Entries.find(Hash);
        return It != Entries.end() ? It->second.Pipeline : RHI::FPipelineH{};
    }

    bool FScenePipelineCache::IsPending(uint64 Hash)
    {
        FReadScopeLock Lock(Mutex);
        return PendingBuilds.find(Hash) != PendingBuilds.end();
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
        PendingBuilds.emplace(Hash, Pending);
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
                Entries.emplace(Hash, FEntry{ Pipeline, { Pending.Shaders[0], Pending.Shaders[1], Pending.Shaders[2] } });
            }
            if (auto It = PendingBuilds.find(Hash); It != PendingBuilds.end() && It->second.get() == &Pending)
            {
                PendingBuilds.erase(It);
            }
        }

        Pending.State.store(PendingDone, std::memory_order_release);
        Pending.Done.SetValue();
        return Pipeline;
    }

    RHI::FPipelineH FScenePipelineCache::Request(uint64 Hash, const FShaderH (&Shaders)[3], FPipelineBuild&& Build)
    {
        {
            FWriteScopeLock Lock(Mutex);
            if (auto It = Entries.find(Hash); It != Entries.end())
            {
                return It->second.Pipeline;
            }
            if (PendingBuilds.find(Hash) != PendingBuilds.end() || !Build)
            {
                return {};
            }

            TSharedPtr<FPendingBuild> Pending = AddPending(Hash, Shaders, Move(Build), true);
            FScopeLock QueueLock(QueueMutex);
            Queue.push_back(Move(Pending));
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
            if (auto It = Entries.find(Hash); It != Entries.end())
            {
                return It->second.Pipeline;
            }

            if (auto It = PendingBuilds.find(Hash); It != PendingBuilds.end())
            {
                Pending = It->second;
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

        Pending->DoneFuture.Wait();
        return Find(Hash);
    }

    struct FScenePipelineCache::FPumpJob
    {
        FScenePipelineCache*        Cache;
        TSharedPtr<FPendingBuild>   Pending;

        static void Run(void* Arg, uint32)
        {
            FPumpJob* Job = static_cast<FPumpJob*>(Arg);
            Job->Cache->RunQueued(*Job->Pending);
            Memory::Delete(Job);
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
            bDrained = InFlight == 0 && Queue.empty();
        }

        if (bDrained && BuiltSinceSave.load(std::memory_order_relaxed) >= kBuildsPerCacheSave)
        {
            BuiltSinceSave.store(0, std::memory_order_relaxed);
            RHI::SavePipelineCache();
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
                if (InFlight >= MaxInFlight || Queue.empty())
                {
                    return;
                }
                Next = Move(Queue.front());
                Queue.erase(Queue.begin());
                ++InFlight;
            }

            // Background, so a thread assist-waiting on frame work never inherits a driver compile.
            FPumpJob* Job = Memory::New<FPumpJob>(FPumpJob{ this, Move(Next) });
            Jobs::RunJob(&FPumpJob::Run, Job, Jobs::EJobPriority::Background, nullptr, "PipelineBuild");
        }
    }

    void FScenePipelineCache::WaitForBuilds()
    {
        {
            FScopeLock Lock(QueueMutex);
            for (TSharedPtr<FPendingBuild>& Pending : Queue)
            {
                uint8 Expected = PendingQueued;
                if (Pending->State.compare_exchange_strong(Expected, PendingDone, std::memory_order_acq_rel))
                {
                    Pending->Done.SetValue();
                }
            }
            Queue.clear();
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
        PendingBuilds.clear();
    }

    void FScenePipelineCache::PurgeStale()
    {
        FWriteScopeLock Lock(Mutex);
        for (auto It = Entries.begin(); It != Entries.end();)
        {
            bool bStale = false;
            for (const FShaderH& Shader : It->second.Shaders)
            {
                bStale |= Shader != nullptr && FShaderLibrary::Resolve(Shader) == nullptr;
            }

            if (bStale)
            {
                RHI::Retire(It->second.Pipeline);
                It = Entries.erase(It);
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
        FWriteScopeLock Lock(Mutex);
        for (auto& [Hash, Entry] : Entries)
        {
            RHI::Retire(Entry.Pipeline);
        }
        Entries.clear();
    }

    FRenderManager::FRenderManager()
    {
    }

    FRenderManager::~FRenderManager()
    {
        // Detach from resize events before teardown so a late resize can't enqueue work.
        FWindow::OnWindowResized.Remove(WindowResizedHandle);

        #if WITH_EDITOR
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

        GShaderCompiler = nullptr;
        if (ShaderCompiler != nullptr)
        {
            ShaderCompiler->Shutdown();
            Memory::Delete(ShaderCompiler);
            ShaderCompiler = nullptr;
        }
        GShaderLibrary = nullptr;
        if (ShaderLibrary != nullptr)
        {
            Memory::Delete(ShaderLibrary);
            ShaderLibrary = nullptr;
        }

        // Before Shutdown, which drains the retire queues the slab hands its allocation to.
        MeshletHeaderSlab::Shutdown();

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
        
        if (GCommandLine != nullptr)
        {
            if (GCommandLine->Has("validation"))
            {
                bValidation = true;
            }
            if (GCommandLine->Has("novalidation"))
            {
                bValidation = false;
            }
        }
        
        uint32 MinVRAMMiB = kMinimumVRAMMiB;
        if (GCommandLine != nullptr)
        {
            if (GCommandLine->Has("ignoreminspec"))
            {
                LOG_WARN("-ignoreminspec, so the {} MiB graphics memory minimum is not enforced. "
                         "Expect corrupt rendering and device loss on a card below it.", kMinimumVRAMMiB);
                MinVRAMMiB = 0;
            }
            // Exercises the rejection dialog on hardware that would otherwise pass.
            else if (const TOptional<int> Override = GCommandLine->GetInt("minvram"))
            {
                MinVRAMMiB = (uint32)Math::Max(*Override, 0);
                LOG_WARN("-minvram, so the graphics memory minimum is {} MiB rather than {} MiB.",
                         MinVRAMMiB, kMinimumVRAMMiB);
            }
        }

        const bool bRenderBootTimings = GCommandLine != nullptr && GCommandLine->Has("boottimings");
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

        RHI::CreateDevice(RHI::FDeviceDesc
        {
            .bValidation = bValidation,
            .bDebugUtils = bDebugUtils,
            .bHeadless   = false,
            // The scene renderer draws every meshlet through the mesh path; there is no fallback.
            .RequiredFeatures = RHI::EDeviceFeature::MeshShading,
            .MinDeviceLocalMemoryMiB = MinVRAMMiB,
            .PipelineCachePath = PipelineCachePath(),
        });
        RenderBootMark("RHI::CreateDevice");

        WarnIfBelowRecommendedVRAM();

        ShaderLibrary   = Memory::New<FShaderLibrary>();
        GShaderLibrary  = ShaderLibrary;
        ShaderCompiler  = Memory::New<FSpirVShaderCompiler>();
        GShaderCompiler = ShaderCompiler;
        ShaderCompiler->Initialize();
        RenderBootMark("ShaderCompiler::Initialize");

        FWindow* Window = Windowing::GetPrimaryWindowHandle();
        SwapchainTarget.Initialize(RHI::CreateSurface(Window->GetWindow()), Window->GetExtent());
        RenderBootMark("Swapchain");

        WindowResizedHandle = FWindow::OnWindowResized.AddMember(this, &FRenderManager::OnWindowResized);

        MaterialManager   = MakeUnique<RHI::FMaterialManager>();
        CollectionManager = MakeUnique<RHI::FMaterialCollectionManager>();

        RenderBootMark("Material managers");

#if WITH_EDITOR
        ImGuiRenderer = Memory::New<FVulkanImGuiRender>();
        ImGuiRenderer->Initialize();
#endif
        RenderBootMark("ImGuiRenderer");
    }
    
    void FRenderManager::WaitForFrameSlot()
    {
        LUMINA_PROFILE_SECTION_COLORED("Frame Fence (GPU)", tracy::Color::Crimson);

        // FrameEnd records into CurrentFrameIndex and advances afterwards, so this is that same slot.
        RHI::BeginFrame(CurrentFrameIndex);
        bFrameSlotWaited = true;
    }

    void FRenderManager::FrameStart(const FUpdateContext& UpdateContext)
    {
        LUMINA_PROFILE_SCOPE();

        #if WITH_EDITOR
        ImGuiRenderer->StartFrame(UpdateContext);
        #endif
        
    }
    
    void FRenderManager::FrameEnd()
    {
        LUMINA_PROFILE_SCOPE();

        const uint8 ThisFrameIndex = CurrentFrameIndex;
        CurrentFrameIndex = (CurrentFrameIndex + 1) % RHI::kFramesInFlight;

        [[maybe_unused]] ImDrawData* ImGuiDrawData = nullptr;
        #if WITH_EDITOR
        ImGuiDrawData = ImGuiRenderer->BuildFrame();
        #endif

        {
            // Normally already done at the top of the frame; this covers a caller that drives FrameEnd
            // directly, and a frame whose slot advanced past an early-out above.
            if (!bFrameSlotWaited)
            {
                LUMINA_PROFILE_SECTION_COLORED("Frame Fence (GPU)", tracy::Color::Crimson);
                RHI::BeginFrame(ThisFrameIndex);
            }
            bFrameSlotWaited = false;

            // Before the scene renders, so last frame's present blit reaches the queue ahead of this frame's work.
            SwapchainTarget.FinishPresent();

            ApplyPendingResize();

            // Read every frame like the other renderer settings, and a no-op unless the value moved.
            RHI::SetMaxAnisotropy((float)GetDefault<CRendererSettings>()->MaxAnisotropy);

            GWorldManager->RenderWorlds(ThisFrameIndex);

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

            #if WITH_EDITOR
            {
                LUMINA_PROFILE_SECTION_COLORED("ImGui Record", tracy::Color::SlateBlue3);
                RHI::CmdBeginMarker(CL, "ImGui");
                ImGuiRenderer->OnEndFrame_NewRHI(CL, SwapImage, Extent, ImGuiDrawData);
                RHI::CmdEndMarker(CL);
            }
            #endif

            #if !WITH_EDITOR
            {
                LUMINA_PROFILE_SECTION_COLORED("Game Composite", tracy::Color::ForestGreen);

                IRenderScene* Scene = nullptr;
                if (FWorldContext* GameContext = GWorldManager->GetPrimaryGameContext())
                {
                    if (CWorld* GameWorld = GameContext->World.Get())
                    {
                        Scene = GameWorld->GetRenderer();
                    }
                }

                const RHI::FTextureH Source = Scene ? Scene->GetDisplayTexture() : RHI::FTextureH{};
                if (RHI::IsValid(Source))
                {
                    RHI::CmdBlitTexture(CL, Source, RHI::FTextureSlice{}, SwapImage, RHI::FTextureSlice{}, RHI::EFilter::Linear);
                }
            }
            #endif

            RHI::CmdEndMarker(CL);

            {
                LUMINA_PROFILE_SECTION_COLORED("Present", tracy::Color::Orange4);
                // A secondary viewport's present waits this one out, so only a frame with tool windows torn off serializes.
                SwapchainTarget.PresentAsync(CL);
            }

            #if WITH_EDITOR
            {
                // Renders and presents each dragged-out tool window into its own swapchain.
                LUMINA_PROFILE_SECTION_COLORED("ImGui Secondary Viewports", tracy::Color::SlateBlue4);
                ImGuiRenderer->RenderSecondaryViewports();
            }
            #endif
        }
    }

    void FRenderManager::SwapchainResized(FVector2 NewSize)
    {
        OnSwapchainResized.Broadcast(NewSize);
    }

    void FRenderManager::RecreatePrimarySwapchain()
    {
        SwapchainTarget.Recreate();
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
