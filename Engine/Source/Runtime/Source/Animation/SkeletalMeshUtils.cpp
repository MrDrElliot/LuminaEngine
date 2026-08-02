#include "RuntimePCH.h"
#include "SkeletalMeshUtils.h"

#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Renderer/MeshData.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"

namespace Lumina::SkeletalUtils
{
    CSkeleton* GetSkeletonAsset(const SSkeletalMeshComponent& Mesh)
    {
        if (!Mesh.SkeletalMesh.IsValid())
        {
            return nullptr;
        }
        return Mesh.SkeletalMesh->Skeleton.Get();
    }

    FSkeletonResource* GetSkeleton(const SSkeletalMeshComponent& Mesh)
    {
        CSkeleton* Skeleton = GetSkeletonAsset(Mesh);
        return Skeleton ? Skeleton->GetSkeletonResource() : nullptr;
    }

    bool GetBoneComponentTransform(const SSkeletalMeshComponent& Mesh, int32 BoneIndex, FMatrix4& OutTransform)
    {
        const FSkeletonResource* Skeleton = GetSkeleton(Mesh);
        if (Skeleton == nullptr || !Skeleton->IsBoneIndexValid(BoneIndex))
        {
            return false;
        }

        // Live pose stores Global * InvBind; undo InvBind to recover the component-space global.
        // inverse(InvBind) is also the bind-pose global, so the no-pose fallback is the same undo.
        const FMatrix4 BindGlobal = Math::Inverse(Skeleton->GetBone(BoneIndex).InvBindMatrix);
        if ((int32)Mesh.BoneTransforms.size() == Skeleton->GetNumBones())
        {
            OutTransform = Mesh.BoneTransforms[BoneIndex] * BindGlobal;
        }
        else
        {
            OutTransform = BindGlobal;
        }
        return true;
    }

    bool ResolveSocket(const SSkeletalMeshComponent& Mesh, const FName& SocketOrBone, int32& OutBoneIndex, FMatrix4& OutSocketOffset)
    {
        OutBoneIndex = INDEX_NONE;
        OutSocketOffset = FMatrix4(1.0f);

        const FSkeletonResource* Skeleton = GetSkeleton(Mesh);
        if (Skeleton == nullptr)
        {
            return false;
        }

        if (const CSkeleton* Asset = GetSkeletonAsset(Mesh))
        {
            if (const FMeshSocket* Socket = Asset->FindSocket(SocketOrBone))
            {
                OutBoneIndex = Skeleton->FindBoneIndex(Socket->BoneName);
                OutSocketOffset = Socket->RelativeTransform.GetMatrix();
                return OutBoneIndex != INDEX_NONE;
            }
        }

        OutBoneIndex = Skeleton->FindBoneIndex(SocketOrBone);
        return OutBoneIndex != INDEX_NONE;
    }

    bool GetSocketComponentTransform(const SSkeletalMeshComponent& Mesh, const FName& SocketOrBone, FMatrix4& OutTransform)
    {
        int32 BoneIndex = INDEX_NONE;
        FMatrix4 SocketOffset;
        if (!ResolveSocket(Mesh, SocketOrBone, BoneIndex, SocketOffset))
        {
            return false;
        }

        FMatrix4 BoneTransform;
        if (!GetBoneComponentTransform(Mesh, BoneIndex, BoneTransform))
        {
            return false;
        }

        OutTransform = BoneTransform * SocketOffset;
        return true;
    }

    bool GetStaticSocketTransform(const SStaticMeshComponent& Mesh, const FName& SocketName, FMatrix4& OutTransform)
    {
        if (!Mesh.StaticMesh.IsValid())
        {
            return false;
        }

        const FMeshSocket* Socket = Mesh.StaticMesh->FindSocket(SocketName);
        if (Socket == nullptr)
        {
            return false;
        }

        OutTransform = Socket->RelativeTransform.GetMatrix();
        return true;
    }

    bool GetEntitySocketTransform(FEntityRegistry& Registry, entt::entity Entity, const FName& SocketOrBone, FMatrix4& OutTransform)
    {
        if (!Registry.valid(Entity))
        {
            return false;
        }

        if (const SSkeletalMeshComponent* SkeletalMesh = Registry.try_get<SSkeletalMeshComponent>(Entity))
        {
            return GetSocketComponentTransform(*SkeletalMesh, SocketOrBone, OutTransform);
        }
        if (const SStaticMeshComponent* StaticMesh = Registry.try_get<SStaticMeshComponent>(Entity))
        {
            return GetStaticSocketTransform(*StaticMesh, SocketOrBone, OutTransform);
        }
        return false;
    }

    bool EntityHasSocket(FEntityRegistry& Registry, entt::entity Entity, const FName& SocketOrBone)
    {
        if (!Registry.valid(Entity))
        {
            return false;
        }

        if (const SSkeletalMeshComponent* SkeletalMesh = Registry.try_get<SSkeletalMeshComponent>(Entity))
        {
            int32 BoneIndex = INDEX_NONE;
            FMatrix4 SocketOffset;
            return ResolveSocket(*SkeletalMesh, SocketOrBone, BoneIndex, SocketOffset);
        }
        if (const SStaticMeshComponent* StaticMesh = Registry.try_get<SStaticMeshComponent>(Entity))
        {
            return StaticMesh->StaticMesh.IsValid() && StaticMesh->StaticMesh->FindSocket(SocketOrBone) != nullptr;
        }
        return false;
    }

    bool GetSocketWorldTransform(FEntityRegistry& Registry, entt::entity Entity, const FName& SocketOrBone, FMatrix4& OutTransform)
    {
        FMatrix4 ComponentTransform;
        if (!GetEntitySocketTransform(Registry, Entity, SocketOrBone, ComponentTransform))
        {
            return false;
        }

        STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity);
        if (Transform == nullptr)
        {
            return false;
        }

        OutTransform = Transform->GetWorldMatrix() * ComponentTransform;
        return true;
    }

    int32 FindClosestBone(FEntityRegistry& Registry, entt::entity Entity, const FVector3& WorldPoint)
    {
        if (!Registry.valid(Entity))
        {
            return INDEX_NONE;
        }

        const SSkeletalMeshComponent* Mesh = Registry.try_get<SSkeletalMeshComponent>(Entity);
        STransformComponent* Transform = Registry.try_get<STransformComponent>(Entity);
        if (Mesh == nullptr || Transform == nullptr)
        {
            return INDEX_NONE;
        }

        const FSkeletonResource* Skeleton = GetSkeleton(*Mesh);
        if (Skeleton == nullptr || Skeleton->GetNumBones() == 0)
        {
            return INDEX_NONE;
        }

        // Compare in component space: one inverse instead of a matrix multiply per bone.
        const FMatrix4 WorldToComponent = Math::Inverse(Transform->GetWorldMatrix());
        const FVector3 LocalPoint = FVector3(WorldToComponent * FVector4(WorldPoint, 1.0f));

        const bool bLivePose = (int32)Mesh->BoneTransforms.size() == Skeleton->GetNumBones();

        int32 Closest = INDEX_NONE;
        float BestDistSq = FLT_MAX;
        for (int32 i = 0; i < Skeleton->GetNumBones(); ++i)
        {
            const FMatrix4 BindGlobal = Math::Inverse(Skeleton->GetBone(i).InvBindMatrix);
            const FMatrix4 Global = bLivePose ? Mesh->BoneTransforms[i] * BindGlobal : BindGlobal;
            const float DistSq = Math::LengthSquared(FVector3(Global[3]) - LocalPoint);
            if (DistSq < BestDistSq)
            {
                BestDistSq = DistSq;
                Closest = i;
            }
        }
        return Closest;
    }

    void PackRenderBones(const TVector<FMatrix4>& BoneTransforms, TVector<FVector4>& OutRows)
    {
        LUMINA_PROFILE_SCOPE();
        static_assert(sizeof(FBoneTransform) == 3 * sizeof(FVector4), "RenderBones rows must alias FBoneTransform");

        const SIZE_T NumBones = BoneTransforms.size();
        OutRows.resize(NumBones * 3);

        FBoneTransform* RESTRICT Out = reinterpret_cast<FBoneTransform*>(OutRows.data());
        for (SIZE_T i = 0; i < NumBones; ++i)
        {
            Out[i] = PackBoneTransform(BoneTransforms[i]);
        }
    }
}
