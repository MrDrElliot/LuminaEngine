#pragma once
#include "Core/Object/ObjectMacros.h"
#include "CameraFollowComponent.generated.h"

namespace Lumina
{
    // Makes a camera entity follow a target (SCameraRigSystem, after physics): eases toward Target + Offset,
    // optionally facing it. Lag is frame-rate independent (0 = snap). Pair with SCameraComponent; for a wall-aware boom use SSpringArmComponent.
    REFLECT(Component, Category = "Camera")
    struct RUNTIME_API SCameraFollowComponent
    {
        GENERATED_BODY()

        /** Entity the camera follows. Unset = component idles. */
        PROPERTY(Editable, Entity, Category = "Camera|Follow")
        uint32 Target = static_cast<uint32>(entt::to_integral(static_cast<entt::entity>(entt::null)));

        /** Offset from the target. Local to the target's orientation unless bWorldSpaceOffset. */
        PROPERTY(Editable, Category = "Camera|Follow")
        FVector3 Offset = FVector3(0.0f, 2.0f, -5.0f);

        /** When true, Offset is in world space; otherwise it rotates with the target. */
        PROPERTY(Editable, Category = "Camera|Follow")
        bool bWorldSpaceOffset = false;

        /** Position tracking responsiveness (1/seconds). Higher = snappier; 0 = instant. */
        PROPERTY(Editable, Category = "Camera|Follow", ClampMin = 0.0f)
        float PositionLagSpeed = 10.0f;

        /** When true, the camera rotates to face the target (+ LookAtOffset). */
        PROPERTY(Editable, Category = "Camera|Follow")
        bool bLookAtTarget = true;

        /** Rotation tracking responsiveness (1/seconds). Higher = snappier; 0 = instant. */
        PROPERTY(Editable, Category = "Camera|Follow", ClampMin = 0.0f)
        float RotationLagSpeed = 10.0f;

        /** World-space offset added to the look-at point (e.g. aim above the feet). */
        PROPERTY(Editable, Category = "Camera|Follow")
        FVector3 LookAtOffset = FVector3(0.0f, 1.0f, 0.0f);

        /** Set the followed entity and re-seat the camera on the next tick (no lag for the first frame). */
        FUNCTION(Script)
        void SetTarget(entt::entity Entity)
        {
            Target = static_cast<uint32>(entt::to_integral(Entity));
            bInitialized = false;
        }

        /** Clear the target; the camera holds its last pose. */
        FUNCTION(Script)
        void ClearTarget()
        {
            Target = static_cast<uint32>(entt::to_integral(static_cast<entt::entity>(entt::null)));
            bInitialized = false;
        }

        // Runtime smoothing state (non-reflected). bInitialized snaps on the first tick (or after
        // SetTarget) so the camera doesn't sweep in from a stale position.
        FVector3   CurrentPosition = FVector3(0.0f);
        FQuat   CurrentRotation = FQuat(1.0f, 0.0f, 0.0f, 0.0f);
        bool        bInitialized = false;
    };
}
