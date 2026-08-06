#include "RuntimePCH.h"
#include "CollisionShape.h"

namespace Lumina
{
    bool CCollisionShape::HasCollision() const
    {
        if (bUseTriangleMesh)
        {
            return TriangleIndices.size() >= 3 && !TriangleVertices.empty();
        }

        for (const SCollisionPrimitive& Primitive : Primitives)
        {
            // A convex piece with too few points cannot form a volume, so it is not collision.
            if (Primitive.Type != ECollisionPrimitiveType::ConvexHull || Primitive.HullPoints.size() >= 4)
            {
                return true;
            }
        }

        return false;
    }

    int32 CCollisionShape::NumHullPoints() const
    {
        int32 Total = 0;
        for (const SCollisionPrimitive& Primitive : Primitives)
        {
            if (Primitive.Type == ECollisionPrimitiveType::ConvexHull)
            {
                Total += (int32)Primitive.HullPoints.size();
            }
        }

        return Total;
    }

    void CCollisionShape::ClearGeneratedShapes()
    {
        Primitives.clear();
        TriangleVertices.clear();
        TriangleIndices.clear();
        bUseTriangleMesh = false;
    }
}
