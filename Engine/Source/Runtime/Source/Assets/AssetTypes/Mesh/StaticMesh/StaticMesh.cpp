#include "RuntimePCH.h"
#include "StaticMesh.h"

#include "Assets/AssetTypes/Physics/CollisionShape.h"

namespace Lumina
{
    const CCollisionShape* CStaticMesh::GetDefaultCollisionShape() const
    {
        const CCollisionShape* Shape = DefaultCollisionShape.Get();
        if (Shape == nullptr || Shape->HasAnyFlag(OF_NeedsLoad) || !Shape->HasCollision())
        {
            return nullptr;
        }
        return Shape;
    }

    bool CStaticMesh::IsDefaultCollisionShapeLoading() const
    {
        const CCollisionShape* Shape = DefaultCollisionShape.Get();
        return Shape != nullptr && Shape->HasAnyFlag(OF_NeedsLoad);
    }
}
