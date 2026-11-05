#include "RuntimePCH.h"

#include <box3d/box3d.h>
#include <box3d/collision.h>

#include "Physics/CollisionGen.h"

namespace Lumina::Physics::CollisionGen
{
    namespace
    {
        // Box3D caps a hull at B3_MAX_HULL_VERTICES, so a denser point cloud comes back simplified.
        b3HullData* CreateHullFrom(const TVector<FVector3>& Points)
        {
            if (Points.Num() < 4)
            {
                return nullptr;
            }

            TVector<b3Vec3> Input;
            Input.Reserve(Points.Num());
            for (const FVector3& P : Points)
            {
                Input.Add(b3Vec3{ P.x, P.y, P.z });
            }

            return b3CreateHull(Input.GetData(), Input.Num(), B3_MAX_HULL_VERTICES);
        }

        void CopyHullPoints(const b3HullData* Hull, TVector<FVector3>& Out)
        {
            const b3Vec3* Points = b3GetHullPoints(Hull);
            Out.Reserve(Hull->vertexCount);
            for (int32 i = 0; i < Hull->vertexCount; ++i)
            {
                Out.Emplace(Points[i].x, Points[i].y, Points[i].z);
            }
        }

        // Walks one face's half-edge loop, appending each edge's origin vertex in winding order.
        void GatherFaceVertices(const b3HullData* Hull, int32 FaceIndex, TVector<uint32>& Out)
        {
            const b3HullHalfEdge* Edges = b3GetHullEdges(Hull);
            const b3HullFace* Faces = b3GetHullFaces(Hull);

            Out.Reset();

            const uint8 Start = Faces[FaceIndex].edge;
            uint8 Edge = Start;
            do
            {
                Out.Add(Edges[Edge].origin);
                Edge = Edges[Edge].next;
            }
            while (Edge != Start && static_cast<size_t>(Out.Num()) <= (size_t)Hull->edgeCount);
        }
    }

    bool BuildHullPoints(const TVector<FVector3>& Points, TVector<FVector3>& OutHull)
    {
        OutHull.Reset();

        b3HullData* Hull = CreateHullFrom(Points);
        if (Hull == nullptr)
        {
            return false;
        }

        CopyHullPoints(Hull, OutHull);
        b3DestroyHull(Hull);

        return OutHull.Num() >= 4;
    }

    bool BuildHullWireframe(const TVector<FVector3>& Points, TVector<FVector3>& OutVertices, TVector<uint32>& OutEdges)
    {
        OutVertices.Reset();
        OutEdges.Reset();

        b3HullData* Hull = CreateHullFrom(Points);
        if (Hull == nullptr)
        {
            return false;
        }

        CopyHullPoints(Hull, OutVertices);

        const b3HullHalfEdge* Edges = b3GetHullEdges(Hull);
        for (int32 i = 0; i < Hull->edgeCount; ++i)
        {
            // Half edges are twinned, so only the lower index of each pair contributes a drawn edge.
            const uint8 Twin = Edges[i].twin;
            if ((int32)Twin < i)
            {
                continue;
            }

            OutEdges.Add(Edges[i].origin);
            OutEdges.Add(Edges[Twin].origin);
        }

        b3DestroyHull(Hull);

        return !OutEdges.IsEmpty();
    }

    bool BuildHullTriangles(const TVector<FVector3>& Points, TVector<FVector3>& OutVertices, TVector<uint32>& OutIndices)
    {
        OutVertices.Reset();
        OutIndices.Reset();

        b3HullData* Hull = CreateHullFrom(Points);
        if (Hull == nullptr)
        {
            return false;
        }

        CopyHullPoints(Hull, OutVertices);

        TVector<uint32> FaceVertices;
        for (int32 Face = 0; Face < Hull->faceCount; ++Face)
        {
            GatherFaceVertices(Hull, Face, FaceVertices);
            if (FaceVertices.Num() < 3)
            {
                continue;
            }

            // Hull faces are convex and consistently wound, so a fan from the first vertex is valid.
            for (int32 i = 1; i + 1 < FaceVertices.Num(); ++i)
            {
                OutIndices.Add(FaceVertices[0]);
                OutIndices.Add(FaceVertices[i]);
                OutIndices.Add(FaceVertices[i + 1]);
            }
        }

        b3DestroyHull(Hull);

        return !OutIndices.IsEmpty();
    }
}
