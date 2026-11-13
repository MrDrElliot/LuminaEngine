#include <gtest/gtest.h>

#include "Assets/AssetTypes/GeometryCollection/GeometryCollection.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Core/Object/ObjectCore.h"
#include "Physics/Physics.h"
#include "Renderer/MeshData.h"
#include "World/World.h"
#include "World/Entity/Components/DestructibleComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Scene/RenderScene/SceneMeshes.h"
#include "World/Subsystems/WorldSettings.h"

using namespace Lumina;

namespace
{
    template<typename TGenerate>
    CStaticMesh* MakePrimitiveMesh(TGenerate&& Generate)
    {
        TUniquePtr<FMeshResource> Resource = MakeUnique<FMeshResource>();
        TVector<FSourceVertex> Vertices;
        Generate(Vertices, Resource->Indices);
        Resource->ReserveVertices(Vertices.Num());
        for (const FSourceVertex& Vertex : Vertices)
        {
            Resource->AppendVertex(Vertex);
        }

        FGeometrySurface Surface;
        Surface.ID         = "Primitive";
        Surface.IndexCount = static_cast<uint32>(Resource->Indices.Num());
        Resource->GeometrySurfaces.Add(Surface);

        CStaticMesh* Mesh = NewObject<CStaticMesh>();
        Mesh->SetMeshResource(Move(Resource));
        return Mesh;
    }

    // A box whose gridded top has its center sunk by less than the hull tolerance, listing the dented triangles before the flat ones.
    void GenerateDentedBox(TVector<FSourceVertex>& OutVertices, TVector<uint32>& OutIndices)
    {
        constexpr int32 GridSize = 5;
        constexpr uint32 CenterVertex = GridSize * GridSize / 2;
        constexpr float DentDepth = 0.002f;

        auto AddVertex = [&](float X, float Y, float Z)
        {
            FSourceVertex Vertex;
            Vertex.Position = FVector3(X, Y, Z);
            OutVertices.Add(Vertex);
            return static_cast<uint32>(OutVertices.Num() - 1);
        };

        for (int32 Row = 0; Row < GridSize; ++Row)
        {
            for (int32 Col = 0; Col < GridSize; ++Col)
            {
                const bool bCenter = static_cast<uint32>(Row * GridSize + Col) == CenterVertex;
                AddVertex(-1.0f + 0.5f * Col, -1.0f + 0.5f * Row, bCenter ? 1.0f - DentDepth : 1.0f);
            }
        }

        TVector<uint32> FlatIndices;
        auto AddTriangle = [&](uint32 A, uint32 B, uint32 C)
        {
            const bool bDented = A == CenterVertex || B == CenterVertex || C == CenterVertex;
            for (uint32 Index : { A, B, C })
            {
                (bDented ? OutIndices : FlatIndices).Add(Index);
            }
        };
        auto AddQuad = [&](uint32 A, uint32 B, uint32 C, uint32 D)
        {
            AddTriangle(A, B, C);
            AddTriangle(A, C, D);
        };
        for (int32 Row = 0; Row + 1 < GridSize; ++Row)
        {
            for (int32 Col = 0; Col + 1 < GridSize; ++Col)
            {
                const uint32 A = Row * GridSize + Col;
                const uint32 C = A + GridSize + 1;
                AddQuad(A, A + 1, C, C - 1);
            }
        }

        const uint32 Top[4] = { 0, GridSize - 1, GridSize * GridSize - 1, GridSize * (GridSize - 1) };
        const uint32 Bottom[4] = { AddVertex(-1, -1, 0), AddVertex(1, -1, 0), AddVertex(1, 1, 0), AddVertex(-1, 1, 0) };
        for (int32 Side = 0; Side < 4; ++Side)
        {
            const int32 Next = (Side + 1) % 4;
            AddQuad(Bottom[Side], Bottom[Next], Top[Next], Top[Side]);
        }
        AddQuad(Bottom[0], Bottom[3], Bottom[2], Bottom[1]);
        for (uint32 Index : FlatIndices)
        {
            OutIndices.Add(Index);
        }
    }

    struct FFractureResult
    {
        int32 Pieces = 0;
        int32 Triangles = 0;
    };

    FFractureResult FractureMesh(const CStaticMesh* Mesh)
    {
        FFractureSettings Settings;
        Settings.NumPieces = 12;
        Settings.Seed      = 1234u;

        TVector<FFracturePiece> Pieces;
        Fracture::GenerateConvexFracture(Mesh, Settings, Pieces);

        FFractureResult Result;
        Result.Pieces = Pieces.Num();
        for (const FFracturePiece& Piece : Pieces)
        {
            Result.Triangles += Piece.Indices.Num() / 3;
        }
        return Result;
    }
}

// Counts were recorded before coplanar triangles shared one hull plane scan, so any drift in the hull shows up here.
TEST(GeometryCollectionFracture, PieceAndTriangleCountsMatchTheBaseline)
{
    const FFractureResult Cube = FractureMesh(MakePrimitiveMesh(PrimitiveMeshes::GenerateCube));
    EXPECT_EQ(Cube.Pieces, 12);
    EXPECT_EQ(Cube.Triangles, 284);

    const FFractureResult Cylinder = FractureMesh(MakePrimitiveMesh([](auto& V, auto& I) { PrimitiveMeshes::GenerateCylinder(V, I); }));
    EXPECT_EQ(Cylinder.Pieces, 12);
    EXPECT_EQ(Cylinder.Triangles, 584);

    const FFractureResult Sphere = FractureMesh(MakePrimitiveMesh([](auto& V, auto& I) { PrimitiveMeshes::GenerateSphere(V, I); }));
    EXPECT_EQ(Sphere.Pieces, 12);
    EXPECT_EQ(Sphere.Triangles, 1508);

    const FFractureResult DenseSphere = FractureMesh(MakePrimitiveMesh([](auto& V, auto& I) { PrimitiveMeshes::GenerateSphere(V, I, 48, 96); }));
    EXPECT_EQ(DenseSphere.Pieces, 12);
    EXPECT_EQ(DenseSphere.Triangles, 7646);

    const FFractureResult DentedBox = FractureMesh(MakePrimitiveMesh(GenerateDentedBox));
    EXPECT_EQ(DentedBox.Pieces, 12);
    EXPECT_EQ(DentedBox.Triangles, 280);
}

namespace
{
    template<typename TGenerate>
    int32 CountHullPlanes(TGenerate&& Generate, const FVector3& Scale)
    {
        TVector<FSourceVertex> Vertices;
        TVector<uint32> Indices;
        Generate(Vertices, Indices);

        TVector<FVector3> Positions;
        for (const FSourceVertex& Vertex : Vertices)
        {
            Positions.Add(Vertex.Position * Scale);
        }
        TVector<FUIntVector3> Triangles;
        for (int32 Index = 0; Index + 2 < Indices.Num(); Index += 3)
        {
            Triangles.Emplace(Indices[Index], Indices[Index + 1], Indices[Index + 2]);
        }

        constexpr float HullTolerance = 0.003f;
        TVector<FVector4> HullPlanes;
        Fracture::ComputeHullPlanes(Positions, Triangles, HullTolerance, HullPlanes);
        return HullPlanes.Num();
    }
}

// The dented triangles are scanned first and are not hull faces, which must not hide the flat top plane they nearly match.
TEST(GeometryCollectionFracture, NonHullTrianglesDoNotHideANearlyCoplanarHullFace)
{
    EXPECT_EQ(CountHullPlanes(GenerateDentedBox, FVector3(1.0f)), 6);
}

// A slab thinner than the hull tolerance keeps both of its broad faces.
TEST(GeometryCollectionFracture, ThinSlabKeepsBothBroadFaces)
{
    EXPECT_EQ(CountHullPlanes(PrimitiveMeshes::GenerateCube, FVector3(1.0f, 0.0005f, 1.0f)), 6);
}

// Zero physics body headroom clamps MaxFragments to zero, so FractureEntity must fail and leave the entity untouched.
TEST(GeometryCollectionFracture, NoPhysicsHeadroomLeavesTheOriginalEntityIntact)
{
    RegisterPendingTypes();
    if (Physics::GetPhysicsContext() == nullptr)
    {
        Physics::Initialize();
    }

    TStrongObjectPtr<CWorld> WorldOwner = NewObject<CWorld>(nullptr, NAME_None, FGuid::New(), OF_Transient);
    CWorld* World = WorldOwner.Get();
    ECS::FRegistry& Registry = World->GetEntityRegistry();

    SDefaultWorldSettings& Settings = World->GetDefaultWorldSettings();
    const uint32 SavedCap = Settings.MaxPhysicsBodies;
    Settings.MaxPhysicsBodies = 16;
    World->EnsurePhysicsScene();
    Settings.MaxPhysicsBodies = SavedCap;

    CStaticMesh* Mesh = MakePrimitiveMesh(PrimitiveMeshes::GenerateCube);

    const ECS::FEntity Entity = Registry.Create();
    Registry.Emplace<STransformComponent>(Entity);
    Registry.Emplace<SStaticMeshComponent>(Entity).SetStaticMesh(Mesh);
    Registry.Emplace<SRigidBodyComponent>(Entity);
    Registry.Emplace<SBoxColliderComponent>(Entity).HalfExtent = FVector3(0.5f);
    Registry.Emplace<SDestructibleComponent>(Entity);

    EXPECT_FALSE(World->FractureEntity(Entity, FVector3(0.0f), 1.0f));
    EXPECT_FALSE(Registry.Get<SDestructibleComponent>(Entity).bFractured);
    EXPECT_NE(Registry.TryGet<SStaticMeshComponent>(Entity), nullptr);
    EXPECT_NE(Registry.TryGet<SRigidBodyComponent>(Entity), nullptr);
    EXPECT_NE(Registry.TryGet<SBoxColliderComponent>(Entity), nullptr);
}
