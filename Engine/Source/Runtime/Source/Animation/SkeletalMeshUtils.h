#pragma once

#include "World/ECS/Hierarchy.h"
#include "Containers/Name.h"
#include "Core/Math/Math.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CSkeleton;
    struct FPackedBoneTransform;
    struct FSkeletonResource;
    struct SFollowerPoseComponent;
    struct SSkeletalMeshComponent;
    struct SStaticMeshComponent;
    namespace ECS { class FRegistry; }
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
    RUNTIME_API bool GetEntitySocketTransform(ECS::FRegistry& Registry, ECS::FEntity Entity, const FName& SocketOrBone, FMatrix4& OutTransform);

    RUNTIME_API bool EntityHasSocket(ECS::FRegistry& Registry, ECS::FEntity Entity, const FName& SocketOrBone);

    // The entity whose pose Follower copies, at the end of its leader chain, or null when it follows nothing or the chain loops.
    RUNTIME_API ECS::FEntity ResolvePoseLeader(const ECS::FRegistry& Registry, ECS::FEntity Follower);

    // The same walk through a system's lookups: Links reaches SFollowerPoseComponent and Meshes SSkeletalMeshComponent.
    template<typename TLinks, typename TMeshes, typename TIsValid>
    ECS::FEntity ResolvePoseLeader(const TLinks& Links, const TMeshes& Meshes, const ECS::FHierarchy& Hierarchy, TIsValid&& IsValid, ECS::FEntity Follower)
    {
        // Deeper than any real outfit, and what stops a cycle.
        constexpr int32 kMaxHops = 8;

        ECS::FEntity Cursor = Follower;
        for (int32 Hop = 0; Hop < kMaxHops; ++Hop)
        {
            const auto* Link = Links.template TryGet<SFollowerPoseComponent>(Cursor);
            if (Link == nullptr)
            {
                return Cursor == Follower ? ECS::NullEntity : Cursor;
            }

            ECS::FEntity Next = Link->Leader;
            if (Next == ECS::NullEntity)
            {
                // An unset leader is the nearest ancestor with a skeletal mesh, which is how a modular piece is usually attached.
                for (ECS::FEntity Parent = Hierarchy.GetParent(Cursor); Parent != ECS::NullEntity; Parent = Hierarchy.GetParent(Parent))
                {
                    if (Meshes.template TryGet<SSkeletalMeshComponent>(Parent) != nullptr)
                    {
                        Next = Parent;
                        break;
                    }
                }
            }

            // A chain that stops on another follower has no leader, since that follower's pose is written in the same pass.
            if (Next == ECS::NullEntity || !IsValid(Next))
            {
                return ECS::NullEntity;
            }
            if (Next == Follower || Next == Cursor)
            {
                return ECS::NullEntity;
            }
            Cursor = Next;
        }
        return ECS::NullEntity;
    }

    // World-space socket-or-bone transform on Entity (folds in the entity's world matrix).
    RUNTIME_API bool GetSocketWorldTransform(ECS::FRegistry& Registry, ECS::FEntity Entity, const FName& SocketOrBone, FMatrix4& OutTransform);

    // Bone whose origin lies nearest WorldPoint; approximates the hit bone for meshes with a single
    // physics body (per-bone hits only come from ragdoll bodies). Constants::kIndexNone without a skeleton.
    RUNTIME_API int32 FindClosestBone(ECS::FRegistry& Registry, ECS::FEntity Entity, const FVector3& WorldPoint);

    // Packs NumBones skinning matrices into the 32-byte GPU layout the skinning shaders read. OutBones is
    // written straight through, so the render gather aims this at its own arena slice and stages nothing.
    RUNTIME_API void PackRenderBones(const FMatrix4* BoneTransforms, uint32 NumBones, FPackedBoneTransform* OutBones);
}
