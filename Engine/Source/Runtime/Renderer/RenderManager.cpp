#include "pch.h"
#include "RenderManager.h"

#include "Tools/UI/ImGui/Vulkan/VulkanImGuiRender.h"

#include "RenderThread.h"
#include "ShaderCompiler.h"
#include "ShaderLibrary.h"
#include "RHI.h"
#include "RHICore.h"
#include "Core/Application/Application.h"
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
        // Render thread recreates the swapchain with the new present mode.
        const bool bEnabled = eastl::get<bool>(Value);
        if (GRenderManager != nullptr)
        {
            ENQUEUE_RENDER_COMMAND(SetVSync)([bEnabled]
            {
                RHI::SetVSync(bEnabled);
                GRenderManager->RecreatePrimarySwapchain();
            });
        }
        else
        {
            RHI::SetVSync(bEnabled);
        }
    });

    FRenderManager::FRenderManager()
    {
    }

    FRenderManager::~FRenderManager()
    {
        // Detach from resize events before teardown so a late resize can't enqueue work.
        FWindow::OnWindowResized.Remove(WindowResizedHandle);

        // Stop the worker first: queued commands hold refs to GPU resources destroyed below.
        if (GRenderThread)
        {
            GRenderThread->Stop();
            Memory::Delete(GRenderThread);
            GRenderThread = nullptr;
        }

        #if WITH_EDITOR
        ImGuiRenderer->Deinitialize();
        Memory::Delete(ImGuiRenderer);
        ImGuiRenderer = nullptr;
        #endif

        MaterialManager = nullptr;

        // Release the shared LUT / icon heap slots while the device is still alive; member
        // teardown would otherwise run after the device is freed below.
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
        constexpr bool bValidation = true;
#else
        constexpr bool bValidation = false;
#endif

#if LUMINA_SHIPPING
        constexpr bool bDebugUtils = false;
#else
        constexpr bool bDebugUtils = true;
#endif

        RHI::CreateDevice(RHI::FDeviceDesc{ bValidation, bDebugUtils });
        RHI::Core::Initialize();

        ShaderLibrary = Memory::New<FShaderLibrary>();
        GShaderLibrary = ShaderLibrary;
        ShaderCompiler = Memory::New<FSpirVShaderCompiler>();
        GShaderCompiler = ShaderCompiler;
        ShaderCompiler->Initialize();

        FWindow* Window = Windowing::GetPrimaryWindowHandle();
        Swapchain = RHI::CreateSwapchain(Window->GetWindow(), Window->GetExtent());

        WindowResizedHandle = FWindow::OnWindowResized.AddMember(this, &FRenderManager::OnWindowResized);

        GRenderThread = Memory::New<FRenderThread>();
        GRenderThread->Start();

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

        [[maybe_unused]] FImDrawDataSnapshot* ImGuiSnapshot = nullptr;
        #if WITH_EDITOR
        ImGuiSnapshot = ImGuiRenderer->BuildFrame_GameThread(ThisFrameIndex);
        #endif

        ENQUEUE_RENDER_COMMAND(RenderFrame)([this, ThisFrameIndex, Snapshot = ImGuiSnapshot]() mutable
        {
            {
                LUMINA_PROFILE_SECTION_COLORED("RT Frame Fence (GPU)", tracy::Color::Crimson);
                RHI::Core::BeginFrame(ThisFrameIndex);
            }
            
            GWorldManager->RenderWorlds(ThisFrameIndex);

            RHI::FTextureH SwapImage;
            {
                LUMINA_PROFILE_SECTION_COLORED("RT Acquire Swapchain", tracy::Color::Orange3);
                SwapImage = RHI::AcquireNextImage(Swapchain);
            }
            if (!RHI::IsValid(SwapImage))
            {
                RHI::RecreateSwapchain(Swapchain, Windowing::GetPrimaryWindowHandle()->GetExtent());
                #if WITH_EDITOR
                if (Snapshot)
                {
                    ImGuiRenderer->SignalSnapshotSlotConsumed(ThisFrameIndex);
                }
                #endif
                return;
            }

            const FUIntVector2 Extent = RHI::GetSwapchainExtent(Swapchain);

            RHI::FCmdListH CL = RHI::OpenCommandList();
            RHI::CmdSetTextureHeap(CL, RHI::Core::GetGlobalHeap());
            RHI::CmdSwapchainBarrierToRender(CL, Swapchain);

            #if WITH_EDITOR
            {
                LUMINA_PROFILE_SECTION_COLORED("RT Editor UI", tracy::Color::SlateBlue1);
                RmlUi::RenderEditorContexts(CL);
            }
            #endif

            #if WITH_EDITOR
            if (Snapshot)
            {
                LUMINA_PROFILE_SECTION_COLORED("RT ImGui Record", tracy::Color::SlateBlue3);
                ImGuiRenderer->OnEndFrame_NewRHI(CL, SwapImage, Extent, *Snapshot);
            }
            #endif

            #if !WITH_EDITOR
            {
                LUMINA_PROFILE_SECTION_COLORED("RT Game Composite", tracy::Color::ForestGreen);

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
                LUMINA_PROFILE_SECTION_COLORED("RT Present", tracy::Color::Orange4);
                RHI::Core::Present(Swapchain, CL);
            }

            #if WITH_EDITOR
            if (Snapshot)
            {
                // Multi-viewport: render + present each dragged-out tool window into its own swapchain.
                // Must finish (it reads this frame slot's captures) before releasing the slot.
                LUMINA_PROFILE_SECTION_COLORED("RT ImGui Secondary Viewports", tracy::Color::SlateBlue4);
                ImGuiRenderer->RenderSecondaryViewports_RenderThread(ThisFrameIndex);
                ImGuiRenderer->SignalSnapshotSlotConsumed(ThisFrameIndex);
            }
            #endif
        });
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
        // Only the primary window owns the presented swapchain. A minimized / zero-area
        // window has no valid extent to build against.
        if (Window != Windowing::GetPrimaryWindowHandle() || Window->IsWindowMinimized())
        {
            return;
        }

        if (Extent.x == 0 || Extent.y == 0)
        {
            return;
        }

        // The swapchain lives on the render thread; rebuild it there. Coalesce duplicate
        // events by skipping when the extent already matches.
        const FVector2 NewSize = FVector2(Extent);
        ENQUEUE_RENDER_COMMAND(RecreateSwapchainOnResize)([this, Extent, NewSize]
        {
            const FUIntVector2 Current = RHI::GetSwapchainExtent(Swapchain);
            if (Current.x == Extent.x && Current.y == Extent.y)
            {
                return;
            }

            RHI::RecreateSwapchain(Swapchain, Extent);
            OnSwapchainResized.Broadcast(NewSize);
        });
    }
}
