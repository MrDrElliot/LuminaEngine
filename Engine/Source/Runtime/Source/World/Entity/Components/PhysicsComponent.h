#pragma once

#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Containers/Vector.h"
#include "Memory/SmartPtr.h"
#include "Physics/PhysicsTypes.h"
#include "World/Entity/Events/CollisionEvent.h"
#include "PhysicsComponent.generated.h"

namespace Lumina
{
    class CStaticMesh;
    class CPhysicsMaterial;
    namespace Physics { class FCollisionShape; }

    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API CACHE_ALIGN SRigidBodyComponent
    {
        GENERATED_BODY()
        
        /** Mass of the rigid body in kg. */
        PROPERTY(Editable, Category = "Physics")
        float Mass = 1.0f;

        /** When true, Mass overrides the value computed from shape density. */
        PROPERTY(Editable, Category = "Physics")
        bool bOverrideMass = false;

        /** When true, InertiaTensor replaces the shape-derived inertia (top-heavy vehicles, hand-tuned
            spin resistance). Uses Mass for the body's mass. Dynamic bodies only. */
        PROPERTY(Editable, Category = "Physics")
        bool bOverrideInertia = false;

        /** Diagonal inertia tensor (Ixx, Iyy, Izz in kg·m²) when bOverrideInertia is set. Larger on an axis
            = harder to spin about it. */
        PROPERTY(Editable, Category = "Physics")
        FVector3 InertiaTensor = FVector3(1.0f);

        /** Local-space offset applied to the body's center of mass. Lower the Y for car-like weight bias. */
        PROPERTY(Editable, Category = "Physics")
        FVector3 CenterOfMassOffset = FVector3(0.0f);

        /** Layer and mask controlling which bodies this one collides with. */
        PROPERTY(Editable, Category = "Physics")
        SCollisionProfile CollisionProfile;

        /** Bounciness override for this body (0 = no bounce, 1 = perfectly elastic). */
        PROPERTY(Editable, ClampMin = 0.001f, ClampMax = 1.0f, Category = "Physics")
        float RestitutionOverride = 0.5f;

        /** Surface friction coefficient override for this body. */
        PROPERTY(Editable, ClampMin = 0.001f, ClampMax = 1.0f, Category = "Physics")
        float FrictionOverride = 0.3f;

        /** Damping applied to linear velocity each step. */
        PROPERTY(Editable, ClampMin = 0.001f, ClampMax = 1.0f, Category = "Physics")
        float LinearDamping = 0.0f;

        /** Damping applied to angular velocity each step. */
        PROPERTY(Editable, ClampMin = 0.001f, ClampMax = 1.0f, Category = "Physics")
        float AngularDamping = 0.05f;

        /** Sweep this body against the world each step instead of testing it only at its new pose. */
        PROPERTY(Editable, Category = "Physics")
        bool bUseContinuousCollision = false;

        /** Whether the body is Static, Kinematic, or Dynamic. */
        PROPERTY(Editable, Category = "Physics")
        EBodyType BodyType = EBodyType::Dynamic;

        /** When true, the body detects overlaps but does not produce contact responses. */
        PROPERTY(Editable, Category = "Physics")
        bool bIsSensor = false;

        /** Allow this body to enter sleep state when it comes to rest. */
        PROPERTY(Editable, Category = "Physics")
        bool bAllowSleeping = true;

        /** Apply global gravity to this body. Disable for projectiles or floating objects. */
        PROPERTY(Editable, Category = "Physics")
        bool bUseGravity = true;

        // Per-axis world space locks for dynamic bodies, and locking all six is ignored (use a Static body).

        /** Prevent the body from translating along the world X axis. */
        PROPERTY(Editable, Category = "Physics|Constraints")
        bool bLockTranslationX = false;

        /** Prevent the body from translating along the world Y axis. */
        PROPERTY(Editable, Category = "Physics|Constraints")
        bool bLockTranslationY = false;

        /** Prevent the body from translating along the world Z axis. */
        PROPERTY(Editable, Category = "Physics|Constraints")
        bool bLockTranslationZ = false;

        /** Prevent the body from rotating about the world X axis. */
        PROPERTY(Editable, Category = "Physics|Constraints")
        bool bLockRotationX = false;

        /** Prevent the body from rotating about the world Y axis. */
        PROPERTY(Editable, Category = "Physics|Constraints")
        bool bLockRotationY = false;

        /** Prevent the body from rotating about the world Z axis. */
        PROPERTY(Editable, Category = "Physics|Constraints")
        bool bLockRotationZ = false;

        PROPERTY(Hidden)
        TScriptDelegate<SCollisionEvent> OnContactBegin;

        // Velocities are zero in this payload.
        PROPERTY(Hidden)
        TScriptDelegate<SCollisionEvent> OnContactEnd;

        PROPERTY(Hidden)
        TScriptDelegate<SCollisionEvent> OnOverlapBegin;

        PROPERTY(Hidden)
        TScriptDelegate<SCollisionEvent> OnOverlapEnd;

        PROPERTY(Hidden)
        FScriptDelegate OnWake;

        PROPERTY(Hidden)
        FScriptDelegate OnSleep;

    };

    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SBoxColliderComponent
    {
        GENERATED_BODY()

        /** Half-size of the box along each axis in local space (meters). */
        PROPERTY(Editable)
        FVector3 HalfExtent = FVector3(0.5f);

        /** Local-space offset applied to the collider position relative to the entity. */
        PROPERTY(Editable)
        FVector3 TranslationOffset = FVector3(0.0f);

        /** Local-space euler rotation offset applied to the collider. */
        PROPERTY(Editable)
        FVector3 RotationOffset = FVector3(0.0f);

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TStrongObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SSphereColliderComponent
    {
        GENERATED_BODY()

        /** Radius of the sphere collider in meters. */
        PROPERTY(Editable)
        float Radius = 0.5f;

        /** Local-space offset applied to the collider position relative to the entity. */
        PROPERTY(Editable)
        FVector3 TranslationOffset;

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TStrongObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    // Capsule collider along local Y, with total height 2*(HalfHeight + Radius).
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SCapsuleColliderComponent
    {
        GENERATED_BODY()

        /** Radius of the hemispherical caps (meters). */
        PROPERTY(Editable, ClampMin = 0.001f)
        float Radius = 0.5f;

        /** Half-height of the cylindrical middle section (meters). Total height is 2*(HalfHeight + Radius). */
        PROPERTY(Editable, ClampMin = 0.001f)
        float HalfHeight = 0.5f;

        /** Local-space offset applied to the collider position relative to the entity. */
        PROPERTY(Editable)
        FVector3 TranslationOffset;

        /** Local-space euler rotation offset applied to the collider. Set X or Z to 90° to lie sideways. */
        PROPERTY(Editable)
        FVector3 RotationOffset;

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TStrongObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    // Flat-ended cylinder collider along local Y with total height 2*HalfHeight, and CapRadius rounds the rim.
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SCylinderColliderComponent
    {
        GENERATED_BODY()

        /** Radius at the cylinder's equator (meters). */
        PROPERTY(Editable, ClampMin = 0.001f)
        float Radius = 0.5f;

        /** Half the cylinder's total height (meters). */
        PROPERTY(Editable, ClampMin = 0.001f)
        float HalfHeight = 0.5f;

        /** Rounding of the top/bottom rim. Set to a small value (e.g. 0.02) to avoid sharp-edge stuck contacts. */
        PROPERTY(Editable, ClampMin = 0.0f)
        float CapRadius = 0.05f;

        /** Local-space offset applied to the collider position relative to the entity. */
        PROPERTY(Editable)
        FVector3 TranslationOffset = FVector3(0.0f);

        /** Local-space euler rotation offset applied to the collider. */
        PROPERTY(Editable)
        FVector3 RotationOffset = FVector3(0.0f);

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TStrongObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    // Capsule with different top and bottom radii along local Y, convex so it works on dynamic bodies.
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API STaperedCapsuleColliderComponent
    {
        GENERATED_BODY()

        /** Half-height of the cylindrical middle section between the two caps (meters). */
        PROPERTY(Editable, ClampMin = 0.0f)
        float HalfHeight = 0.5f;

        /** Radius of the top cap (+Y). */
        PROPERTY(Editable, ClampMin = 0.001f)
        float TopRadius = 0.25f;

        /** Radius of the bottom cap (-Y). */
        PROPERTY(Editable, ClampMin = 0.001f)
        float BottomRadius = 0.5f;

        /** Local-space offset applied to the collider position relative to the entity. */
        PROPERTY(Editable)
        FVector3 TranslationOffset = FVector3(0.0f);

        /** Local-space euler rotation offset applied to the collider. */
        PROPERTY(Editable)
        FVector3 RotationOffset = FVector3(0.0f);

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TStrongObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    // Flat-ended cylinder with different top and bottom radii along local Y, with total height 2*HalfHeight.
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API STaperedCylinderColliderComponent
    {
        GENERATED_BODY()

        /** Half the cylinder's total height (meters). */
        PROPERTY(Editable, ClampMin = 0.001f)
        float HalfHeight = 0.5f;

        /** Radius at the top (+Y). */
        PROPERTY(Editable, ClampMin = 0.001f)
        float TopRadius = 0.25f;

        /** Radius at the bottom (-Y). */
        PROPERTY(Editable, ClampMin = 0.001f)
        float BottomRadius = 0.5f;

        /** Local-space offset applied to the collider position relative to the entity. */
        PROPERTY(Editable)
        FVector3 TranslationOffset = FVector3(0.0f);

        /** Local-space euler rotation offset applied to the collider. */
        PROPERTY(Editable)
        FVector3 RotationOffset = FVector3(0.0f);

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TStrongObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    // Infinite static plane collider whose local +Y is the normal, with the half-space below it solid.
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SPlaneColliderComponent
    {
        GENERATED_BODY()

        /** Half-size of the plane's collision bounding box (meters). Keep as small as covers the play area
            for broad-phase performance; collisions outside it are undefined. */
        PROPERTY(Editable, ClampMin = 1.0f)
        float HalfExtent = 1000.0f;

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TStrongObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    // Triangles handed to a collider, held only until its physics scene cooks or drops them.
    struct FDynamicMeshCollisionInput
    {
        TVector<FVector3> Positions;
        TVector<uint32>   Indices;
    };

    // Collision from triangles handed over through Commit, cooked off the game thread. Needs no SDynamicMeshComponent.
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SDynamicMeshColliderComponent
    {
        GENERATED_BODY()

        /** Build a convex hull (allows dynamic bodies). False builds a concave triangle mesh: static/kinematic only,
            which is what terrain wants. Read when the cook takes the triangles, so a change needs another Commit. */
        PROPERTY(Editable)
        bool bConvex = false;

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TStrongObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;

        // Any thread, one at a time per component. Moves the triangles in; the next cook takes them and frees them.
        void Commit(TVector<FVector3>&& Positions, TVector<uint32>&& Indices);

        // Game thread, for the physics scene or a world without one. Null when nothing was committed since the last take.
        TSharedPtr<FDynamicMeshCollisionInput> TakeNewTriangles();

        // Acquire-load paired with the release in Commit, so a new version comes with its triangles.
        uint32 LoadGeometryVersion() const;

        // Bumped by every Commit.
        uint32 GeometryVersion = 0;

        // What the last cook built, null until one lands. The physics scene patches it in, so OnUpdate announces a new shape.
        TSharedPtr<const Physics::FCollisionShape> Shape;

    private:

        // Reached only through the atomic exchange, since Commit may run on a worker.
        TSharedPtr<FDynamicMeshCollisionInput> PendingTriangles;

        uint32 TakenGeometryVersion = 0;
    };

    /** Static collider built from an STerrainComponent's heightmap (height field). Requires both components on the same entity. */
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API STerrainColliderComponent
    {
        GENERATED_BODY()

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TStrongObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    /** Primitive type for one child of a compound collider. */
    REFLECT()
    enum class ECompoundShapeType : uint8
    {
        Box,
        Sphere,
        Capsule,
        Cylinder,
    };

    // One child primitive of an SCompoundColliderComponent: a shape placed at a local offset/rotation.
    REFLECT()
    struct RUNTIME_API SCompoundSubShape
    {
        GENERATED_BODY()

        /** Which primitive this child is. */
        PROPERTY(Editable)
        ECompoundShapeType Type = ECompoundShapeType::Box;

        /** Local-space position of this child relative to the body origin (meters). */
        PROPERTY(Editable)
        FVector3 Offset = FVector3(0.0f);

        /** Local-space euler rotation offset applied to this child. */
        PROPERTY(Editable)
        FVector3 Rotation = FVector3(0.0f);

        /** Box: half-size along each axis (meters). */
        PROPERTY(Editable)
        FVector3 HalfExtent = FVector3(0.5f);

        /** Sphere / Capsule / Cylinder: radius (meters). */
        PROPERTY(Editable, ClampMin = 0.001f)
        float Radius = 0.5f;

        /** Capsule / Cylinder: half-height of the middle section (meters). */
        PROPERTY(Editable, ClampMin = 0.001f)
        float HalfHeight = 0.5f;
    };

    // Several primitives merged into one body, concave overall yet usable on a dynamic body, and one child falls back to that single offset shape.
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SCompoundColliderComponent
    {
        GENERATED_BODY()

        /** Child primitives, each at its own local offset/rotation, merged into one collider. */
        PROPERTY(Editable)
        TVector<SCompoundSubShape> Shapes;

        /** Physics material driving friction/restitution for the whole body. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TStrongObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    // Conveyor surface that drags resting bodies at SurfaceVelocity (m/s) and AngularSurfaceVelocity (rad/s) without moving, and needs an SRigidBodyComponent.
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SConveyorComponent
    {
        GENERATED_BODY()

        /** World-space linear surface velocity (m/s). Objects on the surface are dragged this way. */
        PROPERTY(Editable, Category = "Conveyor")
        FVector3 SurfaceVelocity = FVector3(0.0f);

        /** World-space angular surface velocity (rad/s) about the body center -- spinning turntables. */
        PROPERTY(Editable, Category = "Conveyor")
        FVector3 AngularSurfaceVelocity = FVector3(0.0f);
    };

    // Joint from this entity (body B) to TargetBody (body A, the anchor), created once both bodies exist.
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SPhysicsConstraintComponent
    {
        GENERATED_BODY()

        /** Joint type. */
        PROPERTY(Editable, Category = "Constraint")
        EPhysicsConstraintType Type = EPhysicsConstraintType::Point;

        /** The body this entity is jointed to. Leave as the null entity to anchor to the world. */
        PROPERTY(Editable, Category = "Constraint")
        ECS::FEntity TargetEntity = ECS::NullEntity;

        /** Pivot in this entity's local space (Point/Hinge/Slider/Cone). The world anchor tracks the body. */
        PROPERTY(Editable, Category = "Constraint")
        FVector3 PivotOffset = FVector3(0.0f);

        /** Hinge/Cone axis or Slider direction, in this entity's local space. */
        PROPERTY(Editable, Category = "Constraint")
        FVector3 Axis = FVector3(0.0f, 1.0f, 0.0f);

        /** Enable the limits below (Hinge swing / Slider travel / Distance range). */
        PROPERTY(Editable, Category = "Limits")
        bool bLimited = false;

        /** Lower limit: Hinge angle (degrees), Slider position (m), Distance min (m). */
        PROPERTY(Editable, Category = "Limits")
        float LowerLimit = 0.0f;

        /** Upper limit: Hinge angle (degrees), Slider position (m), Distance max (m). */
        PROPERTY(Editable, Category = "Limits")
        float UpperLimit = 0.0f;

        /** Cone half-angle in degrees (Cone type only). */
        PROPERTY(Editable, Category = "Limits", ClampMin = 0.0f, ClampMax = 180.0f)
        float ConeHalfAngle = 45.0f;

        /** Passive friction resisting the free axis: torque (Hinge, N m) or force (Slider, N). */
        PROPERTY(Editable, Category = "Constraint", ClampMin = 0.0f)
        float Friction = 0.0f;

        /** Force/torque (N) that snaps the joint, after which it is disabled. 0 = unbreakable. */
        PROPERTY(Editable, Category = "Constraint", ClampMin = 0.0f)
        float BreakForce = 0.0f;

        /** Live constraint handle assigned by the physics system; 0 until created. Read-only. */
        PROPERTY(ReadOnly, Category = "Constraint")
        uint32 ConstraintID = 0;
    };

    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SMeshColliderComponent
    {
        GENERATED_BODY()

        /** Mesh asset used as collision geometry. If null, the system falls back to the entity's StaticMeshComponent. */
        PROPERTY(Editable)
        TStrongObjectPtr<CStaticMesh> Mesh;

        /** Build a convex hull from the mesh (allows dynamic bodies). When false, builds a concave triangle mesh (static / kinematic only). */
        PROPERTY(Editable)
        bool bConvex = false;

        /** Local-space offset applied to the collider position relative to the entity. */
        PROPERTY(Editable)
        FVector3 TranslationOffset = FVector3(0.0f);

        /** Local-space euler rotation offset applied to the collider. */
        PROPERTY(Editable)
        FVector3 RotationOffset = FVector3(0.0f);

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TStrongObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;

        // Mesh geometry version the collision body was built from, left unreflected because a loaded mesh restarts its count.
        uint32 GeometryVersion = 0;
    };

}
