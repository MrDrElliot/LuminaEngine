#pragma once

#include "World/ECS/Registry.h"


#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Math/Transform.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Physics/PhysicsTypes.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "Lumina.h"
#include "FoliageComponent.generated.h"

namespace Lumina
{
    struct FPropertyChangedEvent;

    class CWorld;
    struct SMeshCollision;
    class CPhysicsMaterial;

    // One painted foliage instance in WORLD space, with Rotation a quaternion laid out as (x, y, z, w).
    REFLECT()
    struct RUNTIME_API SFoliageInstance
    {
        GENERATED_BODY()

        PROPERTY(Hidden)
        FVector3 Position = FVector3(0.0f);

        PROPERTY(Hidden)
        FVector4 Rotation = FVector4(0.0f, 0.0f, 0.0f, 1.0f);

        PROPERTY(Hidden)
        FVector3 Scale = FVector3(1.0f);

        /** Index into the owning component's Types array. */
        PROPERTY(Hidden)
        int32 TypeIndex = 0;

        FQuat GetRotationQuat() const { return FQuat(Rotation.w, Rotation.x, Rotation.y, Rotation.z); }
        void  SetRotationQuat(const FQuat& Q) { Rotation = FVector4(Q.x, Q.y, Q.z, Q.w); }

        FMatrix4 GetMatrix() const
        {
            FTransform Xf;
            Xf.SetLocation(Position);
            Xf.SetRotation(GetRotationQuat());
            Xf.SetScale(Scale);
            return Xf.GetMatrix();
        }
    };

    // A paintable foliage species: the mesh to instance plus the scatter/instancing settings the tool uses
    // when painting and the renderer uses when drawing.
    REFLECT()
    struct RUNTIME_API SFoliageType
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Foliage Type")
        FString Name;

        /** Mesh drawn for every instance of this type. */
        PROPERTY(Editable, Category = "Foliage Type")
        TStrongObjectPtr<CStaticMesh> Mesh;

        /** Painted instances per square meter at full brush strength. */
        PROPERTY(Editable, Category = "Scatter", ClampMin = 0.0001f)
        float Density = 0.05f;

        PROPERTY(Editable, Category = "Scatter", ClampMin = 0.001f)
        float ScaleMin = 0.85f;

        PROPERTY(Editable, Category = "Scatter", ClampMin = 0.001f)
        float ScaleMax = 1.25f;

        /** Offset along the surface normal (world units), to sink/raise instances into the ground. */
        PROPERTY(Editable, Category = "Scatter")
        float ZOffset = 0.0f;

        /** 0 = keep upright, 1 = fully align the up-axis to the surface normal. */
        PROPERTY(Editable, Category = "Scatter", ClampMin = 0.0f, ClampMax = 1.0f)
        float AlignToNormal = 0.6f;

        PROPERTY(Editable, Category = "Scatter")
        bool bRandomYaw = true;

        /** Re-project these instances' height (and re-align) when the terrain underneath is sculpted. */
        PROPERTY(Editable, Category = "Scatter")
        bool bFollowTerrain = true;

        /** Beyond this distance (world units) instances are culled (0 = never). */
        PROPERTY(Editable, Category = "Rendering", ClampMin = 0.0f)
        float CullDistance = 0.0f;

        PROPERTY(Editable, Category = "Rendering")
        bool bCastShadow = true;

        // Also casts into the sun's far cascade, for trees that should shade distant hills.
        PROPERTY(Editable, Category = "Rendering")
        bool bCastFarShadow = false;

        // Past this view distance instances thin out so the count on screen stays level, and the survivors widen to keep the coverage. 0 keeps every instance.
        PROPERTY(Editable, Category = "Rendering", ClampMin = 0.0f, Units = "m")
        float ThinningStartDistance = 0.0f;

        PROPERTY(Editable, Category = "Rendering")
        bool bReceiveShadow = true;

        /** Build static collision for this type's instances while the world simulates (merged into compound bodies). */
        PROPERTY(Editable, Category = "Collision")
        bool bEnableCollision = false;

        // For a mesh with no collision of its own, true builds a convex hull and false a concave triangle mesh.
        PROPERTY(Editable, Category = "Collision")
        bool bConvexCollision = true;

        /** Physics material for this type's bodies; null uses the engine defaults. */
        PROPERTY(Editable, Category = "Collision")
        TStrongObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        /** Layer this type's bodies sit on, and the layers they collide with. */
        PROPERTY(Editable, Category = "Collision")
        SCollisionProfile CollisionProfile = { .Layer = ECollisionProfiles::Static };

        // The mesh's own collision when it has any, else null so the render mesh is used.
        const SMeshCollision* GetCollision() const;

        // Transient resolve cache (not serialized); refreshed by ResolveDirtyMeshComponents.
        uint32          CachedMeshletHeaderSlot = 0;
        EInstanceFlags  CachedBaseFlags = EInstanceFlags::None;
        uint32          ResolveHandle = Constants::kIndexNoneU32;
        // Staleness token of the resolve entry this type last copied from. See SMeshComponent for why
        // this is per-entry rather than a stamp of the cache's global epoch.
        mutable uint32  CachedEntryState = MESH_RESOLVE_STATE_STALE;
        const void*     CachedMeshKey = nullptr;
    };

    // Render-ready instance, baked once when the foliage changes (not per frame). Holds the composed world
    // transform + world-space cull sphere, so the per-frame render path does no matrix/quat/AABB math.
    struct FFoliageBakedInstance
    {
        FMatrix4 Transform;
        FVector4 SphereBounds;   // xyz = center, w = radius
        int32    TypeIndex;      // -1 for an instance the bake skipped, so a baked index is the painted index
    };

    // Holds every foliage instance in the world in one flat array (grouped logically by TypeIndex). Painted
    // and erased by the editor's Foliage mode; drawn through the standard instanced meshlet pipeline.
    //
    // Foliage is mostly static, so the heavy per-instance precompute (transform + bounds) is cached in
    // BakedInstances and only rebuilt when InstancesVersion changes (paint / erase / terrain follow). Static
    // foliage then costs nothing to "keep" frame to frame beyond the GPU cull it already rides.
    REFLECT(Component, Category = "Foliage", HideInComponentList)
    struct RUNTIME_API SFoliageComponent
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Foliage")
        TVector<SFoliageType> Types;

        PROPERTY(Hidden, Category = "Foliage")
        TVector<SFoliageInstance> Instances;


        // Transient render cache, rebuilt when BakedVersion trails InstancesVersion or a bake hit a mesh still loading.
        TVector<FFoliageBakedInstance> BakedInstances;
        uint32 InstancesVersion = 1;            // bumped whenever Instances/Types change
        uint32 BakedVersion     = 0;            // version BakedInstances was built from
        bool   bBakeIncomplete  = false;        // a type's mesh wasn't ready; rebake next frame
        // Separate from BakedVersion, so a released bake rebuilds on the next EnsureRenderCache without a version bump.
        bool   bBakeReleased    = false;
        // Per type, the local bounds a bake places instances with, or an inverted box for a type whose mesh is not ready.
        TVector<SAABB> BakeTypeBounds;

        // Instances a game has hidden at runtime, such as a felled tree, kept apart from Instances so no rebake is owed.
        TVector<uint8>  HiddenInstances;

        // Transient physics bake, owned by SFoliageCollisionSystem. Never serialized.
        uint32 CollisionGroupID     = 0;        // static body group handle, 0 = none
        uint32 CollisionBakedVersion = 0;       // InstancesVersion the bodies were built from

        bool IsValidType(int32 Index) const { return Index >= 0 && Index < Types.Num(); }

        bool IsInstanceHidden(uint32 Index) const { return Index < (uint32)HiddenInstances.Num() && HiddenInstances[Index] != 0; }

        /** Marks the render cache stale so the next frame rebakes. Call after any edit to Instances/Types. */
        void MarkInstancesChanged() { ++InstancesVersion; }

        // A details panel or tool edit rebakes like a paint stroke. With no entity to name, it stales the types so the resolve sweep re-marks the render scene.
        void PostEditChange(const FPropertyChangedEvent& Event);

        void AddInstance(const SFoliageInstance& Instance)
        {
            Instances.Add(Instance);
            MarkInstancesChanged();
        }

        /** Erase instances of a given type within Radius of WorldCenter (XZ distance). Returns the count removed. */
        int32 RemoveInRadius(const FVector3& WorldCenter, float Radius, int32 TypeFilter);

        // Rebuilds BakedInstances from Instances and Types when stale, a no-op when current. Game thread.
        void EnsureRenderCache();

        // Frees the bake once the retained scene holds it; the next EnsureRenderCache rebuilds it.
        void ReleaseRenderCache()
        {
            BakedInstances.Reset();
            BakedInstances.Shrink();
            bBakeReleased = true;
        }
    };

    // Call after any edit to Instances or Types, since a version bump alone rebakes what the render scene is never told to draw.
    RUNTIME_API void MarkFoliageChanged(CWorld& World, ECS::FEntity Entity, SFoliageComponent& Foliage);

    // Hides or shows one painted instance at runtime and tells the render scene, which then flips only that instance's slots.
    RUNTIME_API void SetFoliageInstanceHidden(CWorld& World, ECS::FEntity Entity, SFoliageComponent& Foliage, uint32 Index, bool bHidden);
}
