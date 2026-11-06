#pragma once
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/Package/Thumbnail/PackageThumbnail.h"
#include "Core/Threading/Thread.h"
#include "GUID/GUID.h"
#include "Memory/SmartPtr.h"
#include "ThumbnailManager.generated.h"

namespace Lumina
{
    class CClass;
    class CPackage;
    class FThumbnailScene;
}

namespace Lumina
{
    REFLECT()
    class CThumbnailManager : public CObject
    {
        GENERATED_BODY()
    public:

        // Populates a freshly-spun-up FThumbnailScene with what the asset's thumbnail should show; camera is already created and active.
        using FThumbnailRendererFn = TFunction<void(FThumbnailScene&, CObject* Asset)>;

        // Draws a curve or gradient straight into Out with no world or GPU, and false falls through to the renderer.
        using FThumbnailPainterFn = TFunction<bool(CObject* Asset, uint32 Size, FPackageThumbnail& Out)>;

        CThumbnailManager();
        ~CThumbnailManager();

        void Initialize();

        static CThumbnailManager& Get();

        // Null until ready, and a miss starts an async resolve, so callers show a generic icon meanwhile.
        FPackageThumbnail* GetThumbnailForPackage(const FName& Package);

        // Renders up to Budget queued thumbnails and applies pending invalidations, once per editor frame.
        void ProcessRenderQueue(uint32 Budget = 1);

        // Game thread, so a saved or reimported asset shows its refreshed thumbnail on the next request.
        void InvalidateThumbnail(const FName& Package);

        void OnPackageDestroyed(FName Package);

        // Stages Asset in Scene the way its class's renderer frames it, for a caller running its own captures.
        bool StageThumbnail(FThumbnailScene& Scene, CObject* Asset);

        // A display texture for a capture, flipped upright, which the caller releases.
        static RHI::FManagedTexture CreateThumbnailTexture(const FPackageThumbnail& Thumbnail, const FString& DebugName);

        // Register a setup callback for an asset class; matched by walking up the class hierarchy, so subclasses inherit the renderer.
        void RegisterThumbnailRenderer(CClass* AssetClass, FThumbnailRendererFn Renderer);

        // Same hierarchy matching, and a painter wins over a renderer registered for the same class.
        void RegisterThumbnailPainter(CClass* AssetClass, FThumbnailPainterFn Painter);

        // Generate a fresh thumbnail for Asset into Package's slot; false if no renderer is registered (caller can fall back to viewport-grab).
        bool GenerateThumbnail(CObject* Asset, CPackage* Package);

    private:

        // One cached thumbnail plus the identity it was built from (for hash-based invalidation).
        struct FThumbnailRecord
        {
            FPackageThumbnail Thumbnail;
            FGuid             GUID;
            uint64            ContentHash = 0;
        };

        // Waits for something else to load the asset, since loading it here races the editor's loader into a torn mesh.
        struct FRenderRequest
        {
            FName    Package;
            FGuid    GUID;
            uint64   ContentHash = 0;
            uint32   DeferChecks = 0;
            bool     bLoadAttempted = false;

            // Keeps the asset alive from its load until its capture is read back.
            TStrongObjectPtr<CObject> Pin;
        };

        // Worker task that tries the sidecar cache, then the legacy embedded block, then queues a render.
        void ResolveThumbnailAsync(FName Package);

        // Create + upload an RHI texture from Source's RGBA image and mark Package's record ready. Thread-safe.
        void UploadAndStore(const FName& Package, const FPackageThumbnail& Source);

        // Store an already-final record state (Failed etc.) without an image.
        void SetRecordState(const FName& Package, FPackageThumbnail::EState State);

        // Caches and uploads a successful capture, or marks the record Failed.
        void CompleteRequest(const FRenderRequest& Request, bool bCaptured, FPackageThumbnail& Captured);

        // Walk AssetClass's hierarchy for a registered renderer; nullptr if none.
        FThumbnailRendererFn* FindRenderer(CClass* AssetClass);

        FThumbnailPainterFn* FindPainter(CClass* AssetClass);

        // Checks painters as well as renderers, so the resolve path does not write a paintable class off as Failed.
        bool CanGenerateFor(CClass* AssetClass);

        // Game thread only, and waits on the GPU because the save path needs the bytes before writing the package.
        bool RenderThumbnail(CObject* Asset, FPackageThumbnail& Out);

        // Lazily create + Begin the persistent scene. False if it could not be built.
        bool AcquireScene();

        // Submits without waiting, and a later ProcessRenderQueue collects the result once the GPU signals.
        bool BeginThumbnailRender(CObject* Asset);

        // Stores any in-flight browser capture so the scene can be repurposed without losing it.
        void FlushPendingCapture();

        // Drop cached records whose asset changed (content hash) or that failed before the asset existed.
        void SweepInvalidatedRecords();

        FSharedMutex ThumbnailLock;
        THashMap<FName, TUniquePtr<FThumbnailRecord>> Thumbnails;

        // Guarded because worker resolve tasks push to it while ProcessRenderQueue drains it.
        FMutex                  RenderQueueMutex;
        TVector<FRenderRequest> RenderQueue;

        THashMap<CClass*, FThumbnailRendererFn> ThumbnailRenderers;
        THashMap<CClass*, FThumbnailPainterFn>  ThumbnailPainters;

        // Building a scene idles the device and allocates about 40 render targets, so it is kept until a short idle.
        TUniquePtr<FThumbnailScene> PersistentScene;
        uint32                      SceneIdleFrames = 0;

        // At most one capture is in flight because there is one scene and one render target.
        FRenderRequest PendingRequest;
        bool           bHasPendingRequest = false;

        void ClearPendingRequest();

        std::atomic<bool> bRegistryDirty{false};
    };
}
