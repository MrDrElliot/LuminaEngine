#pragma once
#include "MaterialManager.h"
#include "RenderRelease.h"
#include "RHI.h"
#include "RHITexture.h"
#include "RenderResource.h"
#include "SwapchainTarget.h"
#include "Core/Delegates/Delegate.h"
#include "Memory/SmartPtr.h"
#include "Containers/HashTable.h"
#include "Core/Threading/Thread.h"
#include "Renderer/ShaderHandle.h"
#include "Core/Threading/Sync.h"
#include "Assets/AssetTypes/Material/MaterialCompileQueue.h"
#include "Assets/AssetTypes/ParticleSystem/ParticleCompileQueue.h"
#include <atomic>

namespace Lumina
{
    class CMesh;
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

    // Builds one pipeline from its own copy of the shader code, so it can finish after the shader entries are gone.
    using FPipelineBuild = TMoveOnlyFunction<RHI::FPipelineH()>;

    // A pipeline depends only on its shaders and raster state, so every scene renderer shares one set and a new world reuses them.
    struct FScenePipelineCache
    {
        struct FEntry
        {
            RHI::FPipelineH Pipeline;
            FShaderH        Shaders[3];
        };

        FSharedMutex             Mutex;
        THashMap<uint64, FEntry> Entries;

        RUNTIME_API RHI::FPipelineH Find(uint64 Hash);

        // True while a build for Hash is queued or running, so a caller can skip copying the shader code again.
        RUNTIME_API bool IsPending(uint64 Hash);

        // Queues Build on a worker unless one is already queued or done, and returns the pipeline only if it is ready.
        RUNTIME_API RHI::FPipelineH Request(uint64 Hash, const FShaderH (&Shaders)[3], FPipelineBuild&& Build);

        // Returns the pipeline, building it on this thread unless another thread is already doing so.
        RUNTIME_API RHI::FPipelineH BuildNow(uint64 Hash, const FShaderH (&Shaders)[3], FPipelineBuild&& Build);

        // Retires every pipeline built from a shader that no longer resolves, which a recompile leaves behind.
        RUNTIME_API void PurgeStale();
        void ReleaseAll();

    private:
        struct FPendingBuild;
        struct FPumpJob;

        TSharedPtr<FPendingBuild> AddPending(uint64 Hash, const FShaderH (&Shaders)[3], FPipelineBuild&& Build, bool bQueued);
        RHI::FPipelineH RunBuild(uint64 Hash, FPendingBuild& Pending);
        void Pump();
        void RunQueued(FPendingBuild& Pending);
        void WaitForBuilds();

        // Guarded by Mutex.
        THashMap<uint64, TSharedPtr<FPendingBuild>> PendingBuilds;

        FMutex                                QueueMutex;
        TVector<TSharedPtr<FPendingBuild>>    Queue;
        uint32                                InFlight = 0;
        std::atomic<uint32>                   BuiltSinceSave{ 0 };
    };

    class FRenderManager
    {
    public:

        static TMulticastDelegate<void, FVector2> OnSwapchainResized;

        FRenderManager();
        ~FRenderManager();
        FRenderManager(const FRenderManager&) = delete;
        FRenderManager& operator=(const FRenderManager&) = delete;
        FRenderManager(FRenderManager&&) = delete;
        FRenderManager& operator=(FRenderManager&&) = delete;

        void Initialize();

        // Called before input is pumped, so this longest wait in the frame does not count as input latency.
        void WaitForFrameSlot();

        // Called right after the event pump, as the start of the input latency plots.
        void MarkInputRead();

        // ImGui::NewFrame (and any other backend per-frame init).
        void FrameStart(const FUpdateContext& UpdateContext);

        void FrameEnd();

        void SwapchainResized(FVector2 NewSize);

        // Rebuild the primary swapchain (vsync / present-mode change).
        RUNTIME_API void RecreatePrimarySwapchain();

        // The whole composited main window, editor UI included, as tightly packed BGRA8 with the top row first.
        struct FWindowCapture
        {
            TVector<uint8> Pixels;
            uint32         Width  = 0;
            uint32         Height = 0;
        };

        // Arms a copy of the next frame the main window presents, and returns the ticket that collects it.
        RUNTIME_API uint64 RequestWindowCapture();

        // True once the frame the ticket armed has finished on the GPU, moving its pixels into Out.
        RUNTIME_API bool TakeWindowCapture(uint64 Ticket, FWindowCapture& Out);

        #if USING(WITH_DEV_UI)
        IImGuiRenderer* GetImGuiRenderer() const { return ImGuiRenderer; }
        #endif

        #if WITH_EDITOR
        NODISCARD FMaterialCompileQueue& GetMaterialCompileQueue() { return MaterialCompileQueue; }
        NODISCARD FParticleCompileQueue& GetParticleCompileQueue() { return ParticleCompileQueue; }
        #endif

        uint32 GetCurrentFrameIndex() const { return CurrentFrameIndex; }

        NODISCARD RHI::FMaterialManager& GetMaterialManager() const { return *MaterialManager.get(); }
        NODISCARD RHI::FMaterialCollectionManager& GetCollectionManager() const { return *CollectionManager.get(); }

        // Deferred release of renderer-side state whose owner has gone away.
        NODISCARD RHI::FRenderReleaseQueue& GetReleaseQueue() { return ReleaseQueue; }

        // Polls the mesh's staged resource every frame, since an upload batch landing marks nothing dirty. Any thread.
        RUNTIME_API void WatchMeshPublish(CMesh* Mesh);

        // Lazily populated by the first render scene; aliased by all later scenes.
        NODISCARD FSharedRenderResources& GetSharedRenderResources() { return SharedRenderResources; }
        NODISCARD FScenePipelineCache& GetScenePipelineCache() { return ScenePipelineCache; }

    private:
        
        void OnWindowResized(FWindow* Window, const FUIntVector2& Extent);

        void ApplyPendingResize();

        void RecordWindowCapture(RHI::FCmdListH CL, RHI::FTextureH SwapImage, const FUIntVector2& Extent, uint8 FrameIndex);

        // Runs once the slot's fence has been waited, which is when its copy is complete.
        void ResolveWindowCapture(uint8 FrameIndex);

        // Runs once the slot's fence has been waited, which is when its frame finished on the GPU.
        void PlotInputToGPUComplete(uint8 FrameIndex);

        void PublishPendingMeshes();

        #if USING(WITH_DEV_UI)
        IImGuiRenderer*                     ImGuiRenderer = nullptr;
        #endif

        #if WITH_EDITOR
        FMaterialCompileQueue               MaterialCompileQueue;
        FParticleCompileQueue               ParticleCompileQueue;
        #endif

        TUniquePtr<RHI::FMaterialManager>   MaterialManager;
        TUniquePtr<RHI::FMaterialCollectionManager> CollectionManager;

        RHI::FRenderReleaseQueue            ReleaseQueue;

        // Guarded by MeshPublishMutex, since a reimport stages its mesh from a worker.
        FMutex                              MeshPublishMutex;
        TVector<TWeakObjectPtr<CMesh>>      MeshesAwaitingPublish;

        // Backing storage for GShaderLibrary / GShaderCompiler.
        FShaderLibrary*                     ShaderLibrary = nullptr;
        FSpirVShaderCompiler*               ShaderCompiler = nullptr;

        FSharedRenderResources              SharedRenderResources;
        FScenePipelineCache                 ScenePipelineCache;

        // New RHI owns presentation: the primary window swapchain.
        RHI::FSwapchainTarget               SwapchainTarget;

        FDelegateHandle                     WindowResizedHandle;

        std::atomic<uint64>                 PendingResizeExtent = 0;

        // The size the scene views were last told, which a rebuild for another reason never updates.
        FUIntVector2                        BroadcastExtent = FUIntVector2(0, 0);

        uint8                               CurrentFrameIndex = 0;

        // Set by WaitForFrameSlot, cleared by the FrameEnd that consumes it.
        bool                                bFrameSlotWaited = false;

        // Zero means no read to measure, as in a headless or minimized frame.
        double                              InputReadSeconds = 0.0;
        // The read of the frame each slot last presented, cleared once its fence is waited.
        double                              SlotInputReadSeconds[RHI::kFramesInFlight] = {};

        struct FInFlightWindowCapture
        {
            uint64              Ticket     = 0;
            RHI::FGPUAllocation Readback   = {};
            FUIntVector2        Extent     = FUIntVector2(0, 0);
            uint8               FrameIndex = 0;
        };

        FMutex                              WindowCaptureMutex;
        uint64                              NextWindowCaptureTicket = 1;
        // Armed but not yet recorded into a frame.
        uint64                              RequestedWindowCapture  = 0;
        FInFlightWindowCapture              InFlightWindowCapture;
        uint64                              CompletedWindowCaptureTicket = 0;
        FWindowCapture                      CompletedWindowCapture;
    };
    
    NODISCARD RUNTIME_API FRenderManager& Render();
    NODISCARD RUNTIME_API FRenderManager* TryRender();

    namespace Internal
    {
        // FEngine owns the renderer's lifetime, so nothing else should call this.
        RUNTIME_API void SetRenderManager(FRenderManager* Manager);
    }
}
