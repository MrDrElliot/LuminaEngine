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
        RHI::Core::Shutdown();
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
        });
        RenderBootMark("RHI::CreateDevice");
        RHI::Core::Initialize();
        RenderBootMark("RHI::Core::Initialize");

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
        RHI::Core::BeginFrame(CurrentFrameIndex);
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
                RHI::Core::BeginFrame(ThisFrameIndex);
            }
            bFrameSlotWaited = false;

            ApplyPendingResize();

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
            RHI::CmdSetTextureHeap(CL, RHI::Core::GetGlobalHeap());
            SwapchainTarget.BarrierToRender(CL);

            #if WITH_EDITOR
            {
                LUMINA_PROFILE_SECTION_COLORED("Editor UI", tracy::Color::SlateBlue1);
                RmlUi::RenderEditorContexts(CL);
            }
            #endif

            #if WITH_EDITOR
            {
                LUMINA_PROFILE_SECTION_COLORED("ImGui Record", tracy::Color::SlateBlue3);
                ImGuiRenderer->OnEndFrame_NewRHI(CL, SwapImage, Extent, ImGuiDrawData);
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
        if (Extent == SwapchainTarget.GetExtent())
        {
            return;
        }
        
        LUMINA_PROFILE_SCOPE();

        SwapchainTarget.Resize(Extent);
        OnSwapchainResized.Broadcast(FVector2(Extent));
    }
}
