#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Memory/SmartPtr.h"
#include "RagdollComponent.generated.h"

namespace Lumina
{
    class CPhysicsAsset;
    struct FPhysicsRagdollHandle;

    /** How the ragdoll system treats the skeletal mesh. */
    REFLECT()
    enum class ERagdollState : uint8
    {
        Inactive,    // No bodies in the scene; pose stays fully animation-driven.
        Simulated,   // Limp physics; the ragdoll bodies drive the bone pose.
        Powered,     // Physics with muscles that pull every limb toward the animated or procedural pose.
    };

    // What holds a powered ragdoll's root body in place.
    REFLECT()
    enum class ERagdollRootDrive : uint8
    {
        Free,           // Nothing but physics, or a locomotion component's balance, holds the root.
        FollowEntity,   // The root is pulled toward the animated root under the entity, for hit reactions on a moving character.
    };

    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SRagdollComponent
    {
        GENERATED_BODY()

        SRagdollComponent();
        ~SRagdollComponent();
        SRagdollComponent(const SRagdollComponent&);
        SRagdollComponent& operator=(const SRagdollComponent&);
        SRagdollComponent(SRagdollComponent&&) noexcept;
        SRagdollComponent& operator=(SRagdollComponent&&) noexcept;

        /** Authored bodies/constraints. If null, the system auto-generates one from the mesh's skeleton. */
        PROPERTY(Editable, Category = "Ragdoll")
        TStrongObjectPtr<CPhysicsAsset> PhysicsAsset;

        /** Simulated collapses into a limp ragdoll and Powered keeps it standing on its muscles; the system builds and tears down bodies to match. */
        PROPERTY(Editable, Category = "Ragdoll")
        ERagdollState State = ERagdollState::Inactive;

        /** While simulating, move the entity transform to follow the ragdoll's root body each frame. Keep
         *  on so the mesh's culling bounds track the ragdoll; off leaves the entity at its spawn transform. */
        PROPERTY(Editable, Category = "Ragdoll")
        bool bDriveEntityFromRoot = true;

        PROPERTY(Editable, Category = "Ragdoll|Muscles", ClampMin = 0.0f, ClampMax = 4.0f, ToolTip = "Scales every muscle's torque. Near zero goes limp, above one stiffens like a guard pose.")
        float MuscleStrength = 1.0f;

        PROPERTY(Editable, Category = "Ragdoll|Muscles", ClampMin = 0.0f, ClampMax = 60.0f, Units = "Hz", ToolTip = "How quickly a limb springs back to its target. Joints can override it in the physics asset.")
        float MuscleStiffness = 14.0f;

        PROPERTY(Editable, Category = "Ragdoll|Muscles", ClampMin = 0.0f, ClampMax = 4.0f, ToolTip = "Muscle damping ratio, one settles without overshoot.")
        float MuscleDamping = 1.0f;

        PROPERTY(Editable, Category = "Ragdoll|Muscles", ClampMin = 0.0f, ClampMax = 5000.0f, Units = "N m", ToolTip = "Torque the strongest joint can exert at full strength. Lighter limbs get a share by the mass they carry.")
        float MaxMuscleTorque = 400.0f;

        PROPERTY(Editable, Category = "Ragdoll|Muscles")
        ERagdollRootDrive RootDrive = ERagdollRootDrive::Free;

        PROPERTY(Editable, Category = "Ragdoll|Muscles", ClampMin = 0.0f, ClampMax = 10.0f, EditCondition = "RootDrive == FollowEntity", ToolTip = "Force the root may use to follow the entity, as a multiple of the ragdoll's weight.")
        float RootFollowStrength = 2.0f;

        /** Live ragdoll, created on demand by the ragdoll system. */
        TSharedPtr<FPhysicsRagdollHandle> Ragdoll;

        /** State the system last realized; lets it detect transitions into and out of physics. */
        ERagdollState RealizedState = ERagdollState::Inactive;

        // Component-space bone transforms the muscles chase, taken from each fresh animation pose or the bind pose.
        TVector<FMatrix4> TargetGlobals;
        uint32 CapturedPoseSerial = 0;
    };
}
