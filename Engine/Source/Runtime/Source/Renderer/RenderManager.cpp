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

    FRenderManager* TryRender()
    {
        return GRenderManager;
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
                SwapchainTarget.Present(CL);
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
