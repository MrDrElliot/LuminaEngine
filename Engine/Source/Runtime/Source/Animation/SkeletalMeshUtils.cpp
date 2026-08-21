#include "RuntimePCH.h"
#include "SkeletalMeshUtils.h"

#include "Core/Math/SIMD/PackHalf.h"
#include "Core/Math/SIMD/SIMD.h"

#include "Assets/AssetTypes/Mesh/SkeletalMesh/SkeletalMesh.h"
#include "Assets/AssetTypes/Mesh/Skeleton/Skeleton.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Renderer/MeshData.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Scene/RenderScene/SceneRenderTypes.h"
#include "Renderer/SkeletonResource.h"

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

    void PackRenderBones(const TVector<FMatrix4>& BoneTransforms, TVector<FBoneTransform>& OutBones)
    {
        LUMINA_PROFILE_SCOPE();

        const uint32 NumBones = (uint32)BoneTransforms.size();
        OutBones.resize(NumBones);

        FBoneTransform* RESTRICT Out = OutBones.data();
        const FMatrix4* RESTRICT   Src = BoneTransforms.data();

        // Chunked to keep the staging on the stack, and long enough for the wide half-conversion path.
        constexpr uint32 kChunk = 64;
        alignas(16) float  RowScratch[kChunk * 10];
        uint32             PackScratch[kChunk * 5];

        for (uint32 Base = 0; Base < NumBones; Base += kChunk)
        {
            const uint32 Count = Math::Min(kChunk, NumBones - Base);

            for (uint32 n = 0; n < Count; ++n)
            {
                // Columns in, mathematical rows out; the packed layout is row-major.
                __m128 R0 = SIMD::VFloat4::Load(&Src[Base + n][0].x);
                __m128 R1 = SIMD::VFloat4::Load(&Src[Base + n][1].x);
                __m128 R2 = SIMD::VFloat4::Load(&Src[Base + n][2].x);
                __m128 R3 = SIMD::VFloat4::Load(&Src[Base + n][3].x);
                _MM_TRANSPOSE4_PS(R0, R1, R2, R3);

                alignas(16) float Rows[12];
                SIMD::VFloat4(R0).StoreAligned(Rows + 0);
                SIMD::VFloat4(R1).StoreAligned(Rows + 4);
                SIMD::VFloat4(R2).StoreAligned(Rows + 8);

                float* RESTRICT Dst = RowScratch + n * 10;
                Dst[0] = Rows[0]; Dst[1] = Rows[1]; Dst[2] = Rows[2];
                Dst[3] = Rows[4]; Dst[4] = Rows[5]; Dst[5] = Rows[6];
                Dst[6] = Rows[8]; Dst[7] = Rows[9]; Dst[8] = Rows[10];
                Dst[9] = 0.0f;

                // The w lane of each row is the translation, which stays at full precision.
                Out[Base + n].Tx = Rows[3];
                Out[Base + n].Ty = Rows[7];
                Out[Base + n].Tz = Rows[11];
            }

            SIMD::PackHalf2x16Array(RowScratch, PackScratch, Count * 5);

            for (uint32 n = 0; n < Count; ++n)
            {
                const uint32* RESTRICT Packed = PackScratch + n * 5;
                uint32* RESTRICT       Rot    = Out[Base + n].Rot;
                Rot[0] = Packed[0];
                Rot[1] = Packed[1];
                Rot[2] = Packed[2];
                Rot[3] = Packed[3];
                Rot[4] = Packed[4];
            }
        }
    }
}
