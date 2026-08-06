#pragma once

#include "Containers/Array.h"
#include "Core/Math/Math.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "CollisionShape.generated.h"

namespace Lumina
{
    class CStaticMesh;
    class CPhysicsMaterial;

    REFLECT()
    enum class RUNTIME_API ECollisionPrimitiveType : uint8
    {
        Box,
        Sphere,
        Capsule,
        ConvexHull,
    };

    // One piece of a collision shape. Several of these compound into the whole; a convex piece can be
    // simulated dynamically, which is the reason to decompose at all rather than ship one triangle mesh.
    REFLECT()
    struct RUNTIME_API SCollisionPrimitive
    {
        GENERATED_BODY()

        PROPERTY(Editable, Category = "Shape")
        ECollisionPrimitiveType Type = ECollisionPrimitiveType::Box;

        /** Offset from the asset origin, in asset space. */
        PROPERTY(Editable, Category = "Shape")
        FVector3 Center = FVector3(0.0f);

        /** Euler rotation offset in degrees. Ignored by Sphere. */
        PROPERTY(Editable, Category = "Shape")
        FVector3 Rotation = FVector3(0.0f);

        PROPERTY(Editable, Category = "Shape|Box")
        FVector3 HalfExtent = FVector3(0.5f);

        PROPERTY(Editable, ClampMin = 0.001f, Category = "Shape|Sphere & Capsule")
        float Radius = 0.5f;

        /** Cylindrical half-height of a capsule; total height is 2*(HalfHeight + Radius). */
        PROPERTY(Editable, ClampMin = 0.0f, Category = "Shape|Sphere & Capsule")
        float HalfHeight = 0.5f;

        /** Hull points for a ConvexHull piece, already reduced to the hull itself rather than the source
         *  mesh's vertices. Serialized but not shown: a property grid over hundreds of points is noise. */
        PROPERTY()
        TVector<FVector3> HullPoints;
    };

    // Authored collision for a mesh: a set of primitives and hulls, optionally plus a baked triangle mesh
    // for static geometry. Reusable across every entity that needs the same collision, and simplified
    // independently of what is rendered.
    REFLECT()
    class RUNTIME_API CCollisionShape : public CObject
    {
        GENERATED_BODY()

    public:

        bool IsAsset() const override { return true; }

        /** Mesh the shapes were generated from, fixed at creation. Repointing it would leave hulls baked
         *  against geometry that no longer exists, so it is shown but not editable; make a new asset from
         *  the other mesh instead. Kept so shapes can be regenerated after a reimport. */
        PROPERTY(ReadOnly, Category = "Source")
        TObjectPtr<CStaticMesh> SourceMesh;

        /** Surface response for every piece; null falls back to the rigid body's override fields. */
        PROPERTY(Editable, Category = "Collision")
        TObjectPtr<CPhysicsMaterial> PhysicsMaterial;

        PROPERTY(Editable, Category = "Collision")
        TVector<SCollisionPrimitive> Primitives;

        /** Use the baked triangle mesh instead of the primitives. Concave and exact, but static and
         *  kinematic bodies only, which is why it is a choice rather than an addition. */
        PROPERTY(Editable, Category = "Triangle Mesh")
        bool bUseTriangleMesh = false;

        PROPERTY()
        TVector<FVector3> TriangleVertices;

        PROPERTY()
        TVector<uint32> TriangleIndices;

        /** True when this asset can actually produce a shape; an empty asset is a silent no-collision bug
         *  otherwise, so callers check rather than assume. */
        bool HasCollision() const;

        /** True when the asset would build a concave shape, which Jolt restricts to static/kinematic. */
        bool IsConcave() const { return bUseTriangleMesh && !TriangleIndices.empty(); }

        int32 NumTriangles() const { return (int32)(TriangleIndices.size() / 3); }

        /** Total hull points across every convex piece; the editor reports it as the shape's real cost. */
        int32 NumHullPoints() const;

        void ClearGeneratedShapes();
    };
}
