#include "RuntimePCH.h"
#include "GeometryCollection.h"
#include <cfloat>
#include <cmath>
#include "EASTL/sort.h"
#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Core/Object/Package/Package.h"
#include "Memory/SmartPtr.h"
#include "Renderer/MeshData.h"
#include "Renderer/Vertex.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        // Deterministic xorshift32 -- same seed reproduces the same fracture (replay/lockstep friendly).
        struct FRng
        {
            uint32 State;
            explicit FRng(uint32 Seed) : State(Seed ? Seed : 0x9E3779B9u) {}
            uint32 Next() { State ^= State << 13; State ^= State >> 17; State ^= State << 5; return State; }
            float  Unit() { return static_cast<float>(Next() & 0xFFFFFFu) / static_cast<float>(0x1000000); } // [0,1)
        };

        using FFace = TVector<FVector3>;
        struct FConvexPoly { TVector<FFace> Faces; };

        FConvexPoly MakeBox(const FVector3& Mn, const FVector3& Mx)
        {
            auto Quad = [](const FVector3& A, const FVector3& B, const FVector3& C, const FVector3& D)
            {
                FFace F; F.push_back(A); F.push_back(B); F.push_back(C); F.push_back(D); return F;
            };

            FConvexPoly P;
            P.Faces.push_back(Quad({ Mn.x, Mn.y, Mn.z }, { Mn.x, Mx.y, Mn.z }, { Mn.x, Mx.y, Mx.z }, { Mn.x, Mn.y, Mx.z }));
            P.Faces.push_back(Quad({ Mx.x, Mn.y, Mn.z }, { Mx.x, Mx.y, Mn.z }, { Mx.x, Mx.y, Mx.z }, { Mx.x, Mn.y, Mx.z }));
            P.Faces.push_back(Quad({ Mn.x, Mn.y, Mn.z }, { Mx.x, Mn.y, Mn.z }, { Mx.x, Mn.y, Mx.z }, { Mn.x, Mn.y, Mx.z }));
            P.Faces.push_back(Quad({ Mn.x, Mx.y, Mn.z }, { Mx.x, Mx.y, Mn.z }, { Mx.x, Mx.y, Mx.z }, { Mn.x, Mx.y, Mx.z }));
            P.Faces.push_back(Quad({ Mn.x, Mn.y, Mn.z }, { Mx.x, Mn.y, Mn.z }, { Mx.x, Mx.y, Mn.z }, { Mn.x, Mx.y, Mn.z }));
            P.Faces.push_back(Quad({ Mn.x, Mn.y, Mx.z }, { Mx.x, Mn.y, Mx.z }, { Mx.x, Mx.y, Mx.z }, { Mn.x, Mx.y, Mx.z }));
            return P;
        }

        // Order coplanar points (lying on a plane with normal N) into a convex CCW loop.
        FFace OrderCoplanarLoop(const TVector<FVector3>& Pts, const FVector3& N, float Eps)
        {
            TVector<FVector3> Unique;
            for (const FVector3& P : Pts)
            {
                bool bDup = false;
                for (const FVector3& Q : Unique)
                {
                    if (Math::Distance(P, Q) <= Eps) { bDup = true; break; }
                }
                if (!bDup) Unique.push_back(P);
            }
            if (Unique.size() < 3) return {};

            FVector3 Center(0.0f);
            for (const FVector3& P : Unique) Center += P;
            Center /= static_cast<float>(Unique.size());

            const FVector3 Ref = Math::Abs(N.x) < 0.9f ? FVector3(1, 0, 0) : FVector3(0, 1, 0);
            const FVector3 U = Math::Normalize(Math::Cross(N, Ref));
            const FVector3 V = Math::Cross(N, U);

            eastl::sort(Unique.begin(), Unique.end(), [&](const FVector3& A, const FVector3& B)
            {
                const float AngleA = std::atan2(Math::Dot(A - Center, V), Math::Dot(A - Center, U));
                const float AngleB = std::atan2(Math::Dot(B - Center, V), Math::Dot(B - Center, U));
                return AngleA < AngleB;
            });
            return Unique;
        }

        // Clip the convex polyhedron by the half-space { x : dot(N,x) - D <= 0 }, capping the cut.
        void ClipPolyByPlane(FConvexPoly& Poly, const FVector3& N, float D, float Eps)
        {
            TVector<FFace>     NewFaces;
            TVector<FVector3> CapPoints;

            for (const FFace& Face : Poly.Faces)
            {
                FFace Out;
                const size_t L = Face.size();
                for (size_t k = 0; k < L; ++k)
                {
                    const FVector3& A = Face[k];
                    const FVector3& B = Face[(k + 1) % L];
                    const float da = Math::Dot(N, A) - D;
                    const float db = Math::Dot(N, B) - D;

                    if (da <= Eps)
                    {
                        Out.push_back(A);
                    }
                    if ((da < -Eps && db > Eps) || (da > Eps && db < -Eps))
                    {
                        const float t = da / (da - db);
                        const FVector3 P = A + t * (B - A);
                        Out.push_back(P);
                        CapPoints.push_back(P);
                    }
                }
                if (Out.size() >= 3)
                {
                    NewFaces.push_back(Move(Out));
                }
            }

            if (CapPoints.size() >= 3)
            {
                FFace Cap = OrderCoplanarLoop(CapPoints, N, Eps);
                if (Cap.size() >= 3)
                {
                    NewFaces.push_back(Move(Cap));
                }
            }

            Poly.Faces = Move(NewFaces);
        }

        FVector3 NewellNormal(const FFace& Face)
        {
            FVector3 N(0.0f);
            const size_t L = Face.size();
            for (size_t k = 0; k < L; ++k)
            {
                const FVector3& A = Face[k];
                const FVector3& B = Face[(k + 1) % L];
                N.x += (A.y - B.y) * (A.z + B.z);
                N.y += (A.z - B.z) * (A.x + B.x);
                N.z += (A.x - B.x) * (A.y + B.y);
            }
            return N;
        }

        // Triangulate the convex polyhedron into a flat-shaded piece (hard edges, outward winding).
        bool BuildPieceFromPoly(const FConvexPoly& Poly, FFracturePiece& Out)
        {
            if (Poly.Faces.size() < 4)
            {
                return false;
            }

            FVector3 Centroid(0.0f);
            uint32 Count = 0;
            for (const FFace& Face : Poly.Faces)
            {
                for (const FVector3& V : Face) { Centroid += V; ++Count; }
            }
            if (Count == 0)
            {
                return false;
            }
            Centroid /= static_cast<float>(Count);

            FVector3 Mn(FLT_MAX);
            FVector3 Mx(-FLT_MAX);
            Out.Vertices.clear();
            Out.Indices.clear();

            for (const FFace& SrcFace : Poly.Faces)
            {
                if (SrcFace.size() < 3)
                {
                    continue;
                }

                FVector3 Newell = NewellNormal(SrcFace);
                if (Math::Length(Newell) < 1e-10f)
                {
                    continue;
                }
                Newell = Math::Normalize(Newell);

                FVector3 FaceCenter(0.0f);
                for (const FVector3& V : SrcFace) FaceCenter += V;
                FaceCenter /= static_cast<float>(SrcFace.size());

                // Reverse the loop if its winding faces inward, so emitted triangles face outward.
                FFace Face = SrcFace;
                FVector3 Normal = Newell;
                if (Math::Dot(Newell, FaceCenter - Centroid) < 0.0f)
                {
                    eastl::reverse(Face.begin(), Face.end());
                    Normal = -Newell;
                }

                FVector3 Tangent = Math::Abs(Normal.x) < 0.9f ? FVector3(1, 0, 0) : FVector3(0, 1, 0);
                Tangent = Math::Normalize(Tangent - Normal * Math::Dot(Tangent, Normal));
                const FVector3 Bitangent = Math::Cross(Normal, Tangent);

                const uint32 PackedNormal  = PackNormal(Normal);
                const uint32 PackedTangent = PackTangent(Tangent, 1.0f);
                const uint32 PackedColor   = PackColor(FVector4(1.0f));

                const uint32 Base = static_cast<uint32>(Out.Vertices.size());
                for (const FVector3& P : Face)
                {
                    FVertex Vert;
                    Vert.Position = P;
                    Vert.Normal   = PackedNormal;
                    Vert.Tangent  = PackedTangent;
                    Vert.UV       = Math::PackHalf2x16(FVector2(Math::Dot(P, Tangent), Math::Dot(P, Bitangent)));
                    Vert.Color    = PackedColor;
                    Out.Vertices.push_back(Vert);

                    Mn = Math::Min(Mn, P);
                    Mx = Math::Max(Mx, P);
                }

                // Fan triangulation of the convex loop.
                const uint32 L = static_cast<uint32>(Face.size());
                for (uint32 k = 1; k + 1 < L; ++k)
                {
                    Out.Indices.push_back(Base);
                    Out.Indices.push_back(Base + k);
                    Out.Indices.push_back(Base + k + 1);
                }
            }

            if (Out.Vertices.size() < 4 || Out.Indices.size() < 12)
            {
                return false;
            }

            Out.Center = Centroid;
            Out.Bounds = FAABB(Mn, Mx);
            return true;
        }

        // Pull LOD0 positions + triangles out of the mesh's meshlet data (the scratch vertex
        // streams are dropped after GPU upload, so dequantize from the meshlets like the collider does).
        void GatherMeshGeometry(const CMesh* Mesh, TVector<FVector3>& OutPositions, TVector<FUIntVector3>& OutTriangles)
        {
            const FMeshResource& Resource = Mesh->GetMeshResource();
            const FMeshletData&  MD       = Resource.MeshletData;
            if (MD.IsEmpty() || Resource.bSkinnedMesh)
            {
                return;
            }

            Mesh->ForEachSurface([&](const FGeometrySurface& Surface, uint32)
            {
                const uint32 Offset = Surface.LODMeshletOffset[0];
                const uint32 Count  = Surface.LODMeshletCount[0];
                for (uint32 i = 0; i < Count; ++i)
                {
                    const FMeshlet& M       = MD.Meshlets[Offset + i];
                    const uint32 BaseVertex = static_cast<uint32>(OutPositions.size());

                    for (uint32 v = 0; v < M.VertexCount; ++v)
                    {
                        OutPositions.push_back(MD.MeshletVertices[M.VertexOffset + v].Position);
                    }

                    for (uint32 t = 0; t < M.TriangleCount; ++t)
                    {
                        const uint32 Packed = MD.MeshletTriangles[M.TriangleOffset + t];
                        OutTriangles.push_back(FUIntVector3(
                            BaseVertex + ( Packed        & 0xFFu),
                            BaseVertex + ((Packed >>  8) & 0xFFu),
                            BaseVertex + ((Packed >> 16) & 0xFFu)));
                    }
                }
            });
        }

        // Convex-hull supporting planes (normal N, offset D; interior is dot(N,x) <= D). A triangle's
        // plane is a hull face iff every vertex sits on one side.
        void ComputeHullPlanes(const TVector<FVector3>& Positions, const TVector<FUIntVector3>& Triangles, float Tol, TVector<FVector4>& OutPlanes)
        {
            auto AddUnique = [&](const FVector3& N, float D)
            {
                for (const FVector4& Existing : OutPlanes)
                {
                    if (Math::Dot(FVector3(Existing), N) > 0.999f && Math::Abs(Existing.w - D) <= Tol)
                    {
                        return;
                    }
                }
                OutPlanes.push_back(FVector4(N, D));
            };

            for (const FUIntVector3& Tri : Triangles)
            {
                const FVector3& A = Positions[Tri.x];
                const FVector3& B = Positions[Tri.y];
                const FVector3& C = Positions[Tri.z];

                FVector3 N = Math::Cross(B - A, C - A);
                const float Len = Math::Length(N);
                if (Len < 1e-12f)
                {
                    continue;
                }
                N /= Len;
                const float D = Math::Dot(N, A);

                float MaxProj = -FLT_MAX;
                float MinProj =  FLT_MAX;
                for (const FVector3& P : Positions)
                {
                    const float d = Math::Dot(N, P);
                    MaxProj = Math::Max(MaxProj, d);
                    MinProj = Math::Min(MinProj, d);
                }

                if (MaxProj <= D + Tol)       AddUnique( N,  D);   // mesh on the negative side -> outward = N
                else if (MinProj >= D - Tol)  AddUnique(-N, -D);   // mesh on the positive side -> outward = -N
            }
        }
    }

    void Fracture::GenerateConvexFracture(const CMesh* SourceMesh, const FFractureSettings& Settings, TVector<FFracturePiece>& OutPieces)
    {
        OutPieces.clear();
        if (SourceMesh == nullptr)
        {
            return;
        }

        const FAABB Bounds = SourceMesh->GetAABB();
        const FVector3 Mn = Bounds.Min;
        const FVector3 Mx = Bounds.Max;
        const FVector3 Size = Math::Max(Mx - Mn, FVector3(1e-4f));
        const float Diag    = Math::Length(Size);
        const float Eps     = Diag * 1e-5f + 1e-6f;
        const float HullTol = Diag * 1e-3f;

        // Hull supporting planes from the mesh triangles.
        TVector<FVector4> HullPlanes;
        size_t NumPos = 0;
        size_t NumTri = 0;
        {
            TVector<FVector3>  Positions;
            TVector<FUIntVector3> Triangles;
            GatherMeshGeometry(SourceMesh, Positions, Triangles);
            NumPos = Positions.size();
            NumTri = Triangles.size();
            if (Positions.size() >= 4 && !Triangles.empty())
            {
                ComputeHullPlanes(Positions, Triangles, HullTol, HullPlanes);
            }
        }

        // Clip the bounds box down to the hull once; every cell starts from this shape.
        FConvexPoly Hull = MakeBox(Mn, Mx);
        for (const FVector4& Plane : HullPlanes)
        {
            ClipPolyByPlane(Hull, FVector3(Plane), Plane.w, Eps);
        }
        const size_t FacesAfterClip = Hull.Faces.size();
        if (Hull.Faces.size() < 4)
        {
            Hull = MakeBox(Mn, Mx);   // hull clip collapsed (bad data) -> fall back to box
        }

        LOG_INFO("[Fracture] MeshletVerts={} Tris={} HullPlanes={} HullFacesAfterClip={} BoundsDiag={}", NumPos, NumTri, HullPlanes.size(), FacesAfterClip, Diag);

        auto InsideHull = [&](const FVector3& S) -> bool
        {
            for (const FVector4& Plane : HullPlanes)
            {
                if (Math::Dot(FVector3(Plane), S) > Plane.w + HullTol)
                {
                    return false;
                }
            }
            return true;
        };

        const int32 N = Math::Clamp(Settings.NumPieces, 2, 512);
        FRng Rng(Settings.Seed);

        // Rejection-sample seeds inside the hull so cells fill the shape evenly.
        TVector<FVector3> Seeds;
        Seeds.reserve(N);
        for (int32 i = 0; i < N; ++i)
        {
            FVector3 S = Mn + Size * FVector3(Rng.Unit(), Rng.Unit(), Rng.Unit());
            for (int32 Attempt = 0; Attempt < 64 && !InsideHull(S); ++Attempt)
            {
                S = Mn + Size * FVector3(Rng.Unit(), Rng.Unit(), Rng.Unit());
            }
            Seeds.push_back(S);
        }

        OutPieces.reserve(N);
        for (int32 i = 0; i < N; ++i)
        {
            FConvexPoly Poly = Hull;   // start from the hull, not the box
            for (int32 j = 0; j < N && !Poly.Faces.empty(); ++j)
            {
                if (j == i)
                {
                    continue;
                }
                const FVector3 Delta = Seeds[j] - Seeds[i];
                const float Length = Math::Length(Delta);
                if (Length < Eps)
                {
                    continue;
                }
                const FVector3 Normal = Delta / Length;
                const FVector3 Mid = 0.5f * (Seeds[i] + Seeds[j]);
                ClipPolyByPlane(Poly, Normal, Math::Dot(Normal, Mid), Eps);
            }

            FFracturePiece Piece;
            if (BuildPieceFromPoly(Poly, Piece))
            {
                OutPieces.push_back(Move(Piece));
            }
        }
    }

    CStaticMesh* Fracture::BuildPieceMesh(const FFracturePiece& Piece, const TVector<TObjectPtr<CMaterialInterface>>& Materials, const char* DebugName)
    {
        if (Piece.Vertices.empty() || Piece.Indices.empty())
        {
            return nullptr;
        }

        TUniquePtr<FMeshResource> Resource = MakeUnique<FMeshResource>();
        Resource->ReserveVertices(Piece.Vertices.size());
        for (const FVertex& V : Piece.Vertices)
        {
            // Recenter to the piece centroid so the mesh origin (and convex CoM) sits on the chunk;
            // callers place the entity at Piece.Center to reconstruct the original position.
            FVertex Centered = V;
            Centered.Position -= Piece.Center;
            Resource->AppendVertex(Centered);
        }
        Resource->Indices = Piece.Indices;

        FGeometrySurface Surface;
        Surface.ID            = "Piece";
        Surface.IndexCount    = static_cast<uint32>(Piece.Indices.size());
        Surface.StartIndex    = 0;
        Surface.MaterialIndex = 0;
        Resource->GeometrySurfaces.push_back(Surface);

        CStaticMesh* Mesh = NewObject<CStaticMesh>(CPackage::GetTransientPackage(), DebugName);
        Mesh->Materials = Materials;
        if (Mesh->Materials.empty())
        {
            Mesh->Materials.resize(1);
        }
        Mesh->SetMeshResource(Move(Resource));
        return Mesh;
    }

    void CGeometryCollection::Serialize(FArchive& Ar)
    {
        Super::Serialize(Ar);
        Ar << Data;
    }

    void CGeometryCollection::PostLoad()
    {
        Super::PostLoad();
        BuildPieceMeshes();
    }

    void CGeometryCollection::BuildPieceMeshes()
    {
        if (!PieceMeshes.empty() || Data.Pieces.empty())
        {
            return;
        }

        // Match the runtime fracture's material choice: the baked collection materials, else the source mesh's.
        const TVector<TObjectPtr<CMaterialInterface>>& Mats =
            (!Materials.empty() || SourceMesh.Get() == nullptr) ? Materials : SourceMesh->Materials;

        PieceMeshes.reserve(Data.Pieces.size());
        for (const FFracturePiece& Piece : Data.Pieces)
        {
            PieceMeshes.push_back(Fracture::BuildPieceMesh(Piece, Mats, "GCPiece"));
        }
    }

    const TVector<TObjectPtr<CStaticMesh>>& CGeometryCollection::GetPieceMeshes()
    {
        BuildPieceMeshes();
        return PieceMeshes;
    }

    int32 CGeometryCollection::Rebuild()
    {
        PieceMeshes.clear();   // invalidate the shared mesh cache; GetPieceMeshes() rebuilds on demand

        CStaticMesh* Source = SourceMesh.Get();
        if (Source == nullptr)
        {
            Data.Pieces.clear();
            return 0;
        }

        FFractureSettings Settings;
        Settings.NumPieces = NumPieces;
        Settings.Seed      = static_cast<uint32>(Seed);

        Data.SourceBounds = Source->GetAABB();
        Fracture::GenerateConvexFracture(Source, Settings, Data.Pieces);
        Materials = Source->Materials;
        return GetNumPieces();
    }

    CGeometryCollection* CGeometryCollection::GenerateFromMesh(CStaticMesh* Source, const FFractureSettings& Settings, CObject* Outer)
    {
        if (Source == nullptr)
        {
            return nullptr;
        }

        CPackage* Package = Outer ? Outer->GetPackage() : CPackage::GetTransientPackage();
        CGeometryCollection* Collection = NewObject<CGeometryCollection>(Package, "GeometryCollection");

        Collection->SourceMesh = Source;
        Collection->NumPieces  = Settings.NumPieces;
        Collection->Seed       = static_cast<int32>(Settings.Seed);
        Collection->Rebuild();
        return Collection;
    }
}
