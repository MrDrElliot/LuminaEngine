#pragma once
#include "MaterialManager.h"
#include "RenderRelease.h"
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
        // Indices match the IconFiles and IconSlots tables in DefaultSceneRenderer.
        static constexpr int32  kEditorIconCount = 9;
        RHI::FManagedTexture    EditorIcons[kEditorIconCount];
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

        /** Blocks until the GPU has finished with the slot this frame is about to record into. Called at
         *  the top of the application loop, BEFORE input is pumped: everything between sampling the mouse
         *  and submitting is input latency, and this wait is the longest thing in that window. */
        void WaitForFrameSlot();

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

        /** Deferred release of renderer-side state whose owner has gone away. See RenderRelease.h for
            why this exists and what it gates on. */
        NODISCARD RHI::FRenderReleaseQueue& GetReleaseQueue() { return ReleaseQueue; }

        // Lazily populated by the first render scene; aliased by all later scenes.
        NODISCARD FSharedRenderResources& GetSharedRenderResources() { return SharedRenderResources; }

    private:
        
        void OnWindowResized(FWindow* Window, const FUIntVector2& Extent);

        void ApplyPendingResize();

        #if WITH_EDITOR
        IImGuiRenderer*                     ImGuiRenderer = nullptr;
        #endif

        TUniquePtr<RHI::FMaterialManager>   MaterialManager;

        RHI::FRenderReleaseQueue            ReleaseQueue;

        // Backing storage for GShaderLibrary / GShaderCompiler.
        FShaderLibrary*                     ShaderLibrary = nullptr;
        FSpirVShaderCompiler*               ShaderCompiler = nullptr;

        FSharedRenderResources              SharedRenderResources;

        // New RHI owns presentation: the primary window swapchain.
        RHI::FSwapchainH                    Swapchain;

        FDelegateHandle                     WindowResizedHandle;

        std::atomic<uint64>                 PendingResizeExtent = 0;

        uint8                               CurrentFrameIndex = 0;

        // Set by WaitForFrameSlot, cleared by the FrameEnd that consumes it.
        bool                                bFrameSlotWaited = false;
    };
    
    NODISCARD RUNTIME_API FRenderManager& Render();
    NODISCARD RUNTIME_API FRenderManager* TryRender();

    namespace Internal
    {
        /** Publishes the process-wide renderer. FEngine owns the lifetime; nothing else should call this. */
        RUNTIME_API void SetRenderManager(FRenderManager* Manager);
    }
}
