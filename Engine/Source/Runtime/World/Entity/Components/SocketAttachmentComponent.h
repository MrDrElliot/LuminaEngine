#pragma once
#include "Containers/Name.h"
#include "Core/Math/Transform.h"
#include "Core/Object/ObjectMacros.h"
#include "SocketAttachmentComponent.generated.h"

namespace Lumina
{
    /** Glues this entity to a socket on its parent entity's mesh: skeletal parents follow the animated
     *  bone (socket or raw bone name), static mesh parents follow the authored socket. Attach via
     *  CWorld::AttachEntityToSocket (parents + snaps in one call) or add manually to an entity that is
     *  already a child of a mesh entity. */
    REFLECT(Component, Category = "Animation")
    struct RUNTIME_API SSocketAttachmentComponent
    {
        GENERATED_BODY()

        /** Socket (or skeletal bone) on the parent's mesh to follow. */
        PROPERTY(Editable, Category = "Attachment", SocketPicker)
        FName SocketName;

        /** Additional offset applied in socket space. */
        PROPERTY(Editable, Category = "Attachment")
        FTransform RelativeTransform;
    };
}
