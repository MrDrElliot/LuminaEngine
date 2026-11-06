#include "RuntimePCH.h"

#include "World/Subsystems/AnimationSubsystem.h"

#include "Animation/Pose.h"
#include "Animation/SkeletalMeshUtils.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/FollowerPoseComponent.h"
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
            return World != nullptr ? &World->GetEntityRegistry() : nullptr;
        }
    }

    void CAnimationSubsystem::AttachEntityToSocket(ECS::FEntity Child, ECS::FEntity Parent,
        const FName& SocketOrBone)
    {
        ECS::FRegistry* Registry = RegistryOf(GetWorld());
        if (Registry == nullptr || !Registry->IsValid(Child) || !Registry->IsValid(Parent) || Child == Parent)
        {
            return;
        }

        // The socket system overwrites the local transform anyway, and the snap avoids a stale frame.
        ECS::Utils::ReparentEntity(*Registry, Child, Parent, /*bPreserveWorld*/ false);

        SSocketAttachmentComponent& Attachment = Registry->EmplaceOrReplace<SSocketAttachmentComponent>(Child);
        Attachment.SocketName = SocketOrBone;

        FMatrix4 SocketTransform;
        STransformComponent* Transform = Registry->TryGet<ECS::Write<STransformComponent>>(Child);
        if (Transform && SkeletalUtils::GetEntitySocketTransform(*Registry, Parent, SocketOrBone, SocketTransform))
        {
            Transform->SetLocalTransform(FTransform(SocketTransform * Attachment.RelativeTransform.GetMatrix()));
        }
    }

    void CAnimationSubsystem::DetachEntityFromSocket(ECS::FEntity Entity)
    {
        ECS::FRegistry* Registry = RegistryOf(GetWorld());
        if (Registry == nullptr || !Registry->IsValid(Entity))
        {
            return;
        }

        Registry->Remove<SSocketAttachmentComponent>(Entity);
        ECS::Utils::ReparentEntity(*Registry, Entity, ECS::NullEntity, /*bPreserveWorld*/ true);
    }

    bool CAnimationSubsystem::HasSocket(ECS::FEntity Entity, const FName& SocketOrBone)
    {
        ECS::FRegistry* Registry = RegistryOf(GetWorld());
        return Registry != nullptr && SkeletalUtils::EntityHasSocket(*Registry, Entity, SocketOrBone);
    }

    FVector3 CAnimationSubsystem::GetSocketLocation(ECS::FEntity Entity, const FName& SocketOrBone)
    {
        ECS::FRegistry* Registry = RegistryOf(GetWorld());
        FMatrix4 SocketTransform;
        if (Registry == nullptr
            || !SkeletalUtils::GetSocketWorldTransform(*Registry, Entity, SocketOrBone, SocketTransform))
        {
            return FVector3(0.0f);
        }
        return FVector3(SocketTransform[3]);
    }

    FQuat CAnimationSubsystem::GetSocketRotation(ECS::FEntity Entity, const FName& SocketOrBone)
    {
        ECS::FRegistry* Registry = RegistryOf(GetWorld());
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

    FName CAnimationSubsystem::GetBoneName(ECS::FEntity Entity, int32 BoneIndex)
    {
        ECS::FRegistry* Registry = RegistryOf(GetWorld());
        if (Registry == nullptr || !Registry->IsValid(Entity))
        {
            return FName();
        }

        const SSkeletalMeshComponent* Mesh = Registry->TryGet<ECS::Read<SSkeletalMeshComponent>>(Entity);
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

    int32 CAnimationSubsystem::GetBoneIndex(ECS::FEntity Entity, const FName& BoneName)
    {
        ECS::FRegistry* Registry = RegistryOf(GetWorld());
        if (Registry == nullptr || !Registry->IsValid(Entity))
        {
            return Constants::kIndexNone;
        }

        const SSkeletalMeshComponent* Mesh = Registry->TryGet<ECS::Read<SSkeletalMeshComponent>>(Entity);
        if (Mesh == nullptr)
        {
            return Constants::kIndexNone;
        }

        const FSkeletonResource* Skeleton = SkeletalUtils::GetSkeleton(*Mesh);
        return Skeleton ? Skeleton->FindBoneIndex(BoneName) : Constants::kIndexNone;
    }

    FName CAnimationSubsystem::FindClosestBone(ECS::FEntity Entity, FVector3 WorldLocation)
    {
        ECS::FRegistry* Registry = RegistryOf(GetWorld());
        if (Registry == nullptr)
        {
            return FName();
        }

        const int32 BoneIndex = SkeletalUtils::FindClosestBone(*Registry, Entity, WorldLocation);
        return GetBoneName(Entity, BoneIndex);
    }

    void CAnimationSubsystem::SetLeaderPose(ECS::FEntity Follower, ECS::FEntity Leader)
    {
        ECS::FRegistry* Registry = RegistryOf(GetWorld());
        if (Registry == nullptr || !Registry->IsValid(Follower) || Follower == Leader)
        {
            return;
        }

        if (!Registry->HasAll<SSkeletalMeshComponent>(Follower))
        {
            LOG_WARN("SetLeaderPose: entity {} has no skeletal mesh to drive.", Follower.Value);
            return;
        }

        SFollowerPoseComponent& Link = Registry->EmplaceOrReplace<SFollowerPoseComponent>(Follower);
        Link.Leader = Leader;
    }

    void CAnimationSubsystem::ClearLeaderPose(ECS::FEntity Follower)
    {
        ECS::FRegistry* Registry = RegistryOf(GetWorld());
        if (Registry != nullptr && Registry->IsValid(Follower))
        {
            Registry->Remove<SFollowerPoseComponent>(Follower);
        }
    }

    ECS::FEntity CAnimationSubsystem::GetLeaderPose(ECS::FEntity Follower)
    {
        ECS::FRegistry* Registry = RegistryOf(GetWorld());
        if (Registry == nullptr || !Registry->IsValid(Follower))
        {
            return ECS::NullEntity;
        }
        return SkeletalUtils::ResolvePoseLeader(*Registry, Follower);
    }
}
