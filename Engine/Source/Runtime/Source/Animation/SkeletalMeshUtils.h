#pragma once

#include "Containers/Vector.h"
#include "Containers/Name.h"
#include "Core/Math/Math.h"
#include "Platform/GenericPlatform.h"
#include "World/Entity/Registry/EntityRegistry.h"
#include "Renderer/SkeletonResource.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

namespace Lumina
{
    class CSkeleton;
    struct FSkeletonResource;
    struct SSkeletalMeshComponent;
    struct SStaticMeshComponent;
}

namespace Lumina::SkeletalUtils
{
    // Skeleton behind a skeletal mesh component; null while the mesh or its skeleton isn't loaded.
    RUNTIME_API FSkeletonResource* GetSkeleton(const SSkeletalMeshComponent& Mesh);
    RUNTIME_API CSkeleton* GetSkeletonAsset(const SSkeletalMeshComponent& Mesh);

    // Component-space (mesh-entity-relative) bone transform from the live pose, or the bind pose when
    // no pose has been evaluated yet. False when the bone index is out of range.
    RUNTIME_API bool GetBoneComponentTransform(const SSkeletalMeshComponent& Mesh, int32 BoneIndex, FMatrix4& OutTransform);

    // Resolve a socket-or-bone name: a socket on the skeleton wins (its bone + authored offset),
    // else a raw bone name (identity offset). False when neither exists.
    RUNTIME_API bool ResolveSocket(const SSkeletalMeshComponent& Mesh, const FName& SocketOrBone, int32& OutBoneIndex, FMatrix4& OutSocketOffset);

    RUNTIME_API bool GetSocketComponentTransform(const SSkeletalMeshComponent& Mesh, const FName& SocketOrBone, FMatrix4& OutTransform);

    // Static mesh socket transform (fixed, relative to the mesh origin). False when the socket doesn't exist.
    RUNTIME_API bool GetStaticSocketTransform(const SStaticMeshComponent& Mesh, const FName& SocketName, FMatrix4& OutTransform);

    // Component-space socket transform on an entity with either mesh component type
    // (skeletal: animated bone or socket; static: authored socket).
    RUNTIME_API bool GetEntitySocketTransform(FEntityRegistry& Registry, entt::entity Entity, const FName& SocketOrBone, FMatrix4& OutTransform);

    RUNTIME_API bool EntityHasSocket(FEntityRegistry& Registry, entt::entity Entity, const FName& SocketOrBone);

    // World-space socket-or-bone transform on Entity (folds in the entity's world matrix).
    RUNTIME_API bool GetSocketWorldTransform(FEntityRegistry& Registry, entt::entity Entity, const FName& SocketOrBone, FMatrix4& OutTransform);

    // Bone whose origin lies nearest WorldPoint; approximates the hit bone for meshes with a single
    // physics body (per-bone hits only come from ragdoll bodies). INDEX_NONE without a skeleton.
    RUNTIME_API int32 FindClosestBone(FEntityRegistry& Registry, entt::entity Entity, const FVector3& WorldPoint);

    // Packs skinning matrices into the GPU 3x4-row layout (FBoneTransform: 3 FVector4 rows per bone)
    // consumed by the render gather. Writers of SSkeletalMeshComponent.BoneTransforms either call this
    // into Mesh.RenderBones or set Mesh.bRenderBonesDirty so the gather repacks lazily.
    RUNTIME_API void PackRenderBones(const TVector<FMatrix4>& BoneTransforms, TVector<FBoneTransform>& OutBones);
}
