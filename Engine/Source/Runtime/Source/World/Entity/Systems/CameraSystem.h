#pragma once

#include "World/ECS/Registry.h"

#include "EntitySystem.h"
#include "World/Entity/Components/CameraComponent.h"
#include "CameraSystem.generated.h"

namespace Lumina
{
    // Authoring parameters for a camera shake (see SCameraSystem::PlayCameraShake). The shake is additive on
    // the rendered view, so it composes with the cinematic blend and never moves the camera entity.
    REFLECT()
    struct RUNTIME_API FCameraShakeParams
    {
        GENERATED_BODY()

        PROPERTY()
        FVector3 LocationAmplitude = FVector3(0.0f);  // max local-space positional offset per axis (world units)

        PROPERTY()
        FVector3 RotationAmplitude = FVector3(0.0f);  // max rotation per axis in degrees (X pitch, Y yaw, Z roll)

        PROPERTY()
        float    Frequency         = 10.0f;           // oscillation rate (Hz)

        PROPERTY()
        float    Duration          = 0.5f;            // seconds; <= 0 loops until explicitly stopped

        PROPERTY()
        float    BlendInTime       = 0.05f;

        PROPERTY()
        float    BlendOutTime      = 0.2f;
    };

    // Resolves the active view: bakes the camera matrix + blends post-process volumes into the
    // FResolvedSceneView singleton that CWorld::Extract forwards. Runs last (Low) so it sees every change.
    // Also owns active-camera selection + the cinematic blend, stored in the FCameraGlobalState singleton.
    REFLECT()
    class SCameraSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnStartup() override;
        void OnUpdate() override;
        void OnTeardown() override;

        /** Restamps the active camera's ViewVolume from its live transform, advancing no blend or shake clock. */
        RUNTIME_API static void ResolveActiveCameraView(ECS::FRegistry& Registry);

        //~ Active-camera management on the FCameraGlobalState singleton.

        /** Switch the active camera. BlendTime > 0 eases from the current view; 0 snaps. */
        static void SetActiveCamera(ECS::FRegistry& Registry, ECS::FEntity Entity, float BlendTime = 0.0f, ECameraBlendFunction Function = ECameraBlendFunction::EaseInOut);
        static ECS::FEntity GetActiveCameraEntity(ECS::FRegistry& Registry);

        // A fade and letterbox laid over the live camera, which zero clears.
        RUNTIME_API static void SetCinematicOverlay(ECS::FRegistry& Registry, float Fade, FVector3 FadeColor, float LetterboxAspect);
        static SCameraComponent* GetActiveCamera(ECS::FRegistry& Registry);

        //~ Camera shake on the FCameraGlobalState singleton. Additive on the rendered view; multiple shakes sum.

        /** Start a shake; returns a non-zero handle to stop it (0 on failure). */
        static uint32 PlayCameraShake(ECS::FRegistry& Registry, const FCameraShakeParams& Params);
        /** Stop one shake by handle; it fades out over its BlendOutTime rather than cutting abruptly. */
        static void StopCameraShake(ECS::FRegistry& Registry, uint32 Handle);
        /** Immediately clear every active shake. */
        static void StopAllCameraShakes(ECS::FRegistry& Registry);
    };
}
