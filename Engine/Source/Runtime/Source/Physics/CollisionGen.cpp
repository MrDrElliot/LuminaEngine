#include "RuntimePCH.h"
#include "CollisionGen.h"

#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/Mesh/MeshCollision.h"
#include "Renderer/MeshData.h"
#include "Log/Log.h"
#include "Renderer/MeshQuantization.h"

namespace Lumina::Physics::CollisionGen
{
    namespace
    {
        // LOD 0's meshlets are the only geometry an uploaded mesh keeps, since the raw streams are dropped.
        void AppendSurface(const FMeshResource& Resource, const FGeometrySurface& Surface,
                           TVector<FVector3>& OutPositions, TVector<uint32>& OutIndices)
        {
            const FMeshletData& MD = Resource.MeshletData;

            const uint32 Offset = Surface.LODMeshletOffset[0];
            const uint32 Count  = Surface.LODMeshletCount[0];

            for (uint32 i = 0; i < Count; ++i)
            {
                const FMeshlet& M = MD.Meshlets[Offset + i];
                const uint32 BaseVertex = (uint32)OutPositions.Num();

                for (uint32 v = 0; v < M.VertexCount; ++v)
                {
                    OutPositions.Add(GetMeshletVertexPosition(MD, M, v, false));
                }

                for (uint32 t = 0; t < M.TriangleCount; ++t)
                {
                    const uint32 Packed = MD.MeshletTriangles[M.TriangleOffset + t];
                    OutIndices.Add(BaseVertex + ((Packed      ) & 0xFFu));
                    OutIndices.Add(BaseVertex + ((Packed >>  8) & 0xFFu));
                    OutIndices.Add(BaseVertex + ((Packed >> 16) & 0xFFu));
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
        OutPositions.Reset();
        OutIndices.Reset();

        const FMeshResource* Resource = ResolveResource(Mesh);
        if (Resource == nullptr)
        {
            return false;
        }

        for (const FGeometrySurface& Surface : Resource->GeometrySurfaces)
        {
            AppendSurface(*Resource, Surface, OutPositions, OutIndices);
        }

        return !OutPositions.IsEmpty() && OutIndices.Num() >= 3;
    }

    namespace
    {
        bool GatherMeshBounds(const CStaticMesh* Mesh, TVector<FVector3>& OutPositions, FVector3& OutMin, FVector3& OutMax)
        {
            TVector<uint32> Indices;
            if (!GatherMeshGeometry(Mesh, OutPositions, Indices) || OutPositions.IsEmpty())
            {
                return false;
            }

            OutMin = OutPositions[0];
            OutMax = OutPositions[0];
            for (const FVector3& P : OutPositions)
            {
                OutMin = Math::Min(OutMin, P);
                OutMax = Math::Max(OutMax, P);
            }
            return true;
        }
    }

    bool GenerateSingleHull(const CStaticMesh* Mesh, SMeshCollision& OutCollision)
    {

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

        OutCollision.Clear();

        SCollisionPrimitive& Primitive = OutCollision.Primitives.EmplaceGetRef();
        Primitive.Type = ECollisionPrimitiveType::ConvexHull;
        Primitive.HullPoints = Move(Hull);

        return true;
    }

    bool GeneratePerSurfaceHulls(const CStaticMesh* Mesh, SMeshCollision& OutCollision)
    {

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

            if (Positions.IsEmpty())
            {
                continue;
            }

            TVector<FVector3> Hull;
            if (!BuildHullPoints(Positions, Hull))
            {
                // A flat or degenerate surface has no volume; skipping it beats emitting a broken piece.
                continue;
            }

            SCollisionPrimitive& Primitive = Generated.EmplaceGetRef();
            Primitive.Type = ECollisionPrimitiveType::ConvexHull;
            Primitive.HullPoints = Move(Hull);
        }

        if (Generated.IsEmpty())
        {
            return false;
        }

        OutCollision.Clear();
        OutCollision.Primitives = Move(Generated);
        return true;
    }

    bool GenerateTriangleMesh(const CStaticMesh* Mesh, SMeshCollision& OutCollision)
    {

        TVector<FVector3> Positions;
        TVector<uint32> Indices;
        if (!GatherMeshGeometry(Mesh, Positions, Indices))
        {
            return false;
        }

        OutCollision.Clear();
        OutCollision.TriangleVertices = Move(Positions);
        OutCollision.TriangleIndices  = Move(Indices);
        OutCollision.bUseTriangleMesh = true;

        return true;
    }

    bool GenerateFittedBox(const CStaticMesh* Mesh, SMeshCollision& OutCollision)
    {

        TVector<FVector3> Positions;
        FVector3 Min, Max;
        if (!GatherMeshBounds(Mesh, Positions, Min, Max))
        {
            return false;
        }

        OutCollision.Clear();

        SCollisionPrimitive& Primitive = OutCollision.Primitives.EmplaceGetRef();
        Primitive.Type = ECollisionPrimitiveType::Box;
        Primitive.Center = (Min + Max) * 0.5f;
        Primitive.HalfExtent = (Max - Min) * 0.5f;

        return true;
    }

    bool GenerateFittedSphere(const CStaticMesh* Mesh, SMeshCollision& OutCollision)
    {

        TVector<FVector3> Positions;
        FVector3 Min, Max;
        if (!GatherMeshBounds(Mesh, Positions, Min, Max))
        {
            return false;
        }

        const FVector3 Center = (Min + Max) * 0.5f;

        // The bounding-box corner is not on the mesh, so a diagonal-derived radius is loose.
        float RadiusSq = 0.0f;
        for (const FVector3& P : Positions)
        {
            RadiusSq = Math::Max(RadiusSq, Math::LengthSquared(P - Center));
        }

        OutCollision.Clear();

        SCollisionPrimitive& Primitive = OutCollision.Primitives.EmplaceGetRef();
        Primitive.Type = ECollisionPrimitiveType::Sphere;
        Primitive.Center = Center;
        Primitive.Radius = Math::Max(Math::Sqrt(RadiusSq), 0.001f);

        return true;
    }

    bool FitCapsule(const TVector<FVector3>& Points, SCollisionPrimitive& OutPrimitive)
    {
        if (Points.IsEmpty())
        {
            return false;
        }

        FVector3 Min = Points[0];
        FVector3 Max = Points[0];
        for (const FVector3& P : Points)
        {
            Min = Math::Min(Min, P);
            Max = Math::Max(Max, P);
        }

        const FVector3 Center = (Min + Max) * 0.5f;
        const FVector3 Size   = Max - Min;
        const int32 Axis = (Size.x >= Size.y && Size.x >= Size.z) ? 0 : (Size.y >= Size.z ? 1 : 2);

        float RadiusSq = 0.0f;
        for (const FVector3& P : Points)
        {
            FVector3 Radial = P - Center;
            Radial[Axis] = 0.0f;
            RadiusSq = Math::Max(RadiusSq, Math::LengthSquared(Radial));
        }

        // The cylinder stops where a cap's hemisphere still reaches the farthest point at that radius.
        float HalfHeight = 0.0f;
        for (const FVector3& P : Points)
        {
            const FVector3 Offset = P - Center;
            FVector3 Radial = Offset;
            Radial[Axis] = 0.0f;
            HalfHeight = Math::Max(HalfHeight, Math::Abs(Offset[Axis]) - Math::Sqrt(Math::Max(RadiusSq - Math::LengthSquared(Radial), 0.0f)));
        }

        // The capsule runs along its local Y, so the X and Z cases turn Y onto that axis.
        OutPrimitive.Type       = ECollisionPrimitiveType::Capsule;
        OutPrimitive.Center     = Center;
        OutPrimitive.Rotation   = Axis == 0 ? FVector3(0.0f, 0.0f, -90.0f) : (Axis == 2 ? FVector3(90.0f, 0.0f, 0.0f) : FVector3(0.0f));
        OutPrimitive.Radius     = Math::Max(Math::Sqrt(RadiusSq), 0.001f);
        OutPrimitive.HalfHeight = HalfHeight;
        return true;
    }

    bool GenerateFittedCapsule(const CStaticMesh* Mesh, SMeshCollision& OutCollision)
    {

        TVector<FVector3> Positions;
        TVector<uint32> Indices;
        SCollisionPrimitive Capsule;
        if (!GatherMeshGeometry(Mesh, Positions, Indices) || !FitCapsule(Positions, Capsule))
        {
            return false;
        }

        OutCollision.Clear();
        OutCollision.Primitives.Add(Capsule);
        return true;
    }

    bool Generate(ECollisionGenerator Generator, const CStaticMesh* Mesh, SMeshCollision& OutCollision)
    {
        switch (Generator)
        {
        case ECollisionGenerator::Box:             return GenerateFittedBox(Mesh, OutCollision);
        case ECollisionGenerator::Sphere:          return GenerateFittedSphere(Mesh, OutCollision);
        case ECollisionGenerator::Capsule:         return GenerateFittedCapsule(Mesh, OutCollision);
        case ECollisionGenerator::SingleHull:      return GenerateSingleHull(Mesh, OutCollision);
        case ECollisionGenerator::PerSurfaceHulls: return GeneratePerSurfaceHulls(Mesh, OutCollision);
        case ECollisionGenerator::TriangleMesh:    return GenerateTriangleMesh(Mesh, OutCollision);
        }
        return false;
    }

    bool IsConcaveMeshCollider(const CStaticMesh& Mesh, bool bConvex)
    {
        const SMeshCollision* Collision = Mesh.GetCollision();
        return Collision != nullptr ? Collision->IsConcave() : !bConvex;
    }
}
