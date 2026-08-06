#include "RuntimePCH.h"
#include "CollisionShapeGen.h"

#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/Physics/CollisionShape.h"
#include "Renderer/MeshData.h"
#include "Log/Log.h"

namespace Lumina::Physics::CollisionGen
{
    namespace
    {
        // LOD 0's meshlets are the only geometry a uploaded mesh keeps; the raw index and vertex streams
        // are dropped. Appends one surface's meshlets into a flat position/index pair.
        void AppendSurface(const FMeshResource& Resource, const FGeometrySurface& Surface,
                           TVector<FVector3>& OutPositions, TVector<uint32>& OutIndices)
        {
            const FMeshletData& MD = Resource.MeshletData;

            const uint32 Offset = Surface.LODMeshletOffset[0];
            const uint32 Count  = Surface.LODMeshletCount[0];

            for (uint32 i = 0; i < Count; ++i)
            {
                const FMeshlet& M = MD.Meshlets[Offset + i];
                const uint32 BaseVertex = (uint32)OutPositions.size();

                for (uint32 v = 0; v < M.VertexCount; ++v)
                {
                    OutPositions.push_back(MD.MeshletVertices[M.VertexOffset + v].Position);
                }

                for (uint32 t = 0; t < M.TriangleCount; ++t)
                {
                    const uint32 Packed = MD.MeshletTriangles[M.TriangleOffset + t];
                    OutIndices.push_back(BaseVertex + ((Packed      ) & 0xFFu));
                    OutIndices.push_back(BaseVertex + ((Packed >>  8) & 0xFFu));
                    OutIndices.push_back(BaseVertex + ((Packed >> 16) & 0xFFu));
                }
            }
        }

        const FMeshResource* ResolveResource(const CStaticMesh* Mesh)
        {
            if (Mesh == nullptr)
            {
                return nullptr;
            }

            const FMeshResource& Resource = Mesh->GetMeshResource();
            if (Resource.MeshletData.IsEmpty() || Resource.bSkinnedMesh)
            {
                return nullptr;
            }

            return &Resource;
        }
    }

    bool GatherMeshGeometry(const CStaticMesh* Mesh, TVector<FVector3>& OutPositions, TVector<uint32>& OutIndices)
    {
        OutPositions.clear();
        OutIndices.clear();

        const FMeshResource* Resource = ResolveResource(Mesh);
        if (Resource == nullptr)
        {
            return false;
        }

        for (const FGeometrySurface& Surface : Resource->GeometrySurfaces)
        {
            AppendSurface(*Resource, Surface, OutPositions, OutIndices);
        }

        return !OutPositions.empty() && OutIndices.size() >= 3;
    }

    bool GenerateSingleHull(const CStaticMesh* Mesh, CCollisionShape* OutShape)
    {
        if (OutShape == nullptr)
        {
            return false;
        }

        TVector<FVector3> Positions;
        TVector<uint32> Indices;
        if (!GatherMeshGeometry(Mesh, Positions, Indices))
        {
            return false;
        }

        TVector<FVector3> Hull;
        if (!BuildHullPoints(Positions, Hull))
        {
            return false;
        }

        OutShape->ClearGeneratedShapes();

        SCollisionPrimitive& Primitive = OutShape->Primitives.emplace_back();
        Primitive.Type = ECollisionPrimitiveType::ConvexHull;
        Primitive.HullPoints = Move(Hull);

        return true;
    }

    bool GeneratePerSurfaceHulls(const CStaticMesh* Mesh, CCollisionShape* OutShape)
    {
        if (OutShape == nullptr)
        {
            return false;
        }

        const FMeshResource* Resource = ResolveResource(Mesh);
        if (Resource == nullptr)
        {
            return false;
        }

        TVector<SCollisionPrimitive> Generated;

        for (const FGeometrySurface& Surface : Resource->GeometrySurfaces)
        {
            TVector<FVector3> Positions;
            TVector<uint32> Indices;
            AppendSurface(*Resource, Surface, Positions, Indices);

            if (Positions.empty())
            {
                continue;
            }

            TVector<FVector3> Hull;
            if (!BuildHullPoints(Positions, Hull))
            {
                // A flat or degenerate surface has no volume; skipping it beats emitting a broken piece.
                continue;
            }

            SCollisionPrimitive& Primitive = Generated.emplace_back();
            Primitive.Type = ECollisionPrimitiveType::ConvexHull;
            Primitive.HullPoints = Move(Hull);
        }

        if (Generated.empty())
        {
            return false;
        }

        OutShape->ClearGeneratedShapes();
        OutShape->Primitives = Move(Generated);
        return true;
    }

    bool GenerateTriangleMesh(const CStaticMesh* Mesh, CCollisionShape* OutShape)
    {
        if (OutShape == nullptr)
        {
            return false;
        }

        TVector<FVector3> Positions;
        TVector<uint32> Indices;
        if (!GatherMeshGeometry(Mesh, Positions, Indices))
        {
            return false;
        }

        OutShape->ClearGeneratedShapes();
        OutShape->TriangleVertices = Move(Positions);
        OutShape->TriangleIndices  = Move(Indices);
        OutShape->bUseTriangleMesh = true;

        return true;
    }

    bool GenerateFittedBox(const CStaticMesh* Mesh, CCollisionShape* OutShape)
    {
        if (OutShape == nullptr)
        {
            return false;
        }

        TVector<FVector3> Positions;
        TVector<uint32> Indices;
        if (!GatherMeshGeometry(Mesh, Positions, Indices))
        {
            return false;
        }

        FVector3 Min = Positions[0];
        FVector3 Max = Positions[0];
        for (const FVector3& P : Positions)
        {
            Min = FVector3(Math::Min(Min.x, P.x), Math::Min(Min.y, P.y), Math::Min(Min.z, P.z));
            Max = FVector3(Math::Max(Max.x, P.x), Math::Max(Max.y, P.y), Math::Max(Max.z, P.z));
        }

        OutShape->ClearGeneratedShapes();

        SCollisionPrimitive& Primitive = OutShape->Primitives.emplace_back();
        Primitive.Type = ECollisionPrimitiveType::Box;
        Primitive.Center = (Min + Max) * 0.5f;
        Primitive.HalfExtent = (Max - Min) * 0.5f;

        return true;
    }

    bool GenerateFittedSphere(const CStaticMesh* Mesh, CCollisionShape* OutShape)
    {
        if (OutShape == nullptr)
        {
            return false;
        }

        TVector<FVector3> Positions;
        TVector<uint32> Indices;
        if (!GatherMeshGeometry(Mesh, Positions, Indices))
        {
            return false;
        }

        FVector3 Min = Positions[0];
        FVector3 Max = Positions[0];
        for (const FVector3& P : Positions)
        {
            Min = FVector3(Math::Min(Min.x, P.x), Math::Min(Min.y, P.y), Math::Min(Min.z, P.z));
            Max = FVector3(Math::Max(Max.x, P.x), Math::Max(Max.y, P.y), Math::Max(Max.z, P.z));
        }

        const FVector3 Center = (Min + Max) * 0.5f;

        // Farthest point rather than half the diagonal: the bounding-box corner is not on the mesh, so a
        // diagonal-derived radius is loose on anything that is not a cube.
        float RadiusSq = 0.0f;
        for (const FVector3& P : Positions)
        {
            RadiusSq = Math::Max(RadiusSq, Math::LengthSquared(P - Center));
        }

        OutShape->ClearGeneratedShapes();

        SCollisionPrimitive& Primitive = OutShape->Primitives.emplace_back();
        Primitive.Type = ECollisionPrimitiveType::Sphere;
        Primitive.Center = Center;
        Primitive.Radius = Math::Max(Math::Sqrt(RadiusSq), 0.001f);

        return true;
    }
}
