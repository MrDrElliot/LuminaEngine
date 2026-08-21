#include "EditorPCH.h"
#include "CollisionShapeFactory.h"

namespace Lumina
{
    CObject* CCollisionShapeFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        // An empty collision shape has no source mesh, so the useful path is the action on a mesh.
        return NewObject<CCollisionShape>(Package, Name);
    }
}
