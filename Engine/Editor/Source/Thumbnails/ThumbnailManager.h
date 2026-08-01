#pragma once
#include "Containers/Array.h"
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
        using FThumbnailRendererFn = TFunction<void(FThumbnailScene&, CObject* /*Asset*/)>;

        // Fills Out's RGBA8 image directly -- no world, no GPU, no readback. For assets whose thumbnail is a
        // DRAWING rather than a view of something (a curve, a gradient). Runs on the game thread with the
        // asset resident, exactly like a renderer. Returns false to decline (falls through to the renderer
        // path, then to the generic icon).
        using FThumbnailPainterFn = TFunction<bool(CObject* /*Asset*/, uint32 /*Size*/, FPackageThumbnail& /*Out*/)>;

        CThumbnailManager();
        ~CThumbnailManager();

        void Initialize();

        static CThumbnailManager& Get();

        // Returns the ready-to-display thumbnail for a package/asset path, or nullptr if it isn't ready yet.
        // A miss kicks off async resolve/generation in the background; callers show a generic icon meanwhile.
        FPackageThumbnail* GetThumbnailForPackage(const FName& Package);

        // Game-thread pump: render up to Budget queued thumbnails this frame, and apply any pending
        // invalidations. Call once per frame from the editor update.
        void ProcessRenderQueue(uint32 Budget = 1);

        // Drop the in-memory record for a path so the next request re-resolves it (used after save/re-import
        // so a refreshed thumbnail shows without a restart). Game thread.
        void InvalidateThumbnail(const FName& Package);

        void OnPackageDestroyed(FName Package);

        // Register a setup callback for an asset class; matched by walking up the class hierarchy, so subclasses inherit the renderer.
        void RegisterThumbnailRenderer(CClass* AssetClass, FThumbnailRendererFn Renderer);

        // Same hierarchy matching, for classes drawn rather than rendered. Checked BEFORE the renderer, so a
        // painter wins for a class that somehow has both.
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

        // A resolved asset that has a renderer but no cached/embedded thumbnail, so it must be rendered. The
        // thumbnail path NEVER loads the object itself -- loading a CObject off-thread races the editor's
        // loader on the same non-atomic object (torn FMeshResource -> crash). Instead the game-thread drain
        // renders it only once it is ALREADY resident + fully loaded (e.g. it's in the open level, or the user
        // opened it); non-resident requests are deferred and re-checked cheaply. DeferChecks bounds that wait.
        struct FRenderRequest
        {
            FName    Package;
            FGuid    GUID;
            uint64   ContentHash = 0;
            uint32   DeferChecks = 0;
        };

        // Worker task: sidecar cache -> legacy embedded block -> queue a render. Resolves where a thumbnail comes from.
        void ResolveThumbnailAsync(FName Package);

        // Create + upload an RHI texture from Source's RGBA image and mark Package's record ready. Thread-safe.
        void UploadAndStore(const FName& Package, const FPackageThumbnail& Source);

        // Store an already-final record state (Failed etc.) without an image.
        void SetRecordState(const FName& Package, FPackageThumbnail::EState State);

        // Walk AssetClass's hierarchy for a registered renderer; nullptr if none.
        FThumbnailRendererFn* FindRenderer(CClass* AssetClass);

        FThumbnailPainterFn* FindPainter(CClass* AssetClass);

        // True when this class can produce a thumbnail at all, by either route. The resolve path gates on
        // this before queueing, so a paintable class is not written off as Failed.
        bool CanGenerateFor(CClass* AssetClass);

        // Populate the persistent scene with Asset and capture into Out. Game thread only. False if there's
        // no renderer for Asset or the capture failed.
        bool RenderThumbnail(CObject* Asset, FPackageThumbnail& Out);

        // Drop cached records whose asset changed (content hash) or that failed before the asset existed.
        void SweepInvalidatedRecords();

        FSharedMutex ThumbnailLock;
        THashMap<FName, TUniquePtr<FThumbnailRecord>> Thumbnails;

        // Render requests produced by worker resolve tasks, drained by the game-thread ProcessRenderQueue
        // (which renders only resident assets and re-queues the rest). Guarded because workers push to it.
        FMutex                  RenderQueueMutex;
        TVector<FRenderRequest> RenderQueue;

        THashMap<CClass*, FThumbnailRendererFn> ThumbnailRenderers;
        THashMap<CClass*, FThumbnailPainterFn>  ThumbnailPainters;

        std::atomic<bool> bRegistryDirty{false};
    };
}
