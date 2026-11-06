#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "MeshCollision.generated.h"

namespace Lumina
{
    class CPhysicsMaterial;

    REFLECT()
    enum class ECollisionPrimitiveType : uint8
    {
        Box,
        Sphere,
        Capsule,
        ConvexHull,
    };

    // One piece of a mesh's collision; convex pieces can simulate dynamically, which is why collision is split into pieces at all.
    REFLECT()
    struct RUNTIME_API SCollisionPrimitive
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Shape")
        ECollisionPrimitiveType Type = ECollisionPrimitiveType::Box;

        // Offset from the mesh origin, in mesh space.
        PROPERTY(Editable, Category = "Shape")
        FVector3 Center = FVector3(0.0f);

        // Euler rotation offset in degrees, ignored by a sphere.
        PROPERTY(Editable, Category = "Shape")
        FVector3 Rotation = FVector3(0.0f);

        PROPERTY(Editable, Category = "Shape|Box")
        FVector3 HalfExtent = FVector3(0.5f);

        PROPERTY(Editable, ClampMin = 0.001f, Category = "Shape|Sphere & Capsule")
        float Radius = 0.5f;

        // Half the capsule's cylinder, so its full height is 2 * (HalfHeight + Radius).
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Shape|Sphere & Capsule")
        float HalfHeight = 0.5f;

        // Already reduced to the hull's own vertices, and hidden because a grid of hundreds of points is noise.
        PROPERTY(Hidden)
        TVector<FVector3> HullPoints;
    };

    // A static mesh's simplified collision, a set of primitives and hulls or a baked triangle mesh, edited in the mesh's Collision tab.
    REFLECT()
    struct RUNTIME_API SMeshCollision
    {
        GENERATED_BODY()

        // Surface response for every piece; null falls back to the rigid body's override fields.
        PROPERTY(Editable, Category = "Collision")
        TStrongObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        // Use the baked triangles instead of the primitives, exact but restricted to static and kinematic bodies.
        PROPERTY(Editable, Category = "Collision")
        bool bUseTriangleMesh = false;

        // Edited through the Collision tab's list and viewport handles rather than a property grid.
        PROPERTY(Hidden)
        TVector<SCollisionPrimitive> Primitives;

        PROPERTY(Hidden)
        TVector<FVector3> TriangleVertices;

        PROPERTY(Hidden)
        TVector<uint32> TriangleIndices;

        // False for a set that builds nothing, so a mesh with empty collision is treated as having none.
        bool HasCollision() const;

        // A concave set builds a triangle mesh, which the physics engine restricts to static and kinematic bodies.
        bool IsConcave() const { return bUseTriangleMesh && !TriangleIndices.IsEmpty(); }

        int32 NumTriangles() const { return TriangleIndices.Num() / 3; }

        // Every hull's point count summed, which is what the collision really costs to test against.
        int32 NumHullPoints() const;

        void Clear();
    };
}
