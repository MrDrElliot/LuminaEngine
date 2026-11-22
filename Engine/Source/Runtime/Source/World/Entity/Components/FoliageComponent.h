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

    // One foliage instance in world space, with Rotation a quaternion laid out as (x, y, z, w).
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

        // Into the owning component's Types.
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

    // One kind of plant, the mesh every instance draws plus how it scatters and renders.
    REFLECT()
    struct RUNTIME_API SFoliageType
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Foliage Type")
        FString Name;

        PROPERTY(Editable, Category = "Foliage Type")
        TStrongObjectPtr<CStaticMesh> Mesh;

        // Plants per square meter at full brush strength or full forest cover.
        PROPERTY(Editable, Category = "Scatter", ClampMin = 0.0001f)
        float Density = 0.05f;

        PROPERTY(Editable, Category = "Scatter", ClampMin = 0.001f)
        float ScaleMin = 0.85f;

        PROPERTY(Editable, Category = "Scatter", ClampMin = 0.001f)
        float ScaleMax = 1.25f;

        // Along the surface normal, to sink plants into the ground or raise them out of it.
        PROPERTY(Editable, Category = "Scatter")
        float ZOffset = 0.0f;

        // From 0 for upright to 1 for the up axis on the surface normal.
        PROPERTY(Editable, Category = "Scatter", ClampMin = 0.0f, ClampMax = 1.0f)
        float AlignToNormal = 0.6f;

        PROPERTY(Editable, Category = "Scatter")
        bool bRandomYaw = true;

        // Regrounds and realigns the plants when the terrain under them is sculpted.
        PROPERTY(Editable, Category = "Scatter")
        bool bFollowTerrain = true;

        // Plants past this view distance are culled, 0 for never.
        PROPERTY(Editable, Category = "Rendering", ClampMin = 0.0f)
        float CullDistance = 0.0f;

        PROPERTY(Editable, Category = "Rendering")
        bool bCastShadow = true;

        // Past this view distance instances thin out so the count on screen stays level, and the survivors widen to keep the coverage. 0 keeps every instance.
        PROPERTY(Editable, Category = "Rendering", ClampMin = 0.0f, Units = "m")
        float ThinningStartDistance = 0.0f;

        // Shadows follow the material's world position offset. Off casts from the still mesh, which is cheaper and keeps the shadow cacheable.
        PROPERTY(Editable, Category = "Rendering")
        bool bShadowWorldPositionOffset = true;

        // Past this camera distance the material's world position offset is skipped in every pass. 0 always applies it.
        PROPERTY(Editable, Category = "Rendering", ClampMin = 0.0f, Units = "m")
        float WorldPositionOffsetDisableDistance = 0.0f;

        PROPERTY(Editable, Category = "Rendering")
        bool bReceiveShadow = true;

        // Builds static collision for these plants while the world simulates, merged into compound bodies.
        PROPERTY(Editable, Category = "Collision")
        bool bEnableCollision = false;

        // For a mesh with no collision of its own, true builds a convex hull and false a concave triangle mesh.
        PROPERTY(Editable, Category = "Collision")
        bool bConvexCollision = true;

        // Null uses the engine's default physics material.
        PROPERTY(Editable, Category = "Collision")
        TStrongObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        PROPERTY(Editable, Category = "Collision")
        SCollisionProfile CollisionProfile = { .Layer = ECollisionProfiles::Static };

        // The mesh's own collision when it has any, else null so the render mesh is used.
        const SMeshCollision* GetCollision() const;

        // Transient resolve cache, refreshed by ResolveDirtyMeshComponents and never serialized.
        uint32          CachedMeshletHeaderSlot = 0;
        EInstanceFlags  CachedBaseFlags = EInstanceFlags::None;
        uint32          ResolveHandle = Constants::kIndexNoneU32;
        // Staleness token of the resolve entry this type last copied from, per entry for the reason SMeshComponent gives.
        mutable uint32  CachedEntryState = MESH_RESOLVE_STATE_STALE;
        const void*     CachedMeshKey = nullptr;
    };

    // An instance's world transform and cull sphere, baked when the foliage changes so drawing does no transform math.
    struct FFoliageBakedInstance
    {
        FMatrix4 Transform;
        // Center in xyz and radius in w.
        FVector4 SphereBounds;
        // Negative for an instance the bake skipped, so a baked index is the painted index.
        int32    TypeIndex;
    };

    // Plants as one flat instance list, the renderer a forest streams its tiles through, rebaked only when InstancesVersion moves.
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
        // Bumped whenever Instances or Types change.
        uint32 InstancesVersion = 1;
        uint32 BakedVersion     = 0;
        // A type's mesh was still loading, so the next frame bakes again.
        bool   bBakeIncomplete  = false;
        // Separate from BakedVersion, so a released bake rebuilds on the next EnsureRenderCache without a version bump.
        bool   bBakeReleased    = false;
        // Per type, the local bounds a bake places instances with, or an inverted box for a type whose mesh is not ready.
        TVector<SAABB> BakeTypeBounds;

        // Instances a game has hidden at runtime, such as a felled tree, kept apart from Instances so no rebake is owed.
        TVector<uint8>  HiddenInstances;

        // False keeps instances, collision and nav but leaves the whole set out of the render scene. Change it through SetFoliageRendered.
        bool bRendered = true;

        // Set before the first sync on a far forest tile, so every type with an impostor draws as bare points that never become meshes.
        bool bImpostorPoints = false;

        // Transient physics bake owned by CFoliageCollisionSystem, with a static body group handle of 0 for none.
        uint32 CollisionGroupID     = 0;
        uint32 CollisionBakedVersion = 0;

        bool IsValidType(int32 Index) const { return Index >= 0 && Index < Types.Num(); }

        bool IsInstanceHidden(uint32 Index) const { return Index < (uint32)HiddenInstances.Num() && HiddenInstances[Index] != 0; }

        // Stales the render cache so the next frame rebakes, after any edit to Instances or Types.
        void MarkInstancesChanged() { ++InstancesVersion; }

        // A details panel or tool edit rebakes like a paint stroke. With no entity to name, it stales the types so the resolve sweep re-marks the render scene.
        void PostEditChange(const FPropertyChangedEvent& Event);

        void AddInstance(const SFoliageInstance& Instance)
        {
            Instances.Add(Instance);
            MarkInstancesChanged();
        }

        // Erases the type's plants within Radius of WorldCenter on the ground plane and returns how many went.
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

    // Adds or drops the whole set in the render scene without a version bump, so collision and nav never rebuild for it.
    RUNTIME_API void SetFoliageRendered(CWorld& World, ECS::FEntity Entity, SFoliageComponent& Foliage, bool bRendered);
}
