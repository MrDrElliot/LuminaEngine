#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Memory/SmartPtr.h"
#include "Physics/Physics.h"
#include "Physics/PhysicsTypes.h"
#include "CharacterComponent.generated.h"


namespace Lumina
{
    struct FJoltCharacterHandle;
    
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
        
        TSharedPtr<FJoltCharacterHandle> Character;

        // Snapshots for interpolation.
        FVector3 LastBodyPosition;
        FQuat LastBodyRotation;

        /** Layer and mask controlling which bodies this character collides with. */
        PROPERTY(Script, Editable, Category = "Physics")
        FCollisionProfile CollisionProfile;

        /** Half-height of the character capsule in meters. */
        PROPERTY(Script, Editable, Category = "Collision", Units = "m")
        float HalfHeight = 1.8f;

        /** Radius of the character capsule in meters. */
        PROPERTY(Script, Editable, Category = "Collision", Units = "m")
        float Radius = 1.0f;

        /** Mass of the character body in kg, affects how much it is pushed by dynamic bodies. */
        PROPERTY(Script, Editable, Category = "Physics", Units = "kg")
        float Mass = 70.0f;

        /** Small gap between the character shape and other surfaces to prevent tunneling. */
        PROPERTY(Script, Editable, Category = "Physics", Units = "m")
        float Padding = 0.02f;

        /** Cosine of the maximum angle between hit normals to be merged for reduction. */
        PROPERTY(Script, Editable, Category = "Physics")
        float HitReductionCosMaxAngle = 0.999f;

        /** Fraction of penetration resolved per step, higher values snap out of geometry faster. */
        PROPERTY(Script, Editable, Category = "Physics")
        float PenetrationRecoverySpeed = 1.0f;

        /** Distance ahead of the character to search for contacts and prevent clipping. */
        PROPERTY(Script, Editable, Category = "Physics", Units = "m")
        float PredictiveContactDistance = 0.1f;

        /** Maximum push force the character can exert against dynamic bodies. */
        PROPERTY(Script, Editable, Category = "Physics")
        float MaxStrength = 100.0f;

        /** Steepest surface angle (degrees) the character can walk up without sliding. */
        PROPERTY(Script, Editable, Category = "Physics", Units = "deg")
        float MaxSlopeAngle = 45.0f;

        /** Maximum step height the character can automatically climb (meters). */
        PROPERTY(Script, Editable, Category = "Physics", Units = "m")
        float StepHeight = 0.4f;

        /** Maximum collision resolution iterations per step. Higher = more accurate but slower. */
        PROPERTY(Script, Editable, Category = "Physics")
        uint32 MaxCollisionIterations = 5;

        /** Maximum constraint solver iterations per step. */
        PROPERTY(Script, Editable, Category = "Physics")
        uint32 MaxConstraintIterations = 15;

        /** Minimum time remaining in a step before the character stops moving. */
        PROPERTY(Script, Editable, Category = "Physics")
        float MinTimeRemaining = 1.0e-4f;

        /** Tolerance for merging overlapping contact points (meters). */
        PROPERTY(Script, Editable, Category = "Physics", Units = "m")
        float CollisionTolerance = 1.0e-3f;

        /** Maximum number of contact hits processed per step before culling. */
        PROPERTY(Script, Editable, Category = "Physics")
        uint32 MaxNumHits = 256;

        // When true, the capsule contributes to NavMesh bakes so agents path around it. Default false: a path-follower
        // that obstructs the navmesh carves out its own floor poly and can't query. On only for obstacle characters.
        PROPERTY(Script, Editable, Category = "Navigation")
        bool bAffectsNavigation = false;

        // Spend extra effort removing ghost collisions with internal edges of meshes, so the character glides
        // over tiled terrain / multi-triangle floors instead of catching on seams. Slightly more expensive.
        PROPERTY(Script, Editable, Category = "Physics")
        bool bEnhancedInternalEdgeRemoval = true;

        // When true, this character collides with OTHER characters as a character -- smooth mutual
        // pushing/sliding via Jolt's CharacterVsCharacterCollision, instead of merely being blocked by their
        // inner bodies. Note: because that check isn't thread-safe, enabling it on ANY character makes the
        // per-frame character update run serially, so very large crowds lose the parallel update. Set this
        // consistently across characters that should interact.
        PROPERTY(Script, Editable, Category = "Physics")
        bool bCollideWithCharacters = true;

        FUNCTION(Script)
        uint32 GetBodyID() const;

    };

    REFLECT(Component, Category = "Character")
    struct RUNTIME_API SCharacterMovementComponent
    {
        GENERATED_BODY()
    
        /** Target horizontal movement speed (m/s). */
        PROPERTY(Script, Editable, ClampMin = 0.0f, Category = "Movement", Units = "m/s")
        float MoveSpeed = 5.0f;

        /** Rate at which the character accelerates to MoveSpeed (m/s²). */
        PROPERTY(Script, Editable, ClampMin = 0.0f, Category = "Movement", Units = "m/s^2")
        float Acceleration = 10.0f;

        /** Rate at which the character decelerates when no input is applied (m/s²). */
        PROPERTY(Script, Editable, ClampMin = 0.0f, Category = "Movement", Units = "m/s^2")
        float Deceleration = 8.0f;

        /** Fraction of ground acceleration available while airborne (0 = no air steering). */
        PROPERTY(Script, Editable, ClampMin = 0.0f, Category = "Movement")
        float AirControl = 0.3f;

        /** Friction coefficient applied against horizontal velocity while grounded. */
        PROPERTY(Script, Editable, ClampMin = 0.0f, Category = "Movement")
        float GroundFriction = 8.0f;

        /** Vertical impulse speed applied when the character jumps (m/s). */
        PROPERTY(Script, Editable, ClampMin = 0.0f, Category = "Movement", Units = "m/s")
        float JumpSpeed = 8.0f;

        /** Degrees per second the character rotates to face its movement direction. */
        PROPERTY(Script, Editable, ClampMin = 0.0f, ClampMax = 1000.0f, Category = "Movement", Units = "deg/s")
        float RotationRate = 10.0f;

        /** Total number of jumps allowed before landing (1 = single jump, 2 = double jump, etc.). */
        PROPERTY(Script, Editable, ClampMin = 0, Category = "Movement")
        int MaxJumpCount = 1;

        /** Downward gravity acceleration (m/s²). */
        PROPERTY(Script, Editable, Category = "Gravity", Units = "m/s^2")
        float Gravity = Physics::GEarthGravity;

        /** Current velocity of the character in world space. */
        PROPERTY(Script, Visible, Category = "Movement", Units = "m/s")
        FVector3 Velocity;

        /** When true, the character's yaw matches the controller's look direction. */
        PROPERTY(Script, Editable, Category = "Rotation")
        bool bUseControllerRotation = false;

        /** When true, the character rotates to face its movement direction each frame. */
        PROPERTY(Script, Editable, Category = "Rotation")
        bool bOrientRotationToMovement = false;

        /** True when the character is standing on a surface. */
        PROPERTY(Script, ReadOnly, Category = "Movement")
        bool bGrounded = false;

        /** Surface normal of the ground the character stands on (world space); +Y when airborne. Use for
            slope handling and aligning effects. */
        PROPERTY(Script, ReadOnly, Category = "Movement")
        FVector3 GroundNormal = FVector3(0.0f, 1.0f, 0.0f);

        /** Entity the character is standing on, or the null entity (0xFFFFFFFF) when airborne / on static
            world geometry with no entity. Drives footstep-surface lookups and moving-platform logic. */
        PROPERTY(Script, ReadOnly, Category = "Movement")
        uint32 GroundEntity = 0xFFFFFFFF;

        /** Number of jumps performed since last landing. */
        PROPERTY(Script, ReadOnly, Category = "Movement")
        int JumpCount = 0;

        // Internal staging: input is latched from the controller once per
        // frame in PrePhysics, then consumed at fixed-step rate in physics.
        FVector3 PendingMoveDirection = FVector3(0.0f);
        float     PendingLookYaw       = 0.0f;
        bool      bHasPendingMoveInput = false;
        bool      bPendingJump         = false;

        // Input magnitude clamped to [0,1], scales MoveSpeed this step. Lets an
        // analog stick (or a path follower's Speed) walk below full speed.
        float     PendingMoveThrottle  = 0.0f;

        // Staged Launch (jump pad / knockback / dash) consumed in the physics step.
        FVector3  PendingLaunchVelocity     = FVector3(0.0f);
        bool      bPendingLaunch            = false;
        bool      bLaunchOverrideHorizontal = false;
        bool      bLaunchOverrideVertical   = false;

        // Staged Teleport (respawn) consumed in the physics step; moves the
        // authoritative CharacterVirtual, which a plain transform write cannot.
        FVector3 PendingTeleportLocation = FVector3(0.0f);
        bool      bPendingTeleport        = false;
    };

    
}
