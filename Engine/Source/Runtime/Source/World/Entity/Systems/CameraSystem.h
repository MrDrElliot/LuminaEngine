#pragma once
#include "EntitySystem.h"
#include "World/Entity/Components/CameraComponent.h"
#include "CameraSystem.generated.h"

namespace Lumina
{
    // Authoring parameters for a camera shake (see SCameraSystem::PlayCameraShake). The shake is additive on
    // the rendered view, so it composes with the cinematic blend and never moves the camera entity.
    struct FCameraShakeParams
    {
        FVector3 LocationAmplitude = FVector3(0.0f);  // max local-space positional offset per axis (world units)
        FVector3 RotationAmplitude = FVector3(0.0f);  // max rotation per axis in degrees (X pitch, Y yaw, Z roll)
        float    Frequency         = 10.0f;           // oscillation rate (Hz)
        float    Duration          = 0.5f;            // seconds; <= 0 loops until explicitly stopped
        float    BlendInTime       = 0.05f;
        float    BlendOutTime      = 0.2f;
    };

    // Resolves the active view: bakes the camera matrix + blends post-process volumes into the
    // FResolvedSceneView singleton that CWorld::Extract forwards. Runs last (Low) so it sees every change.
    // Also owns active-camera selection + the cinematic blend, stored in the FCameraGlobalState singleton.
    REFLECT(System)
    struct SCameraSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::FrameEnd, EUpdatePriority::Low),
                      RequiresUpdate(EUpdateStage::Paused, EUpdatePriority::Low))

        // Writes the resolved-view + camera-state singletons + the active camera component; reads transforms +
        // post-process volumes. Disjoint from animation/navmesh → overlaps them. Defined in the .cpp.
        static FSystemAccess Access;

        static void Startup(const FSystemContext& Context) noexcept;
        static void Update(const FSystemContext& Context) noexcept;
        static void Teardown(const FSystemContext& Context) noexcept;

        //~ Active-camera management on the FCameraGlobalState singleton.

        /** Switch the active camera. BlendTime > 0 eases from the current view; 0 snaps. */
        static void SetActiveCamera(entt::registry& Registry, entt::entity Entity, float BlendTime = 0.0f, ECameraBlendFunction Function = ECameraBlendFunction::EaseInOut);
        static entt::entity GetActiveCameraEntity(entt::registry& Registry);
        static SCameraComponent* GetActiveCamera(entt::registry& Registry);

        //~ Camera shake on the FCameraGlobalState singleton. Additive on the rendered view; multiple shakes sum.

        /** Start a shake; returns a non-zero handle to stop it (0 on failure). */
        static uint32 PlayCameraShake(entt::registry& Registry, const FCameraShakeParams& Params);
        /** Stop one shake by handle; it fades out over its BlendOutTime rather than cutting abruptly. */
        static void StopCameraShake(entt::registry& Registry, uint32 Handle);
        /** Immediately clear every active shake. */
        static void StopAllCameraShakes(entt::registry& Registry);
    };
}
