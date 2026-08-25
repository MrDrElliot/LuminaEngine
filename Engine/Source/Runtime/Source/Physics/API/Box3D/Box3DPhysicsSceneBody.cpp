#include "RuntimePCH.h"
#include "Box3DPhysicsScene.h"

#include <box3d/collision.h>

#include "Box3DInternal.h"
#include "Box3DUtils.h"

#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/Physics/CollisionShape.h"
#include "Log/Log.h"
#include "Renderer/MeshQuantization.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina::Physics
{
    void WarnTaperedCapsuleOnce()
    {
        static bool bWarned = false;
        if (!bWarned)
        {
            bWarned = true;
            LOG_WARN("Tapered capsule colliders are approximated by a cone hull under Box3D; the rounded caps are lost.");
        }
    }

    const b3HullData* FBox3DPhysicsScene::GetOrCreateBoxHull(const FVector3& HalfExtent)
    {
        const FHullKey Key{ 0, HalfExtent.x, HalfExtent.y, HalfExtent.z, nullptr };

        {
            FReadScopeLock Lock(HullCacheMutex);
            if (auto It = HullCache.find(Key); It != HullCache.end())
            {
                return It->second;
            }
        }

        const b3Vec3 Extent = Box3DUtils::ToB3Vec3(HalfExtent);
        const b3Vec3 Points[8] =
        {
            { -Extent.x, -Extent.y, -Extent.z }, {  Extent.x, -Extent.y, -Extent.z },
            { -Extent.x,  Extent.y, -Extent.z }, {  Extent.x,  Extent.y, -Extent.z },
            { -Extent.x, -Extent.y,  Extent.z }, {  Extent.x, -Extent.y,  Extent.z },
            { -Extent.x,  Extent.y,  Extent.z }, {  Extent.x,  Extent.y,  Extent.z },
        };

        b3HullData* Hull = b3CreateHull(Points, 8, B3_MAX_HULL_VERTICES);
        if (Hull == nullptr)
        {
            return nullptr;
        }

        TScopeLock Lock(HullCacheMutex);
        auto [It, bInserted] = HullCache.try_emplace(Key, Hull);
        if (!bInserted)
        {
            b3DestroyHull(Hull);
        }
        return It->second;
    }

    const b3HullData* FBox3DPhysicsScene::GetOrCreateCylinderHull(float Radius, float HalfHeight)
    {
        const FHullKey Key{ 1, Radius, HalfHeight, 0.0f, nullptr };

        {
            FReadScopeLock Lock(HullCacheMutex);
            if (auto It = HullCache.find(Key); It != HullCache.end())
            {
                return It->second;
            }
        }

        b3HullData* Hull = b3CreateCylinder(HalfHeight * 2.0f, Radius, -HalfHeight, 16);
        if (Hull == nullptr)
        {
            return nullptr;
        }

        TScopeLock Lock(HullCacheMutex);
        auto [It, bInserted] = HullCache.try_emplace(Key, Hull);
        if (!bInserted)
        {
            b3DestroyHull(Hull);
        }
        return It->second;
    }

    const b3HullData* FBox3DPhysicsScene::GetOrCreateTaperedCylinderHull(float HalfHeight, float TopRadius, float BottomRadius)
    {
        const FHullKey Key{ 2, HalfHeight, TopRadius, BottomRadius, nullptr };

        {
            FReadScopeLock Lock(HullCacheMutex);
            if (auto It = HullCache.find(Key); It != HullCache.end())
            {
                return It->second;
            }
        }

        b3HullData* Hull = b3CreateCone(HalfHeight * 2.0f, BottomRadius, TopRadius, 16);
        if (Hull == nullptr)
        {
            return nullptr;
        }

        TScopeLock Lock(HullCacheMutex);
        auto [It, bInserted] = HullCache.try_emplace(Key, Hull);
        if (!bInserted)
        {
            b3DestroyHull(Hull);
        }
        return It->second;
    }

    bool GatherMeshResourceGeometry(const FMeshResource& Resource, const FVector3& Scale,
                                    TVector<b3Vec3>& OutPositions, TVector<int32>* OutIndices)
    {
            const FMeshletData& MD = Resource.MeshletData;
            if (MD.IsEmpty() || Resource.bSkinnedMesh)
            {
                return false;
            }

            for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
            {
                const uint32 Offset = Surface.LODMeshletOffset[0];
                const uint32 Count = Surface.LODMeshletCount[0];

                for (uint32 i = 0; i < Count; ++i)
                {
                    const FMeshlet& M = MD.Meshlets[Offset + i];
                    const uint32 BaseVertex = (uint32)OutPositions.size();

                    for (uint32 v = 0; v < M.VertexCount; ++v)
                    {
                        const FVector3 Decoded = DecodeMeshletPosition(M, MD.MeshletVertices[M.VertexOffset + v]);
                        OutPositions.push_back(Box3DUtils::ToB3Vec3(Decoded * Scale));
                    }

                    if (OutIndices != nullptr)
                    {
                        for (uint32 t = 0; t < M.TriangleCount; ++t)
                        {
                            const uint32 Packed = MD.MeshletTriangles[M.TriangleOffset + t];
                            OutIndices->push_back((int32)(BaseVertex + ((Packed) & 0xFFu)));
                            OutIndices->push_back((int32)(BaseVertex + ((Packed >> 8) & 0xFFu)));
                            OutIndices->push_back((int32)(BaseVertex + ((Packed >> 16) & 0xFFu)));
                        }
                    }
                }
            }

        return !OutPositions.empty();
    }

    const b3HullData* FBox3DPhysicsScene::GetOrCreateMeshHull(const CMesh* Mesh)
    {
        // Scale rides on the shape, so one unit-scale hull is shared by every instance of the mesh.
        const FHullKey Key{ 3, 0.0f, 0.0f, 0.0f, Mesh };

        {
            FReadScopeLock Lock(HullCacheMutex);
            if (auto It = HullCache.find(Key); It != HullCache.end())
            {
                return It->second;
            }
        }

        TVector<b3Vec3> Positions;
        if (!GatherMeshResourceGeometry(Mesh->GetMeshResource(), FVector3(1.0f), Positions, nullptr))
        {
            return nullptr;
        }

        // Box3D simplifies past B3_MAX_HULL_VERTICES, so a dense mesh comes back as a coarser hull.
        b3HullData* Hull = b3CreateHull(Positions.data(), (int)Positions.size(), B3_MAX_HULL_VERTICES);
        if (Hull == nullptr)
        {
            return nullptr;
        }

        TScopeLock Lock(HullCacheMutex);
        auto [It, bInserted] = HullCache.try_emplace(Key, Hull);
        if (!bInserted)
        {
            b3DestroyHull(Hull);
        }
        return It->second;
    }

    const b3MeshData* FBox3DPhysicsScene::GetOrCreateTriangleMesh(const CMesh* Mesh)
    {
        {
            FReadScopeLock Lock(MeshCacheMutex);
            if (auto It = MeshCache.find(Mesh); It != MeshCache.end())
            {
                return It->second;
            }
        }

        TVector<b3Vec3> Positions;
        TVector<int32> Indices;

        // Scale rides on b3CreateMeshShape, so the cached geometry stays unit scale and is shared.
        if (!GatherMeshResourceGeometry(Mesh->GetMeshResource(), FVector3(1.0f), Positions, &Indices) || Indices.empty())
        {
            return nullptr;
        }

        b3MeshDef Def = b3MeshDef{};
        Def.vertices = Positions.data();
        Def.vertexCount = (int)Positions.size();
        Def.indices = Indices.data();
        Def.triangleCount = (int)(Indices.size() / 3);
        Def.weldVertices = true;
        Def.identifyEdges = true;

        b3MeshData* Built = b3CreateMesh(&Def, nullptr, 0);
        if (Built == nullptr)
        {
            return nullptr;
        }

        TScopeLock Lock(MeshCacheMutex);
        auto [It, bInserted] = MeshCache.try_emplace(Mesh, Built);
        if (!bInserted)
        {
            b3DestroyMesh(Built);
        }
        return It->second;
    }

    bool FBox3DPhysicsScene::BuildCompoundShapes(const SCompoundColliderComponent& Comp, const STransformComponent& Transform, TVector<FPendingShape>& OutShapes)
    {
        const FVector3 Scale = Transform.GetScale();
        const float UniformScale = Transform.MaxScale();

        for (const SCompoundSubShape& Child : Comp.Shapes)
        {
            const FQuat ChildRotation(Child.Rotation);
            const FVector3 ChildOffset = Child.Offset * Scale;

            switch (Child.Type)
            {
                case ECompoundShapeType::Box:
                {
                    if (const b3HullData* Hull = GetOrCreateBoxHull(Child.HalfExtent * Scale))
                    {
                        OutShapes.push_back(MakeHullShape(Hull, ChildOffset, ChildRotation));
                    }
                    break;
                }
                case ECompoundShapeType::Sphere:
                {
                    OutShapes.push_back(MakeSphereShape(ChildOffset, Child.Radius * UniformScale));
                    break;
                }
                case ECompoundShapeType::Capsule:
                {
                    OutShapes.push_back(MakeCapsuleShape(ChildOffset, ChildRotation, Child.Radius * UniformScale, Child.HalfHeight * UniformScale));
                    break;
                }
                default:
                {
                    if (const b3HullData* Hull = GetOrCreateBoxHull(Child.HalfExtent * Scale))
                    {
                        OutShapes.push_back(MakeHullShape(Hull, ChildOffset, ChildRotation));
                    }
                    break;
                }
            }
        }

        return !OutShapes.empty();
    }

    bool FBox3DPhysicsScene::BuildCollisionShapeAsset(const CCollisionShape& Asset, const FVector3& Scale, TVector<FPendingShape>& OutShapes)
    {
        const float UniformScale = Math::Max(Math::Max(Math::Abs(Scale.x), Math::Abs(Scale.y)), Math::Abs(Scale.z));

        for (const SCollisionPrimitive& Primitive : Asset.Primitives)
        {
            const FQuat Rotation(Primitive.Rotation);
            const FVector3 Offset = Primitive.Center * Scale;

            switch (Primitive.Type)
            {
                case ECollisionPrimitiveType::Box:
                {
                    if (const b3HullData* Hull = GetOrCreateBoxHull(Primitive.HalfExtent * Scale))
                    {
                        OutShapes.push_back(MakeHullShape(Hull, Offset, Rotation));
                    }
                    break;
                }
                case ECollisionPrimitiveType::Sphere:
                {
                    OutShapes.push_back(MakeSphereShape(Offset, Primitive.Radius * UniformScale));
                    break;
                }
                case ECollisionPrimitiveType::Capsule:
                {
                    OutShapes.push_back(MakeCapsuleShape(Offset, Rotation, Primitive.Radius * UniformScale, Primitive.HalfHeight * UniformScale));
                    break;
                }
                case ECollisionPrimitiveType::ConvexHull:
                {
                    TVector<b3Vec3> Points;
                    Points.reserve(Primitive.HullPoints.size());
                    for (const FVector3& P : Primitive.HullPoints)
                    {
                        Points.push_back(Box3DUtils::ToB3Vec3(P * Scale));
                    }

                    if (Points.size() < 4)
                    {
                        break;
                    }

                    // A hull point set belongs to one asset, so there is nothing a shared cache could hit.
                    if (b3HullData* Hull = b3CreateHull(Points.data(), (int)Points.size(), B3_MAX_HULL_VERTICES))
                    {
                        TrackOwnedHull(Hull);
                        OutShapes.push_back(MakeHullShape(Hull, Offset, Rotation));
                    }
                    break;
                }
                default:
                    break;
            }
        }

        if (Asset.IsConcave() && !Asset.TriangleVertices.empty() && !Asset.TriangleIndices.empty())
        {
            TVector<b3Vec3> Points;
            TVector<int32> Indices;
            Points.reserve(Asset.TriangleVertices.size());
            for (const FVector3& P : Asset.TriangleVertices)
            {
                Points.push_back(Box3DUtils::ToB3Vec3(P));
            }
            Indices.reserve(Asset.TriangleIndices.size());
            for (uint32 Index : Asset.TriangleIndices)
            {
                Indices.push_back((int32)Index);
            }

            b3MeshDef Def = b3MeshDef{};
            Def.vertices = Points.data();
            Def.vertexCount = (int)Points.size();
            Def.indices = Indices.data();
            Def.triangleCount = (int)(Indices.size() / 3);
            Def.weldVertices = true;
            Def.identifyEdges = true;

            if (b3MeshData* Built = b3CreateMesh(&Def, nullptr, 0))
            {
                TrackOwnedMesh(Built);

                FPendingShape Shape;
                Shape.Type = b3_meshShape;
                Shape.Mesh = Built;
                Shape.Scale = Box3DUtils::ToB3Vec3(Scale);
                OutShapes.push_back(Shape);
            }
        }

        return !OutShapes.empty();
    }
}
