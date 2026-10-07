#pragma once
#include "EntitySystem.h"
#include "Containers/Name.h"
#include "Core/Math/Transform.h"
#include "Core/Object/ObjectMacros.h"
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
        int32         Bone       = INDEX_NONE;
        uint32        PoseSerial = 0;
        bool          bResolved  = false;
        bool          bWritten   = false;
    };

    // Drives entities with an SSocketAttachmentComponent from their parent's animated skeletal pose.
    // Runs after the animation systems (PrePhysics/Low) so the frame's pose is final, and while paused
    // so editor preview and paused gameplay keep attachments glued.
    REFLECT()
    class RUNTIME_API SSocketAttachmentSystem : public CEntitySystem
    {
        GENERATED_BODY()
    public:

        void Configure() override;

        void OnUpdate() override;

    private:

        TVector<FSocketAttachmentCache> Cache;
    };
}
