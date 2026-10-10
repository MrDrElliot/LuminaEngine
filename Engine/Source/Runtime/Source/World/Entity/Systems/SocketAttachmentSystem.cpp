#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "SocketAttachmentSystem.h"

#include "Animation/SkeletalMeshUtils.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/SocketAttachmentComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    namespace
    {
        constexpr uint32 kSocketParallelGrain = 128;

        bool SameTransform(const FTransform& A, const FTransform& B)
        {
            return !(A != B);
        }

        // The bone's bind global with the socket and component offsets folded in, so a posed frame is one multiply.
        bool ResolveSkeletal(const SSkeletalMeshComponent& Mesh, const FSkeletonResource& Skeleton, const SSocketAttachmentComponent& Attachment, FSocketAttachmentCache& Entry)
        {
            int32 Bone = Constants::kIndexNone;
            FMatrix4 SocketOffset;
            if (!SkeletalUtils::ResolveSocket(Mesh, Attachment.SocketName, Bone, SocketOffset) || !Skeleton.IsBoneIndexValid(Bone))
            {
                return false;
            }
            const FMatrix4 BindGlobal = Skeleton.HasBindGlobalMatrices()
                ? Skeleton.BindGlobalMatrices[Bone]
                : Math::Inverse(Skeleton.GetBone(Bone).InvBindMatrix);
            Entry.Bone = Bone;
            Entry.Post = BindGlobal * SocketOffset * Attachment.RelativeTransform.GetMatrix();
            return true;
        }

        bool ResolveStatic(const SStaticMeshComponent& Mesh, const SSocketAttachmentComponent& Attachment, FSocketAttachmentCache& Entry)
        {
            FMatrix4 SocketTransform;
            if (!SkeletalUtils::GetStaticSocketTransform(Mesh, Attachment.SocketName, SocketTransform))
            {
                return false;
            }
            Entry.Bone = Constants::kIndexNone;
            Entry.Post = SocketTransform * Attachment.RelativeTransform.GetMatrix();
            return true;
        }
    }

    void SSocketAttachmentSystem::Configure()
    {
        RequireUpdate(EUpdateStage::PrePhysics, EUpdatePriority::Low);
        RequireUpdate(EUpdateStage::Paused, EUpdatePriority::Low);
        Writes<STransformComponent>();
        Reads<SSocketAttachmentComponent, SSkeletalMeshComponent, SStaticMeshComponent, SystemResource::Hierarchy>();
    }

    void SSocketAttachmentSystem::OnUpdate()
    {
        const FSystemContext& SystemContext = GetContext();

        LUMINA_PROFILE_SCOPE();

        auto View = SystemContext.CreateView<SSocketAttachmentComponent, STransformComponent>(ECS::TExclude<SDisabledTag>{});

        // Sized before the fan-out so every worker writes only its own entities' slots; any included pool bounds the view.
        const ECS::FSparseSet* Driver = View.GetDriver();
        const uint32 MaxIndex = Driver != nullptr ? Driver->MaxLiveIndex() : 0u;
        if (Cache.size() <= MaxIndex)
        {
            Cache.resize(MaxIndex + 1);
        }

        // Editing a socket in the editor changes nothing the cache keys on, so a paused world resolves every update.
        const bool bAlwaysResolve = SystemContext.GetUpdateStage() == EUpdateStage::Paused;

        const auto Attach = [&](ECS::FEntity Entity, const SSocketAttachmentComponent& Attachment, STransformComponent& Transform)
        {
            const ECS::FEntity Parent = SystemContext.GetHierarchy().GetParent(Entity);
            if (Parent == ECS::NullEntity)
            {
                return;
            }

            FSocketAttachmentCache& Entry = Cache[Entity.GetIndex()];
            if (Entry.Entity != Entity)
            {
                Entry = FSocketAttachmentCache{};
                Entry.Entity = Entity;
            }

            FMatrix4 SocketTransform;
            if (const SSkeletalMeshComponent* SkeletalMesh = SystemContext.TryGet<SSkeletalMeshComponent>(Parent))
            {
                const FSkeletonResource* Skeleton = SkeletalUtils::GetSkeleton(*SkeletalMesh);
                if (Skeleton == nullptr)
                {
                    return;
                }
                const bool bStale = bAlwaysResolve || !Entry.bResolved || Entry.Parent != Parent || Entry.Source != Skeleton
                    || Entry.SocketName != Attachment.SocketName || !SameTransform(Entry.Relative, Attachment.RelativeTransform);
                if (bStale)
                {
                    Entry.Parent     = Parent;
                    Entry.Source     = Skeleton;
                    Entry.SocketName = Attachment.SocketName;
                    Entry.Relative   = Attachment.RelativeTransform;
                    Entry.bResolved  = ResolveSkeletal(*SkeletalMesh, *Skeleton, Attachment, Entry);
                    Entry.bWritten   = false;
                }
                if (!Entry.bResolved)
                {
                    return;
                }

                // An unchanged pose, with nobody else having moved the child, leaves nothing to do.
                const bool bPosed = (int32)SkeletalMesh->BoneTransforms.size() == Skeleton->GetNumBones();
                const bool bPoseUnchanged = Entry.PoseSerial == SkeletalMesh->PoseSerial && !SkeletalMesh->bRenderBonesDirty;
                if (Entry.bWritten && bPoseUnchanged && SameTransform(Transform.LocalTransform, Entry.LastLocal))
                {
                    return;
                }
                Entry.PoseSerial = SkeletalMesh->PoseSerial;
                SocketTransform = bPosed ? SkeletalMesh->BoneTransforms[Entry.Bone] * Entry.Post : Entry.Post;
            }
            else if (const SStaticMeshComponent* StaticMesh = SystemContext.TryGet<SStaticMeshComponent>(Parent))
            {
                const void* Source = StaticMesh->StaticMesh.Get();
                const bool bStale = bAlwaysResolve || !Entry.bResolved || Entry.Parent != Parent || Entry.Source != Source
                    || Entry.SocketName != Attachment.SocketName || !SameTransform(Entry.Relative, Attachment.RelativeTransform);
                if (bStale)
                {
                    Entry.Parent     = Parent;
                    Entry.Source     = Source;
                    Entry.SocketName = Attachment.SocketName;
                    Entry.Relative   = Attachment.RelativeTransform;
                    Entry.bResolved  = ResolveStatic(*StaticMesh, Attachment, Entry);
                    Entry.bWritten   = false;
                }
                if (!Entry.bResolved || (Entry.bWritten && SameTransform(Transform.LocalTransform, Entry.LastLocal)))
                {
                    return;
                }
                SocketTransform = Entry.Post;
            }
            else
            {
                return;
            }

            // The equality check keeps static sockets and frozen poses from re-dirtying every frame.
            const FTransform NewLocal(SocketTransform);
            if (NewLocal != Transform.LocalTransform)
            {
                Transform.SetLocalTransform(NewLocal);
            }
            Entry.LastLocal = Transform.LocalTransform;
            Entry.bWritten  = true;
        };

        // Each attachment writes only its own transform and cache slot, and SetLocalTransform is safe from any thread for disjoint entities.
        SystemContext.ParallelForEachView(View, Attach, kSocketParallelGrain, kSocketParallelGrain * 2);
    }
}
