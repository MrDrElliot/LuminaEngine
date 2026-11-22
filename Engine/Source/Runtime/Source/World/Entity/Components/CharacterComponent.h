#pragma once

#include "Core/Delegates/ScriptDelegate.h"
#include "World/ECS/EventMeta.h"
#include "Core/Object/ObjectMacros.h"
#include "World/ECS/Entity.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Memory/SmartPtr.h"
#include "Physics/Physics.h"
#include "Physics/PhysicsTypes.h"
#include "CharacterComponent.generated.h"


namespace Lumina
{
    struct FPhysicsCharacterHandle;

    // Runtime ownership, so a copied component starts detached and the construct hook builds it a proxy.
    struct FCharacterHandleRef
    {
        TSharedPtr<FPhysicsCharacterHandle> Handle;

        FCharacterHandleRef() = default;
        ~FCharacterHandleRef() = default;
        FCharacterHandleRef(const FCharacterHandleRef&) {}
        FCharacterHandleRef(FCharacterHandleRef&&) noexcept = default;
        FCharacterHandleRef& operator=(const FCharacterHandleRef&) { Handle.reset(); return *this; }
        FCharacterHandleRef& operator=(FCharacterHandleRef&&) noexcept = default;
        FCharacterHandleRef& operator=(TSharedPtr<FPhysicsCharacterHandle>&& In) { Handle = Move(In); return *this; }

        explicit operator bool() const { return Handle != nullptr; }
        bool operator==(std::nullptr_t) const { return Handle == nullptr; }
        FPhysicsCharacterHandle* operator->() const { return Handle.get(); }
        FPhysicsCharacterHandle& operator*() const { return *Handle; }
        void reset() { Handle.reset(); }
    };
    
    struct SCharacterMovementComponent;

    REFLECT(Component, Category = "Character")
    struct RUNTIME_API SCharacterPhysicsComponent
    {
        GENERATED_BODY()

        SCharacterPhysicsComponent();
        ~SCharacterPhysicsComponent();
        SCharacterPhysicsComponent(const SCharacterPhysicsComponent&);
        SCharacterPhysicsComponent& operator=(const SCharacterPhysicsComponent&);
        SCharacterPhysicsComponent(SCharacterPhysicsComponent&&) noexcept;
        SCharacterPhysicsComponent& operator=(SCharacterPhysicsComponent&&) noexcept;
        
        FCharacterHandleRef Character;

        // The capsule physics uses right now, crouch included, so debug drawing and the mover never disagree about its size.
        void GetActiveCapsule(const SCharacterMovementComponent* Movement, float& OutHalfHeight, float& OutCenterDrop) const;

        // Snapshots for interpolation.
        FVector3 LastBodyPosition;
        FQuat LastBodyRotation;

        /** Layer and mask controlling which bodies this character collides with. */
        PROPERTY(Editable, Category = "Physics")
        SCollisionProfile CollisionProfile;

        /** Half-height of the capsule cylinder in meters, so total height is 2*(HalfHeight+Radius). */
        PROPERTY(Editable, Category = "Collision", Units = "m")
        float HalfHeight = 0.55f;

        /** Radius of the character capsule in meters. */
        PROPERTY(Editable, Category = "Collision", Units = "m")
        float Radius = 0.35f;

        /** Offset of the capsule from the entity origin, to seat the origin at the feet rather than the waist. */
        PROPERTY(Editable, Category = "Collision", Units = "m")
        FVector3 TranslationOffset = FVector3(0.0f);

        /** Mass of the character in kg, scaling how hard it shoves the dynamic bodies it walks into. */
        PROPERTY(Editable, Category = "Physics", Units = "kg")
        float Mass = 70.0f;

        /** Skin on the swept capsule, keeping the character a fraction clear of the surfaces it rests on. */
        PROPERTY(Editable, Category = "Physics", Units = "m")
        float Padding = 0.02f;

        /** Maximum push force the character can exert against dynamic bodies. */
        PROPERTY(Editable, Category = "Physics")
        float MaxStrength = 100.0f;

        /** Steepest surface angle (degrees) the character can walk up without sliding. */
        PROPERTY(Editable, Category = "Physics", Units = "deg")
        float MaxSlopeAngle = 45.0f;

        /** Maximum step height the character can automatically climb (meters). */
        PROPERTY(Editable, Category = "Physics", Units = "m")
        float StepHeight = 0.4f;

        /** How far the character may drop to stay on the floor after walking off a lip or down a step. */
        PROPERTY(Editable, Category = "Physics", Units = "m")
        float StickToFloorDistance = 0.5f;

        /** Depenetration passes per step. Higher pushes out of deeper overlap but costs a world query each. */
        PROPERTY(Editable, Category = "Physics")
        uint32 MaxCollisionIterations = 8;

        // When true, the capsule contributes to NavMesh bakes, which a path-follower must not do or it carves its own floor poly.
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = false;

        /** When false, the mover passes through the capsules of other characters instead of being blocked. */
        PROPERTY(Editable, Category = "Physics")
        bool bCollideWithCharacters = true;
    };

    // A character touched down, queued for systems and fanned out to its movement component's OnLanded.
    REFLECT(Event)
    struct RUNTIME_API SCharacterLandedEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;

        // How fast it was falling at touchdown in m/s, for landing effects and fall damage.
        PROPERTY(Hidden)
        float ImpactSpeed = 0.0f;

        PROPERTY(Hidden)
        FVector3 GroundNormal = FVector3(0.0f, 1.0f, 0.0f);

        // Null for world geometry that has no entity.
        PROPERTY(Hidden)
        ECS::FEntity GroundEntity = ECS::NullEntity;
    };

    // A character jumped, queued for systems and fanned out to its movement component's OnJumped.
    REFLECT(Event)
    struct RUNTIME_API SCharacterJumpedEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;

        // Which jump since the last landing, 1 for the jump off the ground and 2 or more for air jumps.
        PROPERTY(Hidden)
        int32 JumpCount = 0;

        // The velocity it leaves with, jump speed included.
        PROPERTY(Hidden)
        FVector3 Velocity = FVector3(0.0f);
    };

    // A character crouched or stood back up, queued for systems and fanned out to its movement component's OnCrouchChanged.
    REFLECT(Event)
    struct RUNTIME_API SCharacterCrouchEvent
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        ECS::FEntity Entity = ECS::NullEntity;

        // True when it crouched, false when it stood up.
        PROPERTY(Hidden)
        bool bCrouched = false;
    };

    REFLECT(Component, Category = "Character")
    struct RUNTIME_API SCharacterMovementComponent
    {
        GENERATED_BODY()
    
        /** Target horizontal movement speed (m/s). */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Movement", Units = "m/s")
        float MoveSpeed = 5.0f;

        // Speed while the controller holds SprintButtons, read inside the step so client and host sprint on the same move.
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Movement", Units = "m/s")
        float SprintSpeed = 0.0f;

        // Controller button bits that select SprintSpeed.
        PROPERTY(Editable, Category = "Movement")
        uint32 SprintButtons = 1;

        /** Rate at which the character accelerates to MoveSpeed (m/s²). */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Movement", Units = "m/s^2")
        float Acceleration = 10.0f;

        /** Rate at which the character decelerates when no input is applied (m/s²). */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Movement", Units = "m/s^2")
        float Deceleration = 8.0f;

        /** Fraction of ground acceleration available while airborne (0 = no air steering). */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Movement")
        float AirControl = 0.3f;

        /** Friction coefficient applied against horizontal velocity while grounded. */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Movement")
        float GroundFriction = 8.0f;

        /** Vertical impulse speed applied when the character jumps (m/s). */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Movement", Units = "m/s")
        float JumpSpeed = 8.0f;

        /** Degrees per second the character rotates to face its movement direction. */
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 1000.0f, Category = "Movement", Units = "deg/s")
        float RotationRate = 10.0f;

        /** Total number of jumps allowed before landing (1 = single jump, 2 = double jump, etc.). */
        PROPERTY(Editable, ClampMin = 0, Category = "Movement")
        int MaxJumpCount = 1;

        /** Downward gravity acceleration (m/s²). */
        PROPERTY(Editable, Category = "Gravity", Units = "m/s^2")
        float Gravity = Physics::GEarthGravity;

        /** Current velocity of the character in world space. */
        PROPERTY(Hidden, Category = "Movement", Units = "m/s")
        FVector3 Velocity;

        /** When true, the character's yaw matches the controller's look direction. */
        PROPERTY(Editable, Category = "Rotation")
        bool bUseControllerRotation = false;

        /** When true, the character rotates to face its movement direction each frame. */
        PROPERTY(Editable, Category = "Rotation")
        bool bOrientRotationToMovement = false;

        /** True when the character is standing on a surface. */
        PROPERTY(ReadOnly, Category = "Movement")
        bool bGrounded = false;

        /** Surface normal of the ground the character stands on (world space); +Y when airborne. Use for
            slope handling and aligning effects. */
        PROPERTY(ReadOnly, Category = "Movement")
        FVector3 GroundNormal = FVector3(0.0f, 1.0f, 0.0f);

        /** Entity the character is standing on, or the null entity (0xFFFFFFFF) when airborne / on static
            world geometry with no entity. Drives footstep-surface lookups and moving-platform logic. */
        PROPERTY(ReadOnly, Category = "Movement")
        ECS::FEntity GroundEntity = ECS::NullEntity;

        /** Number of jumps performed since last landing. */
        PROPERTY(ReadOnly, Category = "Movement")
        int JumpCount = 0;

        PROPERTY(Editable, Category = "Crouch")
        bool bCanCrouch = true;

        // Capsule cylinder half height while crouched in meters, measured like the physics component's HalfHeight.
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Crouch", Units = "m")
        float CrouchHalfHeight = 0.2f;

        // MoveSpeed and SprintSpeed are multiplied by this while crouched.
        PROPERTY(Editable, ClampMin = 0.0f, ClampMax = 1.0f, Category = "Crouch")
        float CrouchSpeedScale = 0.5f;

        PROPERTY(ReadOnly, Category = "Crouch")
        bool bIsCrouched = false;

        // Set by the controller's Crouch and UnCrouch, or directly by a script that drives the character itself.
        PROPERTY(Hidden)
        bool bWantsToCrouch = false;

        // Systems that hear every character read the SCharacterLandedEvent queue instead.
        PROPERTY(Hidden)
        TScriptDelegate<SCharacterLandedEvent> OnLanded;

        PROPERTY(Hidden)
        TScriptDelegate<SCharacterJumpedEvent> OnJumped;

        PROPERTY(Hidden)
        TScriptDelegate<SCharacterCrouchEvent> OnCrouchChanged;

        // Input latched from the controller once per frame in PrePhysics and consumed at the fixed step.
        FVector3 PendingMoveDirection = FVector3(0.0f);
        float     PendingLookYaw       = 0.0f;
        bool      bHasPendingMoveInput = false;
        bool      bPendingJump         = false;
        uint32    PendingButtons       = 0;

        // Input magnitude clamped to [0,1] that scales MoveSpeed this step.
        float     PendingMoveThrottle  = 0.0f;

        // Staged Launch (jump pad / knockback / dash) consumed in the physics step.
        FVector3  PendingLaunchVelocity     = FVector3(0.0f);
        bool      bPendingLaunch            = false;
        bool      bLaunchOverrideHorizontal = false;
        bool      bLaunchOverrideVertical   = false;

        // Staged teleport consumed in the physics step, since the mover owns the pose and overwrites a plain transform write.
        FVector3 PendingTeleportLocation = FVector3(0.0f);
        bool      bPendingTeleport        = false;
    };

    template<>
    struct TEventFanOut<SCharacterLandedEvent> : TEntityDelegateFanOut<SCharacterLandedEvent, &SCharacterMovementComponent::OnLanded> {};

    template<>
    struct TEventFanOut<SCharacterJumpedEvent> : TEntityDelegateFanOut<SCharacterJumpedEvent, &SCharacterMovementComponent::OnJumped> {};

    template<>
    struct TEventFanOut<SCharacterCrouchEvent> : TEntityDelegateFanOut<SCharacterCrouchEvent, &SCharacterMovementComponent::OnCrouchChanged> {};

    
}
