#pragma once
#include "PhysicsTypes.h"
#include "Core/Templates/Optional.h"
#include "Containers/Array.h"
#include "Memory/SmartPtr.h"
#include "Core/Math/Matrix/MatrixMath.h"
#include "Ray/RayCast.h"
#include "World/Entity/Events/ImpulseEvent.h"
#include "Renderer/SkeletonResource.h"

namespace Lumina
{
    class CPhysicsAsset;
    class CMesh;
    class CCollisionShape;
    class CPhysicsMaterial;
    struct FSkeletonResource;
    struct FJoltRagdollHandle;
}

namespace Lumina::Physics
{
    // Inputs to build one ragdoll. Jolt-free so gameplay/world code can drive it without leaking Jolt.
    struct FRagdollDesc
    {
        entt::entity                Entity = entt::null;            // Owning entity; written to each body's user data for contact mapping.
        CPhysicsAsset*              Asset = nullptr;                 // Authored bodies/constraints; null => auto-generate from Skeleton.
        const FSkeletonResource*    Skeleton = nullptr;             // Source skeleton (bone hierarchy + bind pose).
        const TVector<FMatrix4>*    ComponentBoneGlobals = nullptr; // Component-space bone global transforms to spawn at (size == bone count).
        FMatrix4                    EntityToWorld;                  // Entity world matrix (component space -> world).
        FCollisionProfile           FallbackProfile;               // Applied when Asset is null.
        uint32                      CollisionGroupID = 0;           // Unique per ragdoll for parent/child self-collision filtering.
    };

    // Runtime motor drive mode for a powered Hinge/Slider constraint.
    enum class EConstraintMotorMode : uint8
    {
        Off      = 0,   // Motor disabled (joint is free / friction only).
        Velocity = 1,   // Drive toward a target velocity (rad/s hinge, m/s slider).
        Position = 2,   // Drive toward a target position (rad hinge, m slider) via the motor spring.
    };

    // Jolt-free description of one constraint, built by gameplay/editor code and resolved by the scene.
    // World-space frames; a body set to entt::null is treated as "fixed to the world".
    struct FConstraintDesc
    {
        EPhysicsConstraintType  Type        = EPhysicsConstraintType::Point;
        entt::entity            BodyA       = entt::null;            // entt::null => world anchor (parent).
        entt::entity            BodyB       = entt::null;            // The constrained (child) body.
        FVector3                Anchor      = FVector3(0.0f);        // World pivot (Point/Hinge/Slider/Cone).
        FVector3                Axis        = FVector3(0.0f, 1.0f, 0.0f); // Hinge/Cone axis or Slider direction (world).
        FVector3                AnchorB     = FVector3(0.0f);        // Distance: second attach point (Anchor is the first).
        float                   MinLimit    = 0.0f;                  // Hinge angle (rad) / Slider pos (m) / Distance min.
        float                   MaxLimit    = 0.0f;                  // Hinge angle (rad) / Slider pos (m) / Distance max.
        float                   HalfConeAngle = 0.0f;               // Cone half-angle (rad).
        bool                    bHasLimits  = false;                 // When false, the joint is unlimited on its free axis.
        float                   LimitFrequency = 0.0f;               // Soft-limit spring (0 = hard limit).
        float                   LimitDamping   = 0.0f;
        float                   MaxFriction    = 0.0f;               // Hinge friction torque (N m) / Slider friction force (N).
        float                   MotorFrequency = 0.0f;               // Position-motor spring (0 = stiff default).
        float                   MotorDamping   = 0.0f;
        float                   MotorForceLimit  = 0.0f;             // Slider motor max force (N), 0 = unlimited.
        float                   MotorTorqueLimit = 0.0f;             // Hinge motor max torque (N m), 0 = unlimited.
        float                   BreakForce  = 0.0f;                  // Disable the joint when applied force exceeds this (N). 0 = unbreakable.
    };

    // One static collision instance with no entity of its own (foliage). World space; Shape wins over Mesh.
    struct FStaticInstanceDesc
    {
        FVector3                Position = FVector3(0.0f);
        FQuat                   Rotation = FQuat::Identity();
        FVector3                Scale = FVector3(1.0f);
        const CMesh*            Mesh = nullptr;
        const CCollisionShape*  Shape = nullptr;
        const CPhysicsMaterial* Material = nullptr;
        bool                    bConvex = true;
    };

    class IPhysicsScene
    {
    public:

        virtual ~IPhysicsScene() { }
        virtual void PreUpdate() = 0;
        virtual void Update(double DeltaTime) = 0;
        virtual void PostUpdate() = 0;
        virtual void Simulate() = 0;
        virtual void StopSimulate() = 0;

        // Game-thread drain of step-side events (Lua, entt::dispatcher). Pair with Update.
        virtual void DispatchPendingEvents() {}
        
        virtual void DeactivateBody(uint32 BodyID) = 0;
        virtual void ActivateBody(uint32 BodyID) = 0;
        virtual void ChangeBodyMotionType(uint32 BodyID, EBodyType NewType) = 0;

        // Whether the body is awake (active) vs asleep (at rest). Default false so non-Jolt backends compile.
        virtual bool IsBodyActive(uint32 BodyID) { return false; }
        
        virtual uint32 GetEntityBodyID(entt::entity Entity) = 0;
        
        virtual TOptional<SRayResult> CastRay(const SRayCastSettings& Settings) = 0;

        // Sweep hits near-to-far; OutHits is CLEARED first, so reusing one buffer keeps the query alloc-free.
        virtual void CastSphere(const SSphereCastSettings& Settings, TVector<SRayResult>& OutHits) = 0;

        // Nearest sweep hit; lets the backend shrink the query instead of gathering every hit behind it.
        virtual TOptional<SRayResult> CastSphereClosest(const SSphereCastSettings& Settings) { return {}; }

        // Every body the ray crosses, near-to-far (penetrating bullets). OutHits is CLEARED first.
        virtual void CastRayAll(const SRayCastSettings& Settings, TVector<SRayResult>& OutHits) { OutHits.clear(); }

        // Distinct entities whose bodies CONTAIN the world point (volume containment, no sweep).
        virtual int32 CollidePoint(const FVector3& Point, TSpan<const uint32> IgnoreBodies, TSpan<entt::entity> OutEntities) { return 0; }

        // Distinct entities intersecting the shape; returns the count written, == size() means possibly more.
        virtual int32 OverlapSphere(const FVector3& Center, float Radius, TSpan<const uint32> IgnoreBodies, TSpan<entt::entity> OutEntities) = 0;
        virtual int32 OverlapBox(const FVector3& Center, const FVector3& HalfExtents, const FQuat& Rotation, TSpan<const uint32> IgnoreBodies, TSpan<entt::entity> OutEntities) = 0;
        
        virtual void OnImpulseEvent(const SImpulseEvent& Impulse) = 0;
        virtual void OnForceEvent(const SForceEvent& Force) = 0;
        virtual void OnTorqueEvent(const STorqueEvent& Torque) = 0;
        virtual void OnAngularImpulseEvent(const SAngularImpulseEvent& AngularImpulse) = 0;
        virtual void OnSetVelocityEvent(const SSetVelocityEvent& Velocity) = 0;
        virtual void OnSetAngularVelocityEvent(const SSetAngularVelocityEvent& AngularVelocity) = 0;
        virtual void OnAddImpulseAtPositionEvent(const SAddImpulseAtPositionEvent& Event) = 0;
        virtual void OnAddForceAtPositionEvent(const SAddForceAtPositionEvent& Event) = 0;
        virtual void OnSetGravityFactorEvent(const SSetGravityFactorEvent& Event) = 0;

        // Shape-accurate buoyancy: applies Jolt's submerged-volume buoyancy + linear/angular drag impulse for
        // this frame. The caller supplies the fluid surface point + normal (e.g. sampled from the rendered
        // Gerstner waves), so the fluid plane follows the wave surface instead of being flat. Buoyancy is the
        // fluid/body density ratio: 1 = neutral, >1 floats. No-op without a dynamic body. Default: no backend.
        virtual void ApplyBuoyancyImpulse(entt::entity Entity, const FVector3& SurfacePosition, const FVector3& SurfaceNormal,
            float Buoyancy, float LinearDrag, float AngularDrag, const FVector3& FluidVelocity, float DeltaTime) {}

        // Conveyor belt / moving surface: objects resting on this body's surface are dragged at this
        // world-space velocity (linear m/s, angular rad/s). Zero clears it. Default: no backend.
        virtual void SetSurfaceVelocity(entt::entity Entity, const FVector3& Linear, const FVector3& Angular) {}

        virtual FVector3 GetVelocityAtPoint(uint32 BodyID, const FVector3& Point) = 0;
        virtual FVector3 GetLinearVelocity(uint32 BodyID) = 0;
        virtual FVector3 GetAngularVelocity(uint32 BodyID) = 0;
        virtual FVector3 GetCenterOfMass(uint32 BodyID)= 0;

        /** Body mass in kg, or 0 for a body that cannot move (static/kinematic have infinite mass). Default
         *  1 so non-Jolt backends compile; callers scaling a force by it get sane behaviour either way. */
        virtual float GetBodyMass(uint32 BodyID) { return 1.0f; }

        // Actual current body pose, NOT the interpolated render transform (STransformComponent is lagged).
        virtual FVector3 GetBodyPosition(uint32 BodyID) = 0;
        virtual FQuat GetBodyRotation(uint32 BodyID) = 0;

        /** Current live body count and the configured ceiling. Lets bulk spawners (fracture) clamp to capacity instead of overflowing Jolt's body buffer. */
        virtual uint32 GetBodyCount() = 0;
        virtual uint32 GetMaxBodyCount() = 0;

        // Between Begin/End, body constructions are queued and inserted by End in one AddBodiesPrepare/Finalize.
        // Game-thread only, must be balanced; nests (an inner pair folds into the outermost batch, which is
        // what commits). BodyIDs are valid only after the outermost EndBodyBatch.
        virtual void BeginBodyBatch() = 0;
        virtual void EndBodyBatch() = 0;

        // Static bodies for instanced geometry (foliage), merged per material and cell into compound bodies; hits report Owner. 0 = nothing built.
        virtual uint32 CreateStaticBodyGroup(entt::entity Owner, TSpan<const FStaticInstanceDesc> Instances) { return 0; }
        virtual void DestroyStaticBodyGroup(uint32 GroupID) {}

        // Build a ragdoll's bodies + constraints and add them to the scene. Returns an opaque handle the
        // caller stores; null on failure. Must be called outside the physics step (PrePhysics is fine).
        virtual TSharedPtr<FJoltRagdollHandle> CreateRagdoll(const FRagdollDesc& Desc) = 0;

        // Read the simulated body transforms back into component-space GPU skinning matrices (Global*InvBind).
        // OutBoneTransforms is sized to the skeleton; unmapped bones are rebuilt from their parent + bind local.
        virtual void ReadRagdollPose(const FJoltRagdollHandle& Handle, const FMatrix4& WorldToEntity, const FSkeletonResource* Skeleton, TVector<FMatrix4>& OutBoneTransforms) = 0;

        // Remove a ragdoll's bodies + constraints from the scene. Safe with a null/empty handle.
        virtual void DestroyRagdoll(const TSharedPtr<FJoltRagdollHandle>& Handle) = 0;

        // World-space transform of the ragdoll's root body (used to drive the owning entity so culling tracks it).
        virtual void GetRagdollRootTransform(const FJoltRagdollHandle& Handle, FVector3& OutPosition, FQuat& OutRotation) = 0;

        // Monotonic unique id for the next ragdoll's self-collision group.
        virtual uint32 AllocateRagdollGroupID() = 0;

        // Constraints / joints. Create returns an opaque non-zero handle (0 == failure); the caller stores it
        // to drive motors, query break state, or destroy the joint. Must be called outside the physics step
        // (gameplay/PrePhysics is fine), mirroring CreateRagdoll. Default no-op so non-Jolt backends compile.
        virtual uint32 CreateConstraint(const FConstraintDesc& Desc) { return 0; }
        virtual void DestroyConstraint(uint32 ConstraintID) {}
        virtual void SetConstraintEnabled(uint32 ConstraintID, bool bEnabled) {}
        // Drive a Hinge/Slider motor. Target is rad/s|m/s for Velocity, rad|m for Position; ignored for Off.
        virtual void SetConstraintMotor(uint32 ConstraintID, EConstraintMotorMode Mode, float Target) {}
        // True once a breakable joint's applied force exceeded its BreakForce (the joint is then disabled).
        virtual bool IsConstraintBroken(uint32 ConstraintID) { return false; }
        // Current value of a powered joint: a Hinge's angle (radians) or a Slider's position (meters). 0 for
        // joints without a single driven scalar (Fixed/Point/Distance/Cone).
        virtual float GetConstraintValue(uint32 ConstraintID) { return 0.0f; }

        void AddForce(entt::entity E, const FVector3& Force)                   { SForceEvent Ev; Ev.BodyID = GetEntityBodyID(E); Ev.Force = Force; OnForceEvent(Ev); }
        void AddImpulse(entt::entity E, const FVector3& Impulse)               { SImpulseEvent Ev; Ev.BodyID = GetEntityBodyID(E); Ev.Impulse = Impulse; OnImpulseEvent(Ev); }
        void AddTorque(entt::entity E, const FVector3& Torque)                 { STorqueEvent Ev; Ev.BodyID = GetEntityBodyID(E); Ev.Torque = Torque; OnTorqueEvent(Ev); }
        void AddAngularImpulse(entt::entity E, const FVector3& AngularImpulse) { SAngularImpulseEvent Ev; Ev.BodyID = GetEntityBodyID(E); Ev.AngularImpulse = AngularImpulse; OnAngularImpulseEvent(Ev); }
        void AddForceAtPosition(entt::entity E, const FVector3& Force, const FVector3& Position)     { SAddForceAtPositionEvent Ev; Ev.BodyID = GetEntityBodyID(E); Ev.Force = Force; Ev.Position = Position; OnAddForceAtPositionEvent(Ev); }
        void AddImpulseAtPosition(entt::entity E, const FVector3& Impulse, const FVector3& Position) { SAddImpulseAtPositionEvent Ev; Ev.BodyID = GetEntityBodyID(E); Ev.Impulse = Impulse; Ev.Position = Position; OnAddImpulseAtPositionEvent(Ev); }
        void SetLinearVelocity(entt::entity E, const FVector3& Velocity)         { SSetVelocityEvent Ev; Ev.BodyID = GetEntityBodyID(E); Ev.Velocity = Velocity; OnSetVelocityEvent(Ev); }
        void SetAngularVelocity(entt::entity E, const FVector3& AngularVelocity) { SSetAngularVelocityEvent Ev; Ev.BodyID = GetEntityBodyID(E); Ev.AngularVelocity = AngularVelocity; OnSetAngularVelocityEvent(Ev); }
        void SetGravityFactor(entt::entity E, float Factor)                     { SSetGravityFactorEvent Ev; Ev.BodyID = GetEntityBodyID(E); Ev.GravityFactor = Factor; OnSetGravityFactorEvent(Ev); }

        FVector3 GetLinearVelocity(entt::entity E)                         { return GetLinearVelocity(GetEntityBodyID(E)); }
        FVector3 GetAngularVelocity(entt::entity E)                        { return GetAngularVelocity(GetEntityBodyID(E)); }
        FVector3 GetVelocityAtPoint(entt::entity E, const FVector3& Point) { return GetVelocityAtPoint(GetEntityBodyID(E), Point); }
        FVector3 GetCenterOfMass(entt::entity E)                           { return GetCenterOfMass(GetEntityBodyID(E)); }
        FVector3 GetBodyPosition(entt::entity E)                           { return GetBodyPosition(GetEntityBodyID(E)); }
        FQuat    GetBodyRotation(entt::entity E)                           { return GetBodyRotation(GetEntityBodyID(E)); }
    };
}
