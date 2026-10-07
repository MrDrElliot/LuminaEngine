#include "RuntimePCH.h"

#include "SkeletalMeshLibrary.h"

#include "Animation/Pose.h"
#include "Animation/SkeletalMeshUtils.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/SocketAttachmentComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/EntityUtils.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        ECS::FRegistry* RegistryOf(CWorld* World)
        {
            return World != nullptr ? &ECS::GetWorldRegistry(*World) : nullptr;
        }
    }

    void CSkeletalMeshLibrary::AttachEntityToSocket(CWorld* World, ECS::FEntity Child, ECS::FEntity Parent,
        const FName& SocketOrBone)
    {
        ECS::FRegistry* Registry = RegistryOf(World);
        if (Registry == nullptr || !Registry->IsValid(Child) || !Registry->IsValid(Parent) || Child == Parent)
        {
            return;
        }

        // The socket system overwrites the local transform anyway, and the snap avoids a stale frame.
        ECS::Utils::ReparentEntity(*Registry, Child, Parent, /*bPreserveWorld*/ false);

        SSocketAttachmentComponent& Attachment = Registry->EmplaceOrReplace<SSocketAttachmentComponent>(Child);
        Attachment.SocketName = SocketOrBone;

        FMatrix4 SocketTransform;
        STransformComponent* Transform = Registry->TryGet<STransformComponent>(Child);
        if (Transform && SkeletalUtils::GetEntitySocketTransform(*Registry, Parent, SocketOrBone, SocketTransform))
        {
            Transform->SetLocalTransform(FTransform(SocketTransform * Attachment.RelativeTransform.GetMatrix()));
        }
    }

    void CSkeletalMeshLibrary::DetachEntityFromSocket(CWorld* World, ECS::FEntity Entity)
    {
        ECS::FRegistry* Registry = RegistryOf(World);
        if (Registry == nullptr || !Registry->IsValid(Entity))
        {
            return;
        }

        Registry->Remove<SSocketAttachmentComponent>(Entity);
        ECS::Utils::ReparentEntity(*Registry, Entity, ECS::NullEntity, /*bPreserveWorld*/ true);
    }

    bool CSkeletalMeshLibrary::HasSocket(CWorld* World, ECS::FEntity Entity, const FName& SocketOrBone)
    {
        ECS::FRegistry* Registry = RegistryOf(World);
        return Registry != nullptr && SkeletalUtils::EntityHasSocket(*Registry, Entity, SocketOrBone);
    }

    FVector3 CSkeletalMeshLibrary::GetSocketLocation(CWorld* World, ECS::FEntity Entity, const FName& SocketOrBone)
    {
        ECS::FRegistry* Registry = RegistryOf(World);
        FMatrix4 SocketTransform;
        if (Registry == nullptr
            || !SkeletalUtils::GetSocketWorldTransform(*Registry, Entity, SocketOrBone, SocketTransform))
        {
            return FVector3(0.0f);
        }
        return FVector3(SocketTransform[3]);
    }

    FQuat CSkeletalMeshLibrary::GetSocketRotation(CWorld* World, ECS::FEntity Entity, const FName& SocketOrBone)
    {
        ECS::FRegistry* Registry = RegistryOf(World);
        FMatrix4 SocketTransform;
        if (Registry == nullptr
            || !SkeletalUtils::GetSocketWorldTransform(*Registry, Entity, SocketOrBone, SocketTransform))
        {
            return FQuat::Identity();
        }

        FVector3 Translation; FQuat Rotation; FVector3 Scale;
        AnimPose::DecomposeTRS(SocketTransform, Translation, Rotation, Scale);
        return Rotation;
    }

    FName CSkeletalMeshLibrary::GetBoneName(CWorld* World, ECS::FEntity Entity, int32 BoneIndex)
    {
        ECS::FRegistry* Registry = RegistryOf(World);
        if (Registry == nullptr || !Registry->IsValid(Entity))
        {
            return FName();
        }

        const SSkeletalMeshComponent* Mesh = Registry->TryGet<SSkeletalMeshComponent>(Entity);
        if (Mesh == nullptr)
        {
            return FName();
        }

        const FSkeletonResource* Skeleton = SkeletalUtils::GetSkeleton(*Mesh);
        if (Skeleton == nullptr || !Skeleton->IsBoneIndexValid(BoneIndex))
        {
            return FName();
        }
        return Skeleton->GetBone(BoneIndex).Name;
    }

    int32 CSkeletalMeshLibrary::GetBoneIndex(CWorld* World, ECS::FEntity Entity, const FName& BoneName)
    {
        ECS::FRegistry* Registry = RegistryOf(World);
        if (Registry == nullptr || !Registry->IsValid(Entity))
        {
            return Constants::kIndexNone;
        }

        const SSkeletalMeshComponent* Mesh = Registry->TryGet<SSkeletalMeshComponent>(Entity);
        if (Mesh == nullptr)
        {
            return Constants::kIndexNone;
        }

        const FSkeletonResource* Skeleton = SkeletalUtils::GetSkeleton(*Mesh);
        return Skeleton ? Skeleton->FindBoneIndex(BoneName) : Constants::kIndexNone;
    }

    FName CSkeletalMeshLibrary::FindClosestBone(CWorld* World, ECS::FEntity Entity, FVector3 WorldLocation)
    {
        ECS::FRegistry* Registry = RegistryOf(World);
        if (Registry == nullptr)
        {
            return FName();
        }

        const int32 BoneIndex = SkeletalUtils::FindClosestBone(*Registry, Entity, WorldLocation);
        return GetBoneName(World, Entity, BoneIndex);
    }
}
