#pragma once

#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Object/ObjectMacros.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Physics/PhysicsTypes.h"
#include "World/Entity/Events/CollisionEvent.h"
#include "PhysicsComponent.generated.h"

namespace Lumina
{
    class CStaticMesh;
    class CPhysicsMaterial;
    class CCollisionShape;

    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API CACHE_ALIGN SRigidBodyComponent
    {
        GENERATED_BODY()
        
        // Snapshots for interpolation.
        FVector3 LastBodyPosition;
        FQuat LastBodyRotation;
        
        /** Internal Jolt body ID, read-only, assigned by the physics system. */
        PROPERTY(Script, ReadOnly, Category = "Physics")
        uint32 BodyID = 0xFFFFFFFF;

        /** Mass of the rigid body in kg. */
        PROPERTY(Script, Editable, Category = "Physics")
        float Mass = 1.0f;

        /** When true, Mass overrides the value Jolt would compute from shape density. */
        PROPERTY(Script, Editable, Category = "Physics")
        bool bOverrideMass = false;

        /** When true, InertiaTensor replaces Jolt's shape-derived inertia (top-heavy vehicles, hand-tuned
            spin resistance). Uses Mass for the body's mass. Dynamic bodies only. */
        PROPERTY(Script, Editable, Category = "Physics")
        bool bOverrideInertia = false;

        /** Diagonal inertia tensor (Ixx, Iyy, Izz in kg·m²) when bOverrideInertia is set. Larger on an axis
            = harder to spin about it. */
        PROPERTY(Script, Editable, Category = "Physics")
        FVector3 InertiaTensor = FVector3(1.0f);

        /** Local-space offset applied to the body's center of mass. Lower the Y for car-like weight bias. */
        PROPERTY(Script, Editable, Category = "Physics")
        FVector3 CenterOfMassOffset = FVector3(0.0f);

        /** Layer and mask controlling which bodies this one collides with. */
        PROPERTY(Script, Editable, Category = "Physics")
        FCollisionProfile CollisionProfile;

        /** Per-body override for global velocity solver iterations (0 = use world setting). */
        PROPERTY(Script, Editable, Category = "Physics")
        uint32 NumVelocityStepsOverride = 0;

        /** Per-body override for global position solver iterations (0 = use world setting). */
        PROPERTY(Script, Editable, Category = "Physics")
        uint32 NumPositionStepsOverride = 0;

        /** Maximum linear speed (m/s) this body can reach. */
        PROPERTY(Script, Editable, ClampMin = 0.001f, Category = "Physics")
        float MaxLinearVelocity = 500.0f;

        /** Maximum angular speed (rad/s) this body can reach. */
        PROPERTY(Script, Editable, ClampMin = 0.001f, Category = "Physics")
        float MaxAngularVelocity = 0.25f * LE_PI_F * 60.0f;

        /** Bounciness override for this body (0 = no bounce, 1 = perfectly elastic). */
        PROPERTY(Script, Editable, ClampMin = 0.001f, ClampMax = 1.0f, Category = "Physics")
        float RestitutionOverride = 0.5f;

        /** Surface friction coefficient override for this body. */
        PROPERTY(Script, Editable, ClampMin = 0.001f, ClampMax = 1.0f, Category = "Physics")
        float FrictionOverride = 0.3f;

        /** Damping applied to linear velocity each step. */
        PROPERTY(Script, Editable, ClampMin = 0.001f, ClampMax = 1.0f, Category = "Physics")
        float LinearDamping = 0.0f;

        /** Damping applied to angular velocity each step. */
        PROPERTY(Script, Editable, ClampMin = 0.001f, ClampMax = 1.0f, Category = "Physics")
        float AngularDamping = 0.05f;

        /** Motion quality level: 0 = Discrete, 1 = LinearCast. Higher is more expensive but prevents tunneling. */
        PROPERTY(Script, Editable, Category = "Physics")
        uint8 MotionQualityLevel = 0;

        /** Whether the body is Static, Kinematic, or Dynamic. */
        PROPERTY(Script, Editable, Category = "Physics")
        EBodyType BodyType = EBodyType::Dynamic;

        /** When true, the body detects overlaps but does not produce contact responses. */
        PROPERTY(Script, Editable, Category = "Physics")
        bool bIsSensor = false;

        /** Merge similar contact manifolds to reduce solver work for this body. */
        PROPERTY(Script, Editable, Category = "Physics")
        bool bUseManifoldReduction = true;

        /** Apply gyroscopic torque to spinning bodies for more realistic angular motion. */
        PROPERTY(Script, Editable, Category = "Physics")
        bool bApplyGyroscopicForce = false;

        /** Allow this body to enter sleep state when it comes to rest. */
        PROPERTY(Script, Editable, Category = "Physics")
        bool bAllowSleeping = true;

        /** Apply global gravity to this body. Disable for projectiles or floating objects. */
        PROPERTY(Script, Editable, Category = "Physics")
        bool bUseGravity = true;

        // Per-axis degree-of-freedom locks (world space). Locking lets you build 2D/planar mechanics or
        // pin a body to an axis without an external constraint. Dynamic bodies only; locking all six is
        // invalid (use a Static body instead) and is ignored.

        /** Prevent the body from translating along the world X axis. */
        PROPERTY(Script, Editable, Category = "Physics|Constraints")
        bool bLockTranslationX = false;

        /** Prevent the body from translating along the world Y axis. */
        PROPERTY(Script, Editable, Category = "Physics|Constraints")
        bool bLockTranslationY = false;

        /** Prevent the body from translating along the world Z axis. */
        PROPERTY(Script, Editable, Category = "Physics|Constraints")
        bool bLockTranslationZ = false;

        /** Prevent the body from rotating about the world X axis. */
        PROPERTY(Script, Editable, Category = "Physics|Constraints")
        bool bLockRotationX = false;

        /** Prevent the body from rotating about the world Y axis. */
        PROPERTY(Script, Editable, Category = "Physics|Constraints")
        bool bLockRotationY = false;

        /** Prevent the body from rotating about the world Z axis. */
        PROPERTY(Script, Editable, Category = "Physics|Constraints")
        bool bLockRotationZ = false;

        PROPERTY(Script)
        TScriptDelegate<SCollisionEvent> OnContactBegin;

        // Velocities are zero in this payload.
        PROPERTY(Script)
        TScriptDelegate<SCollisionEvent> OnContactEnd;

        PROPERTY(Script)
        TScriptDelegate<SCollisionEvent> OnOverlapBegin;

        PROPERTY(Script)
        TScriptDelegate<SCollisionEvent> OnOverlapEnd;

        PROPERTY(Script)
        FScriptDelegate OnWake;

        PROPERTY(Script)
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
        FVector3 TranslationOffset;

        /** Local-space euler rotation offset applied to the collider. */
        PROPERTY(Editable)
        FVector3 RotationOffset;

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TObjectPtr<CPhysicsMaterial> PhysicsMaterial;

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
        TObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    // Capsule collider: cylinder of length 2*HalfHeight capped by radius-Radius hemispheres, along local Y
    // (RotationOffset lays it on its side). Total height 2*(HalfHeight + Radius).
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
        TObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    // Cylinder collider: flat-ended, along local Y; cheaper than a convex hull but rolls realistically.
    // Total height 2*HalfHeight; CapRadius rounds the rim edges (0 = sharp).
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
        FVector3 TranslationOffset;

        /** Local-space euler rotation offset applied to the collider. */
        PROPERTY(Editable)
        FVector3 RotationOffset;

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    // Tapered capsule: a capsule with different top/bottom cap radii (tree trunks, tapered limbs), along
    // local Y. Total height ~2*(HalfHeight + max radius). Convex, so it works on dynamic bodies.
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
        FVector3 TranslationOffset;

        /** Local-space euler rotation offset applied to the collider. */
        PROPERTY(Editable)
        FVector3 RotationOffset;

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    // Tapered cylinder: flat-ended cylinder with different top/bottom radii (funnels, barrels, tapered
    // pillars), along local Y. Total height 2*HalfHeight; ConvexRadius rounds the rims.
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

        /** Rounding of the top/bottom rim. Clamped to the smallest radius / half-height. */
        PROPERTY(Editable, ClampMin = 0.0f)
        float ConvexRadius = 0.05f;

        /** Local-space offset applied to the collider position relative to the entity. */
        PROPERTY(Editable)
        FVector3 TranslationOffset;

        /** Local-space euler rotation offset applied to the collider. */
        PROPERTY(Editable)
        FVector3 RotationOffset;

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    // Infinite plane collider (water surfaces, kill floors, level boundaries). The local +Y axis is the
    // surface normal -- orient/position via the entity transform; the half-space below the plane is solid.
    // Static only (the body is forced Static, like terrain).
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
        TObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    /** Static collider built from an STerrainComponent's heightmap (Jolt HeightFieldShape). Requires both components on the same entity. */
    // Collision built from an SDynamicMeshComponent's committed geometry on the same entity. The mesh has
    // no CMesh to point at (it owns its render data outright), so unlike SMeshColliderComponent there is no
    // asset reference -- the shape is rebuilt from whatever that component last committed.
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SDynamicMeshColliderComponent
    {
        GENERATED_BODY()

        /** Build a convex hull (allows dynamic bodies). False builds a concave triangle mesh: static/kinematic only,
            which is what terrain wants. */
        PROPERTY(Script, Editable)
        bool bConvex = false;

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Script, Editable)
        TObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Script, Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Script, Editable, Category = "Navigation")
        bool bAffectsNavigation = true;

        /** Bumped by SDynamicMeshComponent::Commit so the physics scene knows the geometry changed and
            rebuilds the body. Not authored. */
        PROPERTY(Script, ReadOnly)
        uint32 GeometryVersion = 0;
    };

    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API STerrainColliderComponent
    {
        GENERATED_BODY()

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    /** Primitive type for one child of a compound collider. */
    REFLECT()
    enum class RUNTIME_API ECompoundShapeType : uint8
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

    // Custom collision shape built by merging several primitives into one body (Jolt StaticCompoundShape):
    // a table = box top + 4 box legs, an L-wall = two boxes, a barrel cluster = cylinders, etc. Concave
    // overall yet still usable on a dynamic body, and cheap (the children share the cached primitive shapes).
    // Needs at least 2 children to form a compound; with 1 it falls back to that single offset shape.
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SCompoundColliderComponent
    {
        GENERATED_BODY()

        /** Child primitives, each at its own local offset/rotation, merged into one collider. */
        PROPERTY(Editable)
        TVector<SCompoundSubShape> Shapes;

        /** Physics material driving friction/restitution for the whole body. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    // Conveyor / moving surface. Bodies resting on this entity's collider are dragged along its surface at
    // the given world-space velocity, without the body itself moving (Jolt feeds it into the contact solver).
    // Requires an SRigidBodyComponent on the same entity. SurfaceVelocity is m/s; AngularSurfaceVelocity is
    // rad/s about the body's center (best for a body that is the conveyor itself). Runtime changes:
    // World.Physics.SetSurfaceVelocity.
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SConveyorComponent
    {
        GENERATED_BODY()

        /** World-space linear surface velocity (m/s). Objects on the surface are dragged this way. */
        PROPERTY(Script, Editable, Category = "Conveyor")
        FVector3 SurfaceVelocity = FVector3(0.0f);

        /** World-space angular surface velocity (rad/s) about the body center -- spinning turntables. */
        PROPERTY(Script, Editable, Category = "Conveyor")
        FVector3 AngularSurfaceVelocity = FVector3(0.0f);
    };

    // Joint connecting this entity's rigid body to another body (or the world). The system creates the live
    // Jolt constraint once both bodies exist; removing the component (or the entity) tears it down. This
    // entity is body B (the child); TargetBody is body A (the parent / anchor).
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SPhysicsConstraintComponent
    {
        GENERATED_BODY()

        /** Joint type. */
        PROPERTY(Script, Editable, Category = "Constraint")
        EPhysicsConstraintType Type = EPhysicsConstraintType::Point;

        /** The body this entity is jointed to. Leave as the null entity to anchor to the world. */
        PROPERTY(Script, Editable, Entity, Category = "Constraint")
        uint32 TargetBody = 0xFFFFFFFF;

        /** Pivot in this entity's local space (Point/Hinge/Slider/Cone). The world anchor tracks the body. */
        PROPERTY(Script, Editable, Category = "Constraint")
        FVector3 PivotOffset = FVector3(0.0f);

        /** Hinge/Cone axis or Slider direction, in this entity's local space. */
        PROPERTY(Script, Editable, Category = "Constraint")
        FVector3 Axis = FVector3(0.0f, 1.0f, 0.0f);

        /** Enable the limits below (Hinge swing / Slider travel / Distance range). */
        PROPERTY(Script, Editable, Category = "Limits")
        bool bLimited = false;

        /** Lower limit: Hinge angle (degrees), Slider position (m), Distance min (m). */
        PROPERTY(Script, Editable, Category = "Limits")
        float LowerLimit = 0.0f;

        /** Upper limit: Hinge angle (degrees), Slider position (m), Distance max (m). */
        PROPERTY(Script, Editable, Category = "Limits")
        float UpperLimit = 0.0f;

        /** Cone half-angle in degrees (Cone type only). */
        PROPERTY(Script, Editable, Category = "Limits", ClampMin = 0.0f, ClampMax = 180.0f)
        float ConeHalfAngle = 45.0f;

        /** Passive friction resisting the free axis: torque (Hinge, N m) or force (Slider, N). */
        PROPERTY(Script, Editable, Category = "Constraint", ClampMin = 0.0f)
        float Friction = 0.0f;

        /** Force/torque (N) that snaps the joint, after which it is disabled. 0 = unbreakable. */
        PROPERTY(Script, Editable, Category = "Constraint", ClampMin = 0.0f)
        float BreakForce = 0.0f;

        /** Live constraint handle assigned by the physics system; 0 until created. Read-only. */
        PROPERTY(Script, ReadOnly, Category = "Constraint")
        uint32 ConstraintID = 0;
    };

    // Collision from an authored CCollisionShape rather than derived from the render mesh. Preferred over
    // SMeshColliderComponent wherever collision should be simplified, decomposed, or shared.
    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SCollisionShapeComponent
    {
        GENERATED_BODY()

        /** Authored collision. Null builds no body at all rather than silently falling back to the mesh. */
        PROPERTY(Editable)
        TObjectPtr<CCollisionShape> CollisionShape;

        /** Local-space offset applied to the collider position relative to the entity. */
        PROPERTY(Editable)
        FVector3 TranslationOffset;

        /** Local-space euler rotation offset applied to the collider. */
        PROPERTY(Editable)
        FVector3 RotationOffset;

        /** Overrides the asset's material for this instance; null uses the asset's. */
        PROPERTY(Editable)
        TObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

    REFLECT(Component, Category = "Physics")
    struct RUNTIME_API SMeshColliderComponent
    {
        GENERATED_BODY()

        /** Mesh asset used as collision geometry. If null, the system falls back to the entity's StaticMeshComponent. */
        PROPERTY(Editable)
        TObjectPtr<CStaticMesh> Mesh;

        /** Build a convex hull from the mesh (allows dynamic bodies). When false, builds a concave triangle mesh (static / kinematic only). */
        PROPERTY(Editable)
        bool bConvex = false;

        /** Local-space offset applied to the collider position relative to the entity. */
        PROPERTY(Editable)
        FVector3 TranslationOffset;

        /** Local-space euler rotation offset applied to the collider. */
        PROPERTY(Editable)
        FVector3 RotationOffset;

        /** Physics material driving friction/restitution. Null falls back to the rigid body's *Override fields. */
        PROPERTY(Editable)
        TObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** When true, the body produces overlap events but no contact response (trigger volume). */
        PROPERTY(Editable)
        bool bIsTrigger = false;

        /** When true, this collider contributes its shape to NavMesh bakes. */
        PROPERTY(Editable, Category = "Navigation")
        bool bAffectsNavigation = true;
    };

}
