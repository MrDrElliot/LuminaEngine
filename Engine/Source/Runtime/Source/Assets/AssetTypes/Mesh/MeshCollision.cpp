#include "RuntimePCH.h"
#include "MeshCollision.h"

namespace Lumina
{
    bool SMeshCollision::HasCollision() const
    {
        if (bUseTriangleMesh)
        {
            return TriangleIndices.Num() >= 3 && !TriangleVertices.IsEmpty();
        }

        for (const SCollisionPrimitive& Primitive : Primitives)
        {
            // A convex piece with too few points cannot form a volume, so it is not collision.
            if (Primitive.Type != ECollisionPrimitiveType::ConvexHull || Primitive.HullPoints.Num() >= 4)
            {
                return true;
            }
        }

        return false;
    }

    int32 SMeshCollision::NumHullPoints() const
    {
        int32 Total = 0;
        for (const SCollisionPrimitive& Primitive : Primitives)
        {
            if (Primitive.Type == ECollisionPrimitiveType::ConvexHull)
            {
                Total += Primitive.HullPoints.Num();
            }
        }

        return Total;
    }

    void SMeshCollision::Clear()
    {
        Primitives.Reset();
        TriangleVertices.Reset();
        TriangleIndices.Reset();
        bUseTriangleMesh = false;
    }
}
