#include "RuntimePCH.h"
#include "SocketAttachmentSystem.h"

#include "Animation/SkeletalMeshUtils.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/SocketAttachmentComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina
{
    FSystemAccess SSocketAttachmentSystem::Access = FSystemAccess{}
        .Write<STransformComponent>()
        .Read<SSocketAttachmentComponent, SSkeletalMeshComponent, SStaticMeshComponent, FRelationshipComponent>();

    void SSocketAttachmentSystem::Update(const FSystemContext& SystemContext) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        auto View = SystemContext.CreateView<SSocketAttachmentComponent, STransformComponent>(entt::exclude<SDisabledTag>);

        for (entt::entity Entity : View)
        {
            const FRelationshipComponent* Relationship = SystemContext.TryGet<FRelationshipComponent>(Entity);
            if (Relationship == nullptr || Relationship->Parent == entt::null)
            {
                continue;
            }

            const SSocketAttachmentComponent& Attachment = View.get<SSocketAttachmentComponent>(Entity);

            FMatrix4 SocketTransform;
            bool bResolved = false;
            if (const SSkeletalMeshComponent* SkeletalMesh = SystemContext.TryGet<SSkeletalMeshComponent>(Relationship->Parent))
            {
                bResolved = SkeletalUtils::GetSocketComponentTransform(*SkeletalMesh, Attachment.SocketName, SocketTransform);
            }
            else if (const SStaticMeshComponent* StaticMesh = SystemContext.TryGet<SStaticMeshComponent>(Relationship->Parent))
            {
                bResolved = SkeletalUtils::GetStaticSocketTransform(*StaticMesh, Attachment.SocketName, SocketTransform);
            }

            if (!bResolved)
            {
                continue;
            }

            // The equality check keeps static sockets and frozen poses from re-dirtying every frame.
            const FTransform NewLocal(SocketTransform * Attachment.RelativeTransform.GetMatrix());
            STransformComponent& Transform = View.get<STransformComponent>(Entity);
            if (NewLocal != Transform.LocalTransform)
            {
                Transform.SetLocalTransform(NewLocal);
            }
        }
    }
}
