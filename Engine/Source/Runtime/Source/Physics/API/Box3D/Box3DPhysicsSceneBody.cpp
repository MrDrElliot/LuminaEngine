#include "RuntimePCH.h"
#include "Box3DPhysicsScene.h"

#include <box3d/collision.h>

#include "Box3DInternal.h"
#include "Box3DUtils.h"

#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/Mesh/MeshCollision.h"
#include "Log/Log.h"
#include "Renderer/MeshQuantization.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TransformComponent.h"

namespace Lumina::Physics
{
    const b3HullData* FBox3DPhysicsScene::GetOrCreateBoxHull(const FVector3& HalfExtent)
    {
        const FHullKey Key{ 0, HalfExtent.x, HalfExtent.y, HalfExtent.z, nullptr };

        {
            FReadScopeLock Lock(HullCacheMutex);
            if (auto It = BoxHullCache.Find(Key); It != nullptr)
            {
                return &(*It)->base;
            }
        }

        // A flat box has no volume for Box3D to collide against.
        constexpr float MinHalfExtent = 1.0e-4f;
        if (HalfExtent.x < MinHalfExtent || HalfExtent.y < MinHalfExtent || HalfExtent.z < MinHalfExtent)
        {
            return nullptr;
        }

        // Analytic, where a general quickhull over the eight corners cost tens of microseconds for every new size.
        b3BoxHull* Box = Memory::New<b3BoxHull>(b3MakeBoxHull(HalfExtent.x, HalfExtent.y, HalfExtent.z));

        TScopeLock Lock(HullCacheMutex);
        if (!BoxHullCache.TryAdd(Key, Box))
        {
            Memory::Delete(Box);
        }
        return &BoxHullCache.FindChecked(Key)->base;
    }

    const b3HullData* FBox3DPhysicsScene::GetOrCreateCylinderHull(float Radius, float HalfHeight)
    {
        const FHullKey Key{ 1, Radius, HalfHeight, 0.0f, nullptr };

        {
            FReadScopeLock Lock(HullCacheMutex);
            if (auto It = HullCache.Find(Key); It != nullptr)
            {
                return (*It);
            }
        }

        b3HullData* Hull = b3CreateCylinder(HalfHeight * 2.0f, Radius, -HalfHeight, 16);
        if (Hull == nullptr)
        {
            return nullptr;
        }

        TScopeLock Lock(HullCacheMutex);
        if (!HullCache.TryAdd(Key, Hull))
        {
            b3DestroyHull(Hull);
        }
        return HullCache.FindChecked(Key);
    }

    const b3HullData* FBox3DPhysicsScene::GetOrCreateTaperedCylinderHull(float HalfHeight, float TopRadius, float BottomRadius)
    {
        const FHullKey Key{ 2, HalfHeight, TopRadius, BottomRadius, nullptr };

        {
            FReadScopeLock Lock(HullCacheMutex);
            if (auto It = HullCache.Find(Key); It != nullptr)
            {
                return (*It);
            }
        }

        b3HullData* Hull = b3CreateCone(HalfHeight * 2.0f, BottomRadius, TopRadius, 16);
        if (Hull == nullptr)
        {
            return nullptr;
        }

        TScopeLock Lock(HullCacheMutex);
        if (!HullCache.TryAdd(Key, Hull))
        {
            b3DestroyHull(Hull);
        }
        return HullCache.FindChecked(Key);
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
                    const uint32 BaseVertex = (uint32)OutPositions.Num();

                    for (uint32 v = 0; v < M.VertexCount; ++v)
                    {
                        const FVector3 Decoded = GetMeshletVertexPosition(MD, M, v, false);
                        OutPositions.Add(Box3DUtils::ToB3Vec3(Decoded * Scale));
                    }

                    if (OutIndices != nullptr)
                    {
                        for (uint32 t = 0; t < M.TriangleCount; ++t)
                        {
                            const uint32 Packed = MD.MeshletTriangles[M.TriangleOffset + t];
                            OutIndices->Add((int32)(BaseVertex + ((Packed) & 0xFFu)));
                            OutIndices->Add((int32)(BaseVertex + ((Packed >> 8) & 0xFFu)));
                            OutIndices->Add((int32)(BaseVertex + ((Packed >> 16) & 0xFFu)));
                        }
                    }
                }
            }

        return !OutPositions.IsEmpty();
    }

    const b3HullData* FBox3DPhysicsScene::GetOrCreateMeshHull(const CMesh* Mesh)
    {
        // Scale rides on the shape, so one unit-scale hull is shared by every instance of the mesh.
        const FHullKey Key{ 3, 0.0f, 0.0f, 0.0f, Mesh };

        {
            FReadScopeLock Lock(HullCacheMutex);
            if (auto It = HullCache.Find(Key); It != nullptr)
            {
                return (*It);
            }
        }

        TVector<b3Vec3> Positions;
        if (!GatherMeshResourceGeometry(Mesh->GetMeshResource(), FVector3(1.0f), Positions, nullptr))
        {
            return nullptr;
        }

        // Box3D simplifies past B3_MAX_HULL_VERTICES, so a dense mesh comes back as a coarser hull.
        b3HullData* Hull = b3CreateHull(Positions.GetData(), Positions.Num(), B3_MAX_HULL_VERTICES);
        if (Hull == nullptr)
        {
            return nullptr;
        }

        TScopeLock Lock(HullCacheMutex);
        if (!HullCache.TryAdd(Key, Hull))
        {
            b3DestroyHull(Hull);
        }
        return HullCache.FindChecked(Key);
    }

    const b3MeshData* FBox3DPhysicsScene::GetOrCreateTriangleMesh(const CMesh* Mesh)
    {
        {
            FReadScopeLock Lock(MeshCacheMutex);
            if (auto It = MeshCache.Find(Mesh); It != nullptr)
            {
                return (*It);
            }
        }

        TVector<b3Vec3> Positions;
        TVector<int32> Indices;

        // Scale rides on b3CreateMeshShape, so the cached geometry stays unit scale and is shared.
        if (!GatherMeshResourceGeometry(Mesh->GetMeshResource(), FVector3(1.0f), Positions, &Indices) || Indices.IsEmpty())
        {
            return nullptr;
        }

        b3MeshDef Def = b3MeshDef{};
        Def.vertices = Positions.GetData();
        Def.vertexCount = Positions.Num();
        Def.indices = Indices.GetData();
        Def.triangleCount = Indices.Num() / 3;
        Def.weldVertices = true;
        Def.identifyEdges = true;

        b3MeshData* Built = b3CreateMesh(&Def, nullptr, 0);
        if (Built == nullptr)
        {
            return nullptr;
        }

        TScopeLock Lock(MeshCacheMutex);
        if (!MeshCache.TryAdd(Mesh, Built))
        {
            b3DestroyMesh(Built);
        }
        return MeshCache.FindChecked(Mesh);
    }

    bool FBox3DPhysicsScene::BuildCompoundShapes(const SCompoundColliderComponent& Comp, const STransformComponent& Transform, TVector<FPendingShape>& OutShapes)
    {
        const FVector3 Scale = Transform.ComputeWorldTransform().GetScale();
        const float UniformScale = Math::Max(Scale.x, Math::Max(Scale.y, Scale.z));

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
                        OutShapes.Add(MakeHullShape(Hull, ChildOffset, ChildRotation));
                    }
                    break;
                }
                case ECompoundShapeType::Sphere:
                {
                    OutShapes.Add(MakeSphereShape(ChildOffset, Child.Radius * UniformScale));
                    break;
                }
                case ECompoundShapeType::Capsule:
                {
                    OutShapes.Add(MakeCapsuleShape(ChildOffset, ChildRotation, Child.Radius * UniformScale, Child.HalfHeight * UniformScale));
                    break;
                }
                default:
                {
                    if (const b3HullData* Hull = GetOrCreateBoxHull(Child.HalfExtent * Scale))
                    {
                        OutShapes.Add(MakeHullShape(Hull, ChildOffset, ChildRotation));
                    }
                    break;
                }
            }
        }

        return !OutShapes.IsEmpty();
    }

    bool FBox3DPhysicsScene::BuildMeshCollision(const SMeshCollision& Collision, const FVector3& Scale, TVector<FPendingShape>& OutShapes)
    {
        const float UniformScale = Math::Max(Math::Max(Math::Abs(Scale.x), Math::Abs(Scale.y)), Math::Abs(Scale.z));

        for (const SCollisionPrimitive& Primitive : Collision.Primitives)
        {
            const FQuat Rotation(Math::Radians(Primitive.Rotation));
            const FVector3 Offset = Primitive.Center * Scale;

            switch (Primitive.Type)
            {
                case ECollisionPrimitiveType::Box:
                {
                    if (const b3HullData* Hull = GetOrCreateBoxHull(Primitive.HalfExtent * Scale))
                    {
                        OutShapes.Add(MakeHullShape(Hull, Offset, Rotation));
                    }
                    break;
                }
                case ECollisionPrimitiveType::Sphere:
                {
                    OutShapes.Add(MakeSphereShape(Offset, Primitive.Radius * UniformScale));
                    break;
                }
                case ECollisionPrimitiveType::Capsule:
                {
                    OutShapes.Add(MakeCapsuleShape(Offset, Rotation, Primitive.Radius * UniformScale, Primitive.HalfHeight * UniformScale));
                    break;
                }
                case ECollisionPrimitiveType::ConvexHull:
                {
                    TVector<b3Vec3> Points;
                    Points.Reserve(Primitive.HullPoints.Num());
                    for (const FVector3& P : Primitive.HullPoints)
                    {
                        Points.Add(Box3DUtils::ToB3Vec3(P * Scale));
                    }

                    if (Points.Num() < 4)
                    {
                        break;
                    }

                    // A hull point set belongs to one asset, so there is nothing a shared cache could hit.
                    if (b3HullData* Hull = b3CreateHull(Points.GetData(), Points.Num(), B3_MAX_HULL_VERTICES))
                    {
                        TrackOwnedHull(Hull);
                        OutShapes.Add(MakeHullShape(Hull, Offset, Rotation));
                    }
                    break;
                }
                default:
                    break;
            }
        }

        if (Collision.IsConcave() && !Collision.TriangleVertices.IsEmpty() && !Collision.TriangleIndices.IsEmpty())
        {
            TVector<b3Vec3> Points;
            TVector<int32> Indices;
            Points.Reserve(Collision.TriangleVertices.Num());
            for (const FVector3& P : Collision.TriangleVertices)
            {
                Points.Add(Box3DUtils::ToB3Vec3(P));
            }
            Indices.Reserve(Collision.TriangleIndices.Num());
            for (uint32 Index : Collision.TriangleIndices)
            {
                Indices.Add((int32)Index);
            }

            b3MeshDef Def = b3MeshDef{};
            Def.vertices = Points.GetData();
            Def.vertexCount = Points.Num();
            Def.indices = Indices.GetData();
            Def.triangleCount = Indices.Num() / 3;
            Def.weldVertices = true;
            Def.identifyEdges = true;

            if (b3MeshData* Built = b3CreateMesh(&Def, nullptr, 0))
            {
                TrackOwnedMesh(Built);

                FPendingShape Shape;
                Shape.Type = b3_meshShape;
                Shape.Mesh = Built;
                Shape.Scale = Box3DUtils::ToB3Vec3(Scale);
                OutShapes.Add(Shape);
            }
        }

        return !OutShapes.IsEmpty();
    }
}
