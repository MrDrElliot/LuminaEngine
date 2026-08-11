#pragma once
#include "MaterialManager.h"
#include "RHI.h"
#include "RHITexture.h"
#include "RenderResource.h"
#include "Core/Delegates/Delegate.h"
#include "Memory/SmartPtr.h"

namespace Lumina
{
    class IImGuiRenderer;
    class FSpirVShaderCompiler;
    class FShaderLibrary;
    class FUpdateContext;
    class FWindow;
}

namespace Lumina
{
    struct FSharedRenderResources
    {
        RHI::FManagedTexture    BRDFLut;
        uint32                  BRDFLutUAV = RHI::kInvalidHeapSlot;
        RHI::FManagedTexture    SMAAArea;
        RHI::FManagedTexture    SMAASearch;

        #if WITH_EDITOR
        // PointLight, DirectionalLight, SkyLight, SpotLight, Camera, Character, ParticleSystem.
        RHI::FManagedTexture    EditorIcons[7];
        #endif

        bool            bInitialized = false;

        void Reset() { *this = FSharedRenderResources{}; }
    };

    class FRenderManager
    {
    public:

        static TMulticastDelegate<void, FVector2> OnSwapchainResized;

        FRenderManager();
        ~FRenderManager();

        void Initialize();

        // ImGui::NewFrame (and any other backend per-frame init).
        void FrameStart(const FUpdateContext& UpdateContext);

        void FrameEnd();

        void SwapchainResized(FVector2 NewSize);

        // Rebuild the primary swapchain (vsync / present-mode change).
        RUNTIME_API void RecreatePrimarySwapchain();

        #if WITH_EDITOR
        IImGuiRenderer* GetImGuiRenderer() const { return ImGuiRenderer; }
        #endif

        uint32 GetCurrentFrameIndex() const { return CurrentFrameIndex; }

        NODISCARD RHI::FMaterialManager& GetMaterialManager() const { return *MaterialManager.get(); }

        // Lazily populated by the first render scene; aliased by all later scenes.
        NODISCARD FSharedRenderResources& GetSharedRenderResources() { return SharedRenderResources; }

    private:
        
        void OnWindowResized(FWindow* Window, const FUIntVector2& Extent);

        void ApplyPendingResize();

        #if WITH_EDITOR
        IImGuiRenderer*                     ImGuiRenderer = nullptr;
        #endif

        TUniquePtr<RHI::FMaterialManager>   MaterialManager;

        // Backing storage for GShaderLibrary / GShaderCompiler.
        FShaderLibrary*                     ShaderLibrary = nullptr;
        FSpirVShaderCompiler*               ShaderCompiler = nullptr;

        FSharedRenderResources              SharedRenderResources;

        // New RHI owns presentation: the primary window swapchain.
        RHI::FSwapchainH                    Swapchain;

        FDelegateHandle                     WindowResizedHandle;

        std::atomic<uint64>                 PendingResizeExtent = 0;

        uint8                               CurrentFrameIndex = 0;
    };
    
    NODISCARD RUNTIME_API FRenderManager& Render();
    NODISCARD RUNTIME_API FRenderManager* TryRender();

    namespace Internal
    {
        /** Publishes the process-wide renderer. FEngine owns the lifetime; nothing else should call this. */
        RUNTIME_API void SetRenderManager(FRenderManager* Manager);
    }
}
