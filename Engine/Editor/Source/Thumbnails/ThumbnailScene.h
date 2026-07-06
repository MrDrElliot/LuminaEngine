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

        // Renders one frame and reads back into Thumbnail (256x256 RGBA8).
        // Returns false if the world or render target is missing.
        bool Capture(FPackageThumbnail& Thumbnail);

    private:

        TObjectPtr<CWorld>        World;
        entt::entity              CameraEntity = entt::null;
        TVector<entt::entity>     SpawnedEntities;
        uint32                    RTSize       = 512;
        bool                      bInitialized = false;
    };
}
