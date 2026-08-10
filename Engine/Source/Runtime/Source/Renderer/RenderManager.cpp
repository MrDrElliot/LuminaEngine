#include "RuntimePCH.h"
#include "RenderManager.h"

#include "Tools/UI/ImGui/Vulkan/VulkanImGuiRender.h"

#include "ShaderCompiler.h"
#include "ShaderLibrary.h"
#include "RHI.h"
#include "RHICore.h"
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

namespace Lumina
{
    TMulticastDelegate<void, FVector2> FRenderManager::OnSwapchainResized;
    RUNTIME_API FRenderManager* GRenderManager = nullptr;

    static TConsoleVar CVarVSync("Core.VSync", true, "Toggles v-sync", [](const CVarValueType& Value)
    {
        const bool bEnabled = eastl::get<bool>(Value);
        RHI::SetVSync(bEnabled);
        if (GRenderManager != nullptr)
        {
            GRenderManager->RecreatePrimarySwapchain();
        }
    });

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

        MaterialManager = nullptr;

        if (SharedRenderResources.bInitialized)
        {
            if (SharedRenderResources.BRDFLutUAV != RHI::kInvalidHeapSlot)
            {
                RHI::HeapFreeRWTexture(RHI::Core::GetGlobalHeap(), SharedRenderResources.BRDFLutUAV);
            }
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

        RHI::FreeH(Swapchain);
        RHI::Core::Shutdown();
        RHI::FreeDevice();
    }

    void FRenderManager::Initialize()
    {
        #if defined(LUMINA_WITH_VALIDATION)
        bool bValidation = true;
        #else
        bool bValidation = true;
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

        // Designated, NOT positional. FDeviceDesc's third field is bHeadless; a positional
        // `{ bValidation, bDebugUtils, bValidation }` quietly built a headless device whenever validation
        // was on, and headless skips the GLFW surface extensions and VK_KHR_swapchain -- which the two
        // lines below then need. GPU-AV is no longer a separate field; it follows bValidation inside
        // CreateDevice.
        RHI::CreateDevice(RHI::FDeviceDesc
        {
            .bValidation = bValidation,
            .bDebugUtils = bDebugUtils,
            .bHeadless   = false,
        });
        RHI::Core::Initialize();

        ShaderLibrary   = Memory::New<FShaderLibrary>();
        GShaderLibrary  = ShaderLibrary;
        ShaderCompiler  = Memory::New<FSpirVShaderCompiler>();
        GShaderCompiler = ShaderCompiler;
        ShaderCompiler->Initialize();

        FWindow* Window = Windowing::GetPrimaryWindowHandle();
        Swapchain = RHI::CreateSwapchain(RHI::CreateSurface(Window->GetWindow()), Window->GetExtent());

        WindowResizedHandle = FWindow::OnWindowResized.AddMember(this, &FRenderManager::OnWindowResized);

        MaterialManager = MakeUnique<RHI::FMaterialManager>();

#if WITH_EDITOR
        ImGuiRenderer = Memory::New<FVulkanImGuiRender>();
        ImGuiRenderer->Initialize();
#endif
        
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
            {
                LUMINA_PROFILE_SECTION_COLORED("Frame Fence (GPU)", tracy::Color::Crimson);
                RHI::Core::BeginFrame(ThisFrameIndex);
            }

            ApplyPendingResize();

            GWorldManager->RenderWorlds(ThisFrameIndex);

            RHI::FTextureH SwapImage;
            {
                LUMINA_PROFILE_SECTION_COLORED("Acquire Swapchain", tracy::Color::Orange3);
                SwapImage = RHI::AcquireNextImage(Swapchain);
            }
            if (!RHI::IsValid(SwapImage))
            {
                RHI::RecreateSwapchain(Swapchain, Windowing::GetPrimaryWindowHandle()->GetExtent());
                return;
            }

            const FUIntVector2 Extent = RHI::GetSwapchainExtent(Swapchain);

            RHI::FCmdListH CL = RHI::OpenCommandList();
            RHI::CmdSetTextureHeap(CL, RHI::Core::GetGlobalHeap());
            RHI::CmdSwapchainBarrierToRender(CL, Swapchain);

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
                RHI::Core::Present(Swapchain, CL);
            }

            #if WITH_EDITOR
            {
                // Multi-viewport: render + present each dragged-out tool window into its own swapchain.
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
        RHI::RecreateSwapchain(Swapchain, Windowing::GetPrimaryWindowHandle()->GetExtent());
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
        const FUIntVector2 Current = RHI::GetSwapchainExtent(Swapchain);
        if (Current.x == Extent.x && Current.y == Extent.y)
        {
            return;
        }

        RHI::RecreateSwapchain(Swapchain, Extent);
        OnSwapchainResized.Broadcast(FVector2(Extent));
    }
}
