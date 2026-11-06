#pragma once

#include "World/ECS/Registry.h"


#include "Containers/Vector.h"
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

        // Renderers spawn through this instead of World->ConstructEntity so ResetContents can remove them.
        ECS::FEntity SpawnEntity(FName Name);

        // Destroys every entity spawned via SpawnEntity and keeps the camera, so the scene can be reused.
        void ResetContents();

        CWorld* GetWorld() const { return World.Get(); }
        ECS::FEntity GetCameraEntity() const { return CameraEntity; }

        // Recomputes the view matrix immediately because the thumbnail world is never ticked.
        void SetCameraTransform(const FVector3& Position, const FVector3& Target, float FOVDegrees = 35.0f);

        // Waits on the GPU and stores RGBA8 downsampled from the render target to ThumbnailUtils::kThumbnailResolution.
        bool Capture(FPackageThumbnail& Thumbnail);

        // Returns at once and fails while a capture is pending, since a new render would overwrite the target it reads.
        bool BeginCapture();
        bool HasPendingCapture() const { return Pending != nullptr; }
        bool IsCaptureReady() const;
        bool FinishCapture(FPackageThumbnail& Thumbnail);

        // Block until the pending capture lands, then finish it. For callers that must not lose it.
        bool WaitAndFinishCapture(FPackageThumbnail& Thumbnail);

        // Still waits for the GPU, since a submitted copy may still write to the readback buffer.
        void AbandonCapture();

    private:

        // GPU-side state of an in-flight capture; RHI types stay out of this header.
        struct FPendingCapture;

        // Submits the readback copy without waiting and fills Out with the timeline value that signals it.
        bool RecordCapture(FPendingCapture& Out);

        // Maps the finished readback into Thumbnail and frees it. Caller must have waited on the value.
        bool ResolveCapture(FPendingCapture& In, FPackageThumbnail& Thumbnail);

        TStrongObjectPtr<CWorld>    World;
        ECS::FEntity                CameraEntity = ECS::NullEntity;
        TVector<ECS::FEntity>       SpawnedEntities;
        TUniquePtr<FPendingCapture> Pending;
        uint32                      RTSize       = 512;
        bool                        bInitialized = false;
    };
}
