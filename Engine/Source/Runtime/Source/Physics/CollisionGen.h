#pragma once

#include "Containers/Vector.h"
#include "Core/Math/Math.h"

namespace Lumina
{
    class CStaticMesh;
    struct SCollisionPrimitive;
    struct SMeshCollision;

    // Bakes authored collision out of a render mesh. Lives in Runtime rather than the editor because the
    // hull reduction needs the physics backend, which the editor module does not link.
    namespace Physics::CollisionGen
    {
        // The ways collision can be made from a mesh, cheapest first, in the order the editor offers them.
        enum class ECollisionGenerator : uint8
        {
            Box,
            Sphere,
            Capsule,
            SingleHull,
            PerSurfaceHulls,
            TriangleMesh,
        };

        // Reduces a point cloud to the vertices of its convex hull. False when the points are degenerate
        // (fewer than four, or coplanar) and no hull exists.
        RUNTIME_API bool BuildHullPoints(const TVector<FVector3>& Points, TVector<FVector3>& OutHull);

        // Unique edges of the hull formed by Points, as index pairs into OutVertices. For drawing only:
        // face loops rather than triangulated faces, so a wireframe has no interior diagonals across it.
        RUNTIME_API bool BuildHullWireframe(const TVector<FVector3>& Points, TVector<FVector3>& OutVertices,
                                            TVector<uint32>& OutEdges);

        // Triangulated hull faces, for consumers that need a surface rather than an outline (navmesh bakes).
        RUNTIME_API bool BuildHullTriangles(const TVector<FVector3>& Points, TVector<FVector3>& OutVertices,
                                            TVector<uint32>& OutIndices);

        // Every LOD-0 position in the mesh, and the triangles indexing them. Both generators below read
        // through this, so they agree on what the mesh's collision geometry is.
        RUNTIME_API bool GatherMeshGeometry(const CStaticMesh* Mesh, TVector<FVector3>& OutPositions, TVector<uint32>& OutIndices);

        // One hull wrapping the whole mesh. Cheap and always dynamic-safe, but it fills in every concavity.
        RUNTIME_API bool GenerateSingleHull(const CStaticMesh* Mesh, SMeshCollision& OutCollision);

        // One hull per geometry surface. A crude decomposition, but the split is already authored in the
        // mesh, so a table's legs and top come out as separate hulls with no algorithm to tune.
        RUNTIME_API bool GeneratePerSurfaceHulls(const CStaticMesh* Mesh, SMeshCollision& OutCollision);

        // Exact concave geometry. Static and kinematic bodies only.
        RUNTIME_API bool GenerateTriangleMesh(const CStaticMesh* Mesh, SMeshCollision& OutCollision);

        // A single box or sphere fitted to the mesh bounds; the cheapest useful collision there is.
        RUNTIME_API bool GenerateFittedBox(const CStaticMesh* Mesh, SMeshCollision& OutCollision);
        RUNTIME_API bool GenerateFittedSphere(const CStaticMesh* Mesh, SMeshCollision& OutCollision);

        // A capsule along the longest bounds axis of Points, sized so every point lies inside it. False for no points.
        RUNTIME_API bool FitCapsule(const TVector<FVector3>& Points, SCollisionPrimitive& OutPrimitive);
        RUNTIME_API bool GenerateFittedCapsule(const CStaticMesh* Mesh, SMeshCollision& OutCollision);

        // Replaces OutCollision with what Generator makes from Mesh. False when the mesh gave no geometry.
        RUNTIME_API bool Generate(ECollisionGenerator Generator, const CStaticMesh* Mesh, SMeshCollision& OutCollision);

        // A mesh collider that builds triangles stays static whatever its rigid body asks for, and navigation bakes it as level geometry.
        RUNTIME_API bool IsConcaveMeshCollider(const CStaticMesh& Mesh, bool bConvex);
    }
}
