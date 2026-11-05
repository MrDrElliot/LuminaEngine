#pragma once
#include "EntitySystem.h"
#include "Containers/Name.h"
#include "Core/Math/Transform.h"
#include "Core/Object/ObjectMacros.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/Entity/Components/SkeletalMeshComponent.h"
#include "World/Entity/Components/SocketAttachmentComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "SocketAttachmentSystem.generated.h"

namespace Lumina
{

    // What an attachment resolved to last time, indexed by entity, so a frame only redoes the name lookups when something changed.
    struct FSocketAttachmentCache
    {
        ECS::FEntity  Entity     = ECS::NullEntity;
        ECS::FEntity  Parent     = ECS::NullEntity;
        const void*   Source     = nullptr;
        FName         SocketName;
        FTransform    Relative;
        FMatrix4      Post       = FMatrix4(1.0f);
        FTransform    LastLocal;
        int32         Bone       = Constants::kIndexNone;
        uint32        PoseSerial = 0;
        bool          bResolved  = false;
        bool          bWritten   = false;
    };

    // Drives entities with an SSocketAttachmentComponent from their parent's final pose this frame, paused worlds included.
    REFLECT()
    class RUNTIME_API SSocketAttachmentSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        // Only the attachments, since the parent lookups match almost every mesh.
        bool HasWork(const SSystemContext& Context) override;

        void OnUpdate(const SSystemContext& Context) override;

    private:

        ECS::TQuery<ECS::Read<SSocketAttachmentComponent>, ECS::Write<STransformComponent>, ECS::Without<SDisabledTag>> Attachments;
        ECS::TQuery<ECS::Read<SSkeletalMeshComponent>> SkeletalParents;
        ECS::TQuery<ECS::Read<SStaticMeshComponent>> StaticParents;
        TVector<FSocketAttachmentCache> Cache;
    };
}
