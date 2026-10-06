#pragma once

#include "Core/Object/FunctionLibrary.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Math/Vector/VectorTypes.h"
#include "World/ECS/Entity.h"
#include "World/Entity/Components/CameraComponent.h"
#include "World/Entity/Systems/CameraSystem.h"
#include "CameraLibrary.generated.h"

namespace Lumina
{
    class CWorld;

    // Reads of the resolved view and additive shake control, neither of which is the world's own business.
    REFLECT()
    class RUNTIME_API CCameraLibrary : public CFunctionLibrary
    {
        GENERATED_BODY()

    public:

        // Taken from the resolved view, so an active blend and any running shake are already folded in.
        FUNCTION()
        static FVector3 GetViewPosition(CWorld* World);

        FUNCTION()
        static FVector3 GetViewForward(CWorld* World);

        FUNCTION()
        static FVector3 GetViewRight(CWorld* World);

        FUNCTION()
        static FVector3 GetViewUp(CWorld* World);

        /** Vertical field of view in degrees, zero before anything has been rendered. */
        FUNCTION()
        static float GetFieldOfView(CWorld* World);

        FUNCTION()
        static ECS::FEntity GetActiveCameraEntity(CWorld* World);

        /** A BlendTime above zero runs the cinematic blend rather than cutting. */
        FUNCTION()
        static void SetActiveCamera(CWorld* World, ECS::FEntity Entity, float BlendTime = 0.0f,
            ECameraBlendFunction BlendFunction = ECameraBlendFunction::EaseInOut);

        /** Returns a handle to stop the shake with. Several shakes sum on the rendered view. */
        FUNCTION()
        static uint32 PlayShake(CWorld* World, FCameraShakeParams Params);

        /** An impact or explosion kick, where Intensity near one is a light bump and five a heavy blast. */
        FUNCTION()
        static uint32 PlayImpactShake(CWorld* World, float Intensity, float Duration = 0.4f);

        FUNCTION()
        static void StopShake(CWorld* World, uint32 ShakeHandle);

        FUNCTION()
        static void StopAllShakes(CWorld* World);

        /** Viewport size in pixels, or zero when the world has no renderer such as a dedicated server. */
        FUNCTION()
        static FVector2 GetViewportSize(CWorld* World);

        /** Projects a world point to pixel coordinates, origin top-left. Check bOnScreen before using it. */
        FUNCTION()
        static FScreenProjection WorldToScreen(CWorld* World, FVector3 WorldLocation);

        /** Pixel coordinates to a world ray, the aiming and picking primitive. */
        FUNCTION()
        static FWorldRay ScreenToWorldRay(CWorld* World, FVector2 ScreenPosition);

        /** ScreenToWorldRay through the center of the viewport, which is where a crosshair sits. */
        FUNCTION()
        static FWorldRay ViewportCenterRay(CWorld* World);

        /** A point WorldDistance along the ray through ScreenPosition, measured from the near plane. */
        FUNCTION()
        static FVector3 DeprojectScreenToWorld(CWorld* World, FVector2 ScreenPosition, float WorldDistance);

        // On screen in the active view, within MaxDistance, and with nothing solid in between. IgnoreA is the viewer's body, IgnoreB the target's.
        FUNCTION()
        static bool IsPointVisible(CWorld* World, FVector3 Point, ECS::FEntity IgnoreA, ECS::FEntity IgnoreB, float MaxDistance);

        // The same test from any eye, which is how a server judges what each player's camera sees.
        FUNCTION()
        static bool IsPointVisibleFrom(CWorld* World, FVector3 Eye, FVector3 Forward, float FovDegrees, float Aspect,
            FVector3 Point, ECS::FEntity IgnoreA, ECS::FEntity IgnoreB, float MaxDistance);
    };
}
