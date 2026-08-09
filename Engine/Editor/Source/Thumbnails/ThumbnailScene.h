#pragma once

#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"
#include "World/World.h"

namespace Lumina
{
    struct FPackageThumbnail;
}

namespace Lumina
{
    // Transient one-shot world used to render asset thumbnails.
    class FThumbnailScene
    {
    public:

        explicit FThumbnailScene(uint32 RenderTargetSize = 512);
        ~FThumbnailScene();

        LE_NO_COPYMOVE(FThumbnailScene);

        // Constructs the world + renderer at the requested square size and adds a default camera. Call before populating.
        void Begin();

        // Tears down the world. Safe to call repeatedly; called by destructor.
        void End();

        // Constructs an entity and tracks it so ResetContents can remove it. Renderers must spawn
        // their preview entities through this (not World->ConstructEntity) so the scene can be reused.
        entt::entity SpawnEntity(FName Name);

        // Destroys every entity spawned via SpawnEntity, leaving the camera intact, so one long-lived
        // scene can be repopulated for the next asset instead of rebuilding the whole world each time.
        void ResetContents();

        CWorld* GetWorld() const { return World; }
        entt::entity GetCameraEntity() const { return CameraEntity; }

        // Place the thumbnail camera. Recomputes the view matrix immediately
        // so render results are deterministic without ticking the world.
        void SetCameraTransform(const FVector3& Position, const FVector3& Target, float FOVDegrees = 35.0f);

        // Renders one frame and reads back into Thumbnail (256x256 RGBA8). BLOCKS until the GPU copy
        // completes. Kept for the save path, which cannot write the package without the bytes.
        bool Capture(FPackageThumbnail& Thumbnail);

        // Non-blocking capture. BeginCapture records the render + readback, submits, and returns
        // immediately; poll IsCaptureReady on later frames and then FinishCapture. This is what keeps the
        // content browser off a synchronous GPU round-trip per thumbnail.
        //
        // At most ONE capture may be in flight: the scene is reused, so starting another render would
        // overwrite the render target the pending copy is still reading from.
        bool BeginCapture();
        bool HasPendingCapture() const { return Pending != nullptr; }
        bool IsCaptureReady() const;
        bool FinishCapture(FPackageThumbnail& Thumbnail);

        // Block until the pending capture lands, then finish it. For callers that must not lose it.
        bool WaitAndFinishCapture(FPackageThumbnail& Thumbnail);

        // Drop a pending capture. Still waits for the GPU first -- the readback buffer cannot be freed
        // while a submitted copy may still write to it.
        void AbandonCapture();

    private:

        // GPU-side state of an in-flight capture; RHI types stay out of this header.
        struct FPendingCapture;

        // Renders one frame and submits the readback copy WITHOUT waiting. Fills Out with the buffer and
        // the timeline value that signals its completion.
        bool RecordCapture(FPendingCapture& Out);

        // Maps the finished readback into Thumbnail and frees it. Caller must have waited on the value.
        bool ResolveCapture(FPendingCapture& In, FPackageThumbnail& Thumbnail);

        TObjectPtr<CWorld>          World;
        entt::entity                CameraEntity = entt::null;
        TVector<entt::entity>       SpawnedEntities;
        TUniquePtr<FPendingCapture> Pending;
        uint32                      RTSize       = 512;
        bool                        bInitialized = false;
    };
}
