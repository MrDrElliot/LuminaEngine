#pragma once

#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Containers/Array.h"
#include "Core/Math/Math.h"
#include "Core/Math/Transform.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "World/Entity/Registry/EntityRegistry.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "FoliageComponent.generated.h"

namespace Lumina
{
    class CWorld;
    class CCollisionShape;
    class CPhysicsMaterial;

    // One painted foliage instance in WORLD space, with Rotation a quaternion laid out as (x, y, z, w).
    REFLECT()
    struct RUNTIME_API SFoliageInstance
    {
        GENERATED_BODY()

        PROPERTY()
        FVector3 Position = FVector3(0.0f);

        PROPERTY()
        FVector4 Rotation = FVector4(0.0f, 0.0f, 0.0f, 1.0f);

        PROPERTY()
        FVector3 Scale = FVector3(1.0f);

        /** Index into the owning component's Types array. */
        PROPERTY()
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
        TObjectPtr<CStaticMesh> Mesh;

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

        PROPERTY(Editable, Category = "Rendering")
        bool bReceiveShadow = true;

        /** Build static collision for this type's instances while the world simulates (merged into compound bodies). */
        PROPERTY(Editable, Category = "Collision")
        bool bEnableCollision = false;

        /** Authored collision used per instance; null falls back to collision built from the mesh. */
        PROPERTY(Editable, Category = "Collision")
        TObjectPtr<CCollisionShape> CollisionShape;

        /** Mesh fallback only: true builds a convex hull, false a concave triangle mesh. */
        PROPERTY(Editable, Category = "Collision")
        bool bConvexCollision = true;

        /** Physics material for this type's bodies; null uses the engine defaults. */
        PROPERTY(Editable, Category = "Collision")
        TObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        // Transient resolve cache (not serialized); refreshed by ResolveDirtyMeshComponents.
        uint32          CachedMeshletHeaderSlot = 0;
        EInstanceFlags  CachedBaseFlags = EInstanceFlags::None;
        uint32          ResolveHandle = ~0u;
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
        int32     TypeIndex;
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

        PROPERTY(Category = "Foliage")
        TVector<SFoliageInstance> Instances;

        // Transient: last terrain HeightmapVersion this component re-projected against (terrain follow).
        uint32 LastTerrainVersion = 0;

        // Transient render cache. BakedInstances is regenerated only when BakedVersion != InstancesVersion
        // (or a previous bake hit a not-yet-loaded mesh). Never serialized.
        TVector<FFoliageBakedInstance> BakedInstances;
        uint32 InstancesVersion = 1;            // bumped whenever Instances/Types change
        uint32 BakedVersion     = 0;            // version BakedInstances was built from
        bool   bBakeIncomplete  = false;        // a type's mesh wasn't ready; rebake next frame

        // Transient physics bake, owned by SFoliageCollisionSystem. Never serialized.
        uint32 CollisionGroupID     = 0;        // static body group handle, 0 = none
        uint32 CollisionBakedVersion = 0;       // InstancesVersion the bodies were built from

        bool IsValidType(int32 Index) const { return Index >= 0 && Index < (int32)Types.size(); }

        /** Marks the render cache stale so the next frame rebakes. Call after any edit to Instances/Types. */
        void MarkInstancesChanged() { ++InstancesVersion; }

        void AddInstance(const SFoliageInstance& Instance)
        {
            Instances.push_back(Instance);
            MarkInstancesChanged();
        }

        /** Erase instances of a given type within Radius of WorldCenter (XZ distance). Returns the count removed. */
        int32 RemoveInRadius(const FVector3& WorldCenter, float Radius, int32 TypeFilter);

        /** Rebuild BakedInstances from Instances/Types if the cache is stale. Cheap no-op when up to date.
         *  Game-thread only (called during render-command compile). */
        void EnsureRenderCache();
    };

    /** Call after ANY edit to a foliage component's Instances or Types.
     *
     *  Foliage has two independent caches and an edit invalidates both, which is exactly the trap this
     *  exists to close:
     *
     *    1. MarkInstancesChanged() bumps InstancesVersion, which rebuilds BakedInstances (the CPU-side
     *       transform + bounds bake).
     *    2. The render dirty tracker is what makes FScenePrimitiveSet::SyncFoliage run, and that is what
     *       actually adds or removes primitives in the retained scene.
     *
     *  Doing only (1) bakes instances nothing ever draws. That was the painting bug: newly painted foliage
     *  stayed invisible until some unrelated change happened to force a scene sync. Always use this rather
     *  than calling MarkInstancesChanged() directly. */
    RUNTIME_API void MarkFoliageChanged(CWorld& World, entt::entity Entity, SFoliageComponent& Foliage);
}
