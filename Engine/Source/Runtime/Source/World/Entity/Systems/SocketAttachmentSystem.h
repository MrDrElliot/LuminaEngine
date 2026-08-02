#pragma once
#include "EntitySystem.h"
#include "Core/Object/ObjectMacros.h"
#include "SocketAttachmentSystem.generated.h"

namespace Lumina
{
    // Drives entities with an SSocketAttachmentComponent from their parent's animated skeletal pose.
    // Runs after the animation systems (PrePhysics/Low) so the frame's pose is final, and while paused
    // so editor preview and paused gameplay keep attachments glued.
    REFLECT(System)
    struct RUNTIME_API SSocketAttachmentSystem
    {
        GENERATED_BODY()
        ENTITY_SYSTEM(RequiresUpdate(EUpdateStage::PrePhysics, EUpdatePriority::Low),
                      RequiresUpdate(EUpdateStage::Paused, EUpdatePriority::Low))

        // Writes the attached entity's transform; reads the parent's mesh/pose. Defined in the .cpp.
        static FSystemAccess Access;

        static void Update(const FSystemContext& SystemContext) noexcept;
    };
}
