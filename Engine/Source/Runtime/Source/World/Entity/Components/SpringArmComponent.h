#pragma once
#include "Core/Object/ObjectMacros.h"
#include "SpringArmComponent.generated.h"

namespace Lumina
{
    /**
     * Third-person camera boom. Processed by SCameraRigSystem: it places the
     * camera entity a fixed distance behind a pivot (the Target entity, or the
     * camera's own position if unset), oriented by the control rotation. With
     * bDoCollisionTest the boom shortens via a physics sphere-cast so the camera
     * never clips through geometry between the pivot and the camera.
     *
     * Drive the look direction by rotating the camera entity (the control
     * rotation) - e.g. a mouse-look script writing yaw/pitch. The arm keeps the
     * camera behind the pivot along that rotation. Pair with SCameraComponent.
     */
    REFLECT(Component, Category = "Camera")
    struct RUNTIME_API SSpringArmComponent
    {
        GENERATED_BODY()

        /** Pivot the arm orbits. Unset = orbit the camera entity's own location. */
        PROPERTY(Editable, Entity, Category = "Camera|Spring Arm")
        uint32 Target = entt::to_integral(static_cast<entt::entity>(entt::null));

        /** World-space offset added to the pivot (e.g. raise the focus to head height). */
        PROPERTY(Editable, Category = "Camera|Spring Arm")
        FVector3 TargetOffset = FVector3(0.0f, 1.5f, 0.0f);

        /** Desired distance from the pivot to the camera along the arm (meters). */
        PROPERTY(Editable, Category = "Camera|Spring Arm", ClampMin = 0.0f)
        float TargetArmLength = 4.0f;

        /** Local-space offset applied at the camera end of the arm (e.g. over-the-shoulder X). */
        PROPERTY(Editable, Category = "Camera|Spring Arm")
        FVector3 SocketOffset = FVector3(0.0f);

        /** When true, the arm orientation follows the camera entity's rotation; otherwise the target's. */
        PROPERTY(Editable, Category = "Camera|Spring Arm")
        bool bUseControlRotation = true;

        /** When true, the arm shrinks via a sphere-cast to avoid clipping into geometry. */
        PROPERTY(Editable, Category = "Camera|Spring Arm")
        bool bDoCollisionTest = true;

        /** Radius of the sphere probe used for collision testing along the arm (meters). */
        PROPERTY(Editable, Category = "Camera|Spring Arm", ClampMin = 0.0f)
        float ProbeSize = 0.2f;

        /** Pivot tracking responsiveness (1/seconds). Higher = snappier; 0 = instant. */
        PROPERTY(Editable, Category = "Camera|Spring Arm", ClampMin = 0.0f)
        float PositionLagSpeed = 0.0f;

        /** Smoothing of the collision-shortened length (1/seconds). Higher = snappier; 0 = instant. */
        PROPERTY(Editable, Category = "Camera|Spring Arm", ClampMin = 0.0f)
        float LengthLagSpeed = 15.0f;

        /** Set the pivot entity and re-seat the camera on the next tick. */
        FUNCTION(Script)
        void SetTarget(entt::entity Entity)
        {
            Target = entt::to_integral(Entity);
            bInitialized = false;
        }

        // Runtime smoothing state (non-reflected). bInitialized snaps on the first
        // tick (or after SetTarget) so the boom doesn't sweep in from a stale pose.
        FVector3   CurrentPivot = FVector3(0.0f);
        float       CurrentArmLength = 4.0f;
        bool        bInitialized = false;
    };
}
