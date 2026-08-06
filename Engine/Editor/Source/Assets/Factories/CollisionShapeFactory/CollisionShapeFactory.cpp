#include "EditorPCH.h"
#include "CollisionShapeFactory.h"

namespace Lumina
{
    CObject* CCollisionShapeFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        // No creation dialogue: an empty collision shape has no source mesh to bind to, and the useful
        // path is the "Create Collision Shape" action on a mesh, which sets it.
        return NewObject<CCollisionShape>(Package, Name);
    }
}
