#include "RuntimePCH.h"
#include "Memory/MemoryTracking.h"
#include "GeometryCollection.h"
#include <cfloat>
#include <cmath>
#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Core/Object/Package/Package.h"
#include "Memory/SmartPtr.h"
#include "Renderer/MeshData.h"
#include "Renderer/Vertex.h"
#include "Log/Log.h"
#include "Platform/Time/PlatformTime.h"
#include "Renderer/MeshQuantization.h"
#include "TaskSystem/TaskSystem.h"

namespace Lumina
{
    namespace
    {
        // A deterministic xorshift32, so the same seed reproduces the same fracture.
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
                FFace F; F.Add(A); F.Add(B); F.Add(C); F.Add(D); return F;
            };

            FConvexPoly P;
            P.Faces.Add(Quad({ Mn.x, Mn.y, Mn.z }, { Mn.x, Mx.y, Mn.z }, { Mn.x, Mx.y, Mx.z }, { Mn.x, Mn.y, Mx.z }));
            P.Faces.Add(Quad({ Mx.x, Mn.y, Mn.z }, { Mx.x, Mx.y, Mn.z }, { Mx.x, Mx.y, Mx.z }, { Mx.x, Mn.y, Mx.z }));
            P.Faces.Add(Quad({ Mn.x, Mn.y, Mn.z }, { Mx.x, Mn.y, Mn.z }, { Mx.x, Mn.y, Mx.z }, { Mn.x, Mn.y, Mx.z }));
            P.Faces.Add(Quad({ Mn.x, Mx.y, Mn.z }, { Mx.x, Mx.y, Mn.z }, { Mx.x, Mx.y, Mx.z }, { Mn.x, Mx.y, Mx.z }));
            P.Faces.Add(Quad({ Mn.x, Mn.y, Mn.z }, { Mx.x, Mn.y, Mn.z }, { Mx.x, Mx.y, Mn.z }, { Mn.x, Mx.y, Mn.z }));
            P.Faces.Add(Quad({ Mn.x, Mn.y, Mx.z }, { Mx.x, Mn.y, Mx.z }, { Mx.x, Mx.y, Mx.z }, { Mn.x, Mx.y, Mx.z }));
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
                if (!bDup) Unique.Add(P);
            }
            if (Unique.Num() < 3) return {};

            FVector3 Center(0.0f);
            for (const FVector3& P : Unique) Center += P;
            Center /= static_cast<float>(Unique.Num());

            const FVector3 Ref = Math::Abs(N.x) < 0.9f ? FVector3(1, 0, 0) : FVector3(0, 1, 0);
            const FVector3 U = Math::Normalize(Math::Cross(N, Ref));
            const FVector3 V = Math::Cross(N, U);

            Algo::Sort(Unique, [&](const FVector3& A, const FVector3& B)
            {
                const float AngleA = std::atan2(Math::Dot(A - Center, V), Math::Dot(A - Center, U));
                const float AngleB = std::atan2(Math::Dot(B - Center, V), Math::Dot(B - Center, U));
                return AngleA < AngleB;
            });
            return Unique;
        }

        // Clips the convex polyhedron by the half-space and caps the cut.
        void ClipPolyByPlane(FConvexPoly& Poly, const FVector3& N, float D, float Eps)
        {
            TVector<FVector3> CapPoints;

            // A face wholly inside the half-space clips to itself, so it stays in place without a rebuild.
            Poly.Faces.RemoveAll([&](FFace& Face)
            {
                if (Algo::AllOf(Face, [&](const FVector3& P) { return Math::Dot(N, P) - D <= Eps; }))
                {
                    return false;
                }

                FFace Out;
                const int32 L = Face.Num();
                for (int32 k = 0; k < L; ++k)
                {
                    const FVector3& A = Face[k];
                    const FVector3& B = Face[(k + 1) % L];
                    const float da = Math::Dot(N, A) - D;
                    const float db = Math::Dot(N, B) - D;

                    if (da <= Eps)
                    {
                        Out.Add(A);
                    }
                    if ((da < -Eps && db > Eps) || (da > Eps && db < -Eps))
                    {
                        const float t = da / (da - db);
                        const FVector3 P = A + t * (B - A);
                        Out.Add(P);
                        CapPoints.Add(P);
                    }
                }
                Face = Move(Out);
                return Face.Num() < 3;
            });

            if (CapPoints.Num() >= 3)
            {
                FFace Cap = OrderCoplanarLoop(CapPoints, N, Eps);
                if (Cap.Num() >= 3)
                {
                    Poly.Faces.Add(Move(Cap));
                }
            }
        }

        FVector3 NewellNormal(const FFace& Face)
        {
            FVector3 N(0.0f);
            const int32 L = Face.Num();
            for (int32 k = 0; k < L; ++k)
            {
                const FVector3& A = Face[k];
                const FVector3& B = Face[(k + 1) % L];
                N.x += (A.y - B.y) * (A.z + B.z);
                N.y += (A.z - B.z) * (A.x + B.x);
                N.z += (A.x - B.x) * (A.y + B.y);
            }
            return N;
        }

        struct FSurfaceTriangle
        {
            FVector3 Centroid;
            FVector3 Normal;    // zero for a degenerate triangle, so it never counts as coplanar
            FVector3 Corner;
            uint32   Color = 0u;
            uint32   Index = 0u;
        };

        // The source surface a piece is cut from, sorted by centroid along the widest axis so a face only visits the slab that can hold its nearest one.
        struct FSurfaceColors
        {
            TVector<FSurfaceTriangle> Triangles;
            int32                     Axis = 0;
            float                     Tolerance = 0.0f;

            FSurfaceColors(const TVector<FVector3>& Positions, const TVector<FUIntVector3>& SourceTriangles, const TVector<uint32>& Colors, const FVector3& Size, float InTolerance)
                : Axis(Size.x >= Size.y && Size.x >= Size.z ? 0 : (Size.y >= Size.z ? 1 : 2))
                , Tolerance(InTolerance)
            {
                Triangles.Reserve(SourceTriangles.Num());
                for (const FUIntVector3& Tri : SourceTriangles)
                {
                    const FVector3& A = Positions[Tri.x];
                    const FVector3& B = Positions[Tri.y];
                    const FVector3& C = Positions[Tri.z];
                    const FVector3 Cross = Math::Cross(B - A, C - A);
                    const float Length = Math::Length(Cross);
                    Triangles.Add({ (A + B + C) / 3.0f, Length < 1e-12f ? FVector3(0.0f) : Cross / Length, A, Colors[Tri.x], static_cast<uint32>(Triangles.Num()) });
                }
                Algo::Sort(Triangles, [this](const FSurfaceTriangle& L, const FSurfaceTriangle& R) { return L.Centroid[Axis] < R.Centroid[Axis]; });
            }
        };

        // An outer face lies in a source triangle's plane and takes its color; a fresh cut takes the nearest color, darkened like broken material.
        uint32 ColorForFace(const FSurfaceColors& Source, const FVector3& FaceCenter, const FVector3& Normal)
        {
            constexpr float CutShade = 0.55f;
            const FSurfaceTriangle* BestSurface = nullptr;
            const FSurfaceTriangle* BestAny = nullptr;
            float BestSurfaceSq = FLT_MAX;
            float BestAnySq = FLT_MAX;

            // Ties go to the lower source index, as the unsorted scan did.
            auto Closer = [](float DistanceSq, const FSurfaceTriangle& Tri, float BestSq, const FSurfaceTriangle* Best)
            {
                return DistanceSq < BestSq || (DistanceSq == BestSq && Best != nullptr && Tri.Index < Best->Index);
            };

            const TVector<FSurfaceTriangle>& Tris = Source.Triangles;
            const float Key = FaceCenter[Source.Axis];
            const int32 Split = static_cast<int32>(Algo::LowerBound(Tris, Key, [&](const FSurfaceTriangle& Tri, float Value) { return Tri.Centroid[Source.Axis] < Value; }) - Tris.begin());
            int32 Above = Split;
            int32 Below = Split - 1;
            while (Above < Tris.Num() || Below >= 0)
            {
                const float GapAbove = Above < Tris.Num() ? Tris[Above].Centroid[Source.Axis] - Key : FLT_MAX;
                const float GapBelow = Below >= 0 ? Key - Tris[Below].Centroid[Source.Axis] : FLT_MAX;
                const bool bTakeAbove = GapAbove <= GapBelow;
                const float Gap = bTakeAbove ? GapAbove : GapBelow;
                if (Gap * Gap > BestSurfaceSq)
                {
                    break;
                }

                const FSurfaceTriangle& Tri = Tris[bTakeAbove ? Above++ : Below--];
                const FVector3 Offset = Tri.Centroid - FaceCenter;
                const float DistanceSq = Math::Dot(Offset, Offset);
                if (Closer(DistanceSq, Tri, BestAnySq, BestAny))
                {
                    BestAnySq = DistanceSq;
                    BestAny = &Tri;
                }
                const bool bCoplanar = Math::Dot(Tri.Normal, Normal) > 0.9f
                    && Math::Abs(Math::Dot(FaceCenter - Tri.Corner, Tri.Normal)) <= Source.Tolerance;
                if (bCoplanar && Closer(DistanceSq, Tri, BestSurfaceSq, BestSurface))
                {
                    BestSurfaceSq = DistanceSq;
                    BestSurface = &Tri;
                }
            }

            if (BestSurface != nullptr)
            {
                return BestSurface->Color;
            }
            const FVector4 Cut = UnpackColor(BestAny->Color);
            return PackColor(FVector4(FVector3(Cut) * CutShade, Cut.w));
        }

        // Triangulate the convex polyhedron into a flat-shaded piece (hard edges, outward winding).
        bool BuildPieceFromPoly(const FConvexPoly& Poly, FFracturePiece& Out, const FSurfaceColors* Source = nullptr)
        {
            if (Poly.Faces.Num() < 4)
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
            Out.Vertices.Reset();
            Out.Indices.Reset();

            for (const FFace& SrcFace : Poly.Faces)
            {
                if (SrcFace.Num() < 3)
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
                FaceCenter /= static_cast<float>(SrcFace.Num());

                // Reverse the loop if its winding faces inward, so emitted triangles face outward.
                FFace Face = SrcFace;
                FVector3 Normal = Newell;
                if (Math::Dot(Newell, FaceCenter - Centroid) < 0.0f)
                {
                    Algo::Reverse(Face);
                    Normal = -Newell;
                }

                FVector3 Tangent = Math::Abs(Normal.x) < 0.9f ? FVector3(1, 0, 0) : FVector3(0, 1, 0);
                Tangent = Math::Normalize(Tangent - Normal * Math::Dot(Tangent, Normal));
                const FVector3 Bitangent = Math::Cross(Normal, Tangent);

                const uint32 PackedNormal  = PackNormal(Normal);
                const uint32 PackedTangent = PackTangent(Tangent, 1.0f);
                const uint32 PackedColor   = Source != nullptr ? ColorForFace(*Source, FaceCenter, Normal) : PackColor(FVector4(1.0f));

                const uint32 Base = static_cast<uint32>(Out.Vertices.Num());
                for (const FVector3& P : Face)
                {
                    FSourceVertex Vert;
                    Vert.Position = P;
                    Vert.Normal   = PackedNormal;
                    Vert.Tangent  = PackedTangent;
                    Vert.UV       = Math::PackHalf2x16(FVector2(Math::Dot(P, Tangent), Math::Dot(P, Bitangent)));
                    Vert.Color    = PackedColor;
                    Out.Vertices.Add(Vert);

                    Mn = Math::Min(Mn, P);
                    Mx = Math::Max(Mx, P);
                }

                // Fan triangulation of the convex loop.
                const uint32 L = static_cast<uint32>(Face.Num());
                for (uint32 k = 1; k + 1 < L; ++k)
                {
                    Out.Indices.Add(Base);
                    Out.Indices.Add(Base + k);
                    Out.Indices.Add(Base + k + 1);
                }
            }

            if (Out.Vertices.Num() < 4 || Out.Indices.Num() < 12)
            {
                return false;
            }

            Out.Center = Centroid;
            Out.Bounds = SAABB(Mn, Mx);
            return true;
        }

        // The scratch vertex streams are dropped after upload, so dequantize from the meshlets.
        void GatherMeshGeometry(const CMesh* Mesh, TVector<FVector3>& OutPositions, TVector<FUIntVector3>& OutTriangles, TVector<uint32>* OutColors = nullptr)
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
                    const uint32 BaseVertex = static_cast<uint32>(OutPositions.Num());

                    for (uint32 v = 0; v < M.VertexCount; ++v)
                    {
                        OutPositions.Add(GetMeshletVertexPosition(MD, M, v, false));
                        if (OutColors != nullptr)
                        {
                            OutColors->Add(MD.VertexColors.IsEmpty() ? 0xFFFFFFFFu : MD.VertexColors[GetStaticVertexIndex(MD, M, v)]);
                        }
                    }

                    for (uint32 t = 0; t < M.TriangleCount; ++t)
                    {
                        const uint32 Packed = MD.MeshletTriangles[M.TriangleOffset + t];
                        OutTriangles.Emplace(
                            BaseVertex + ( Packed        & 0xFFu),
                            BaseVertex + ((Packed >>  8) & 0xFFu),
                            BaseVertex + ((Packed >> 16) & 0xFFu));
                    }
                }
            });
        }
    }

    void Fracture::ComputeHullPlanes(const TVector<FVector3>& Positions, const TVector<FUIntVector3>& Triangles, float Tol, TVector<FVector4>& OutPlanes)
    {
        LUMINA_PROFILE_SCOPE();
        OutPlanes.Reset();

        // Accepted planes sit in a grid over (normal, distance), so coplanar triangles skip the scan of every position.
        constexpr float MinNormalDot = 0.999f;
        const FVector4 Margin(0.05f, 0.05f, 0.05f, Tol);   // 0.05 bounds each normal component's drift while the dot passes MinNormalDot
        const FVector4 CellSize = Margin * 4.0f;

        THashMap<uint64, int32> CellHeads;
        TVector<int32> NextInCell;

        auto CellKey = [](const int32 (&Cell)[4]) -> uint64
        {
            return (uint64(uint32(Cell[3])) << 32) | (uint64(Cell[0] & 0x3FF) << 20) | (uint64(Cell[1] & 0x3FF) << 10) | uint64(Cell[2] & 0x3FF);
        };

        // NearSide steps toward the neighboring cell a plane within Margin of this one could land in.
        auto CellOf = [&](const FVector4& Plane, int32 (&Cell)[4], int32 (&NearSide)[4])
        {
            for (int32 Axis = 0; Axis < 4; ++Axis)
            {
                const float Scaled = Plane[Axis] / CellSize[Axis];
                Cell[Axis] = static_cast<int32>(std::floor(Scaled));
                const float IntoCell = (Scaled - static_cast<float>(Cell[Axis])) * CellSize[Axis];
                NearSide[Axis] = IntoCell < Margin[Axis] ? -1 : (IntoCell > CellSize[Axis] - Margin[Axis] ? 1 : 0);
            }
        };

        auto IsKnownHullPlane = [&](const FVector4& Plane)
        {
            int32 Base[4];
            int32 NearSide[4];
            CellOf(Plane, Base, NearSide);
            for (uint32 Neighbor = 0; Neighbor < 16; ++Neighbor)
            {
                int32 Cell[4];
                bool bReachable = true;
                for (int32 Axis = 0; Axis < 4; ++Axis)
                {
                    const bool bStep = ((Neighbor >> Axis) & 1u) != 0;
                    bReachable &= !bStep || NearSide[Axis] != 0;
                    Cell[Axis] = Base[Axis] + (bStep ? NearSide[Axis] : 0);
                }
                const int32* Head = bReachable ? CellHeads.Find(CellKey(Cell)) : nullptr;
                for (int32 Index = Head ? *Head : -1; Index != -1; Index = NextInCell[Index])
                {
                    const FVector4& Existing = OutPlanes[Index];
                    if (Math::Dot(FVector3(Existing), FVector3(Plane)) > MinNormalDot && Math::Abs(Existing.w - Plane.w) <= Tol)
                    {
                        return true;
                    }
                }
            }
            return false;
        };

        auto AddHullPlane = [&](const FVector4& Outward)
        {
            if (IsKnownHullPlane(Outward))
            {
                return;
            }
            int32 Cell[4];
            int32 NearSide[4];
            CellOf(Outward, Cell, NearSide);
            int32& Head = CellHeads.FindOrAdd(CellKey(Cell), -1);
            NextInCell.Add(Head);
            Head = OutPlanes.Num();
            OutPlanes.Add(Outward);
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
            // Only the same facing proves the scan would add nothing; the opposite facing can be the other side of a thin slab.
            if (IsKnownHullPlane(FVector4(N, D)))
            {
                continue;
            }

            float MaxProj = -FLT_MAX;
            float MinProj =  FLT_MAX;
            for (const FVector3& P : Positions)
            {
                const float d = Math::Dot(N, P);
                MaxProj = Math::Max(MaxProj, d);
                MinProj = Math::Min(MinProj, d);
                if (MaxProj > D + Tol && MinProj < D - Tol)
                {
                    break;
                }
            }

            if (MaxProj <= D + Tol)       AddHullPlane(FVector4( N,  D));   // mesh on the negative side -> outward = N
            else if (MinProj >= D - Tol)  AddHullPlane(FVector4(-N, -D));   // mesh on the positive side -> outward = -N
        }
    }

    void Fracture::GenerateConvexFracture(const CMesh* SourceMesh, const FFractureSettings& Settings, TVector<FFracturePiece>& OutPieces)
    {
        OutPieces.Reset();
        if (SourceMesh == nullptr)
        {
            return;
        }

        const SAABB Bounds = SourceMesh->GetAABB();
        const FVector3 Mn = Bounds.Min;
        const FVector3 Mx = Bounds.Max;
        const FVector3 Size = Math::Max(Mx - Mn, FVector3(1e-4f));
        const float Diag    = Math::Length(Size);
        const float Eps     = Diag * 1e-5f + 1e-6f;
        const float HullTol = Diag * 1e-3f;

        // Hull supporting planes from the mesh triangles.
        TVector<FVector4> HullPlanes;
        TVector<FVector3>  Positions;
        TVector<FUIntVector3> Triangles;
        TVector<uint32> Colors;
        GatherMeshGeometry(SourceMesh, Positions, Triangles, &Colors);
        const int32 NumPos = Positions.Num();
        const int32 NumTri = Triangles.Num();
        PlatformTime::FStopwatch HullWatch;
        if (Positions.Num() >= 4 && !Triangles.IsEmpty())
        {
            ComputeHullPlanes(Positions, Triangles, HullTol, HullPlanes);
        }
        const double HullMs = HullWatch.ElapsedMilliseconds();
        const FSurfaceColors SurfaceColors(Positions, Triangles, Colors, Size, HullTol * 4.0f);
        const FSurfaceColors* ColorSource = Colors.Num() == Positions.Num() && !Triangles.IsEmpty() ? &SurfaceColors : nullptr;

        // Clip the bounds box down to the hull once; every cell starts from this shape.
        FConvexPoly Hull = MakeBox(Mn, Mx);
        for (const FVector4& Plane : HullPlanes)
        {
            ClipPolyByPlane(Hull, FVector3(Plane), Plane.w, Eps);
        }
        const int32 FacesAfterClip = Hull.Faces.Num();
        if (Hull.Faces.Num() < 4)
        {
            Hull = MakeBox(Mn, Mx);   // hull clip collapsed (bad data) -> fall back to box
        }

        LOG_DEBUG("[Fracture] MeshletVerts={} Tris={} HullPlanes={} HullFacesAfterClip={} BoundsDiag={} HullMs={:.2f}", NumPos, NumTri, HullPlanes.Num(), FacesAfterClip, Diag, HullMs);

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
        Seeds.Reserve(N);
        for (int32 i = 0; i < N; ++i)
        {
            FVector3 S = Mn + Size * FVector3(Rng.Unit(), Rng.Unit(), Rng.Unit());
            for (int32 Attempt = 0; Attempt < 64 && !InsideHull(S); ++Attempt)
            {
                S = Mn + Size * FVector3(Rng.Unit(), Rng.Unit(), Rng.Unit());
            }
            Seeds.Add(S);
        }

        // Every cell clips against every other seed, so the cells run in parallel into their own slots and keep seed order.
        TVector<FFracturePiece> Cells((size_t)N);
        TVector<uint8> bCellBuilt((size_t)N, 0);
        Jobs::ParallelFor((uint32)N, [&](uint32 CellIndex)
        {
            const int32 i = (int32)CellIndex;
            FConvexPoly Poly = Hull;   // start from the hull, not the box
            for (int32 j = 0; j < N && !Poly.Faces.IsEmpty(); ++j)
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

            bCellBuilt[i] = BuildPieceFromPoly(Poly, Cells[i], ColorSource) ? 1 : 0;
        });

        OutPieces.Reserve(N);
        for (int32 i = 0; i < Cells.Num(); ++i)
        {
            if (bCellBuilt[i] != 0)
            {
                OutPieces.Add(Move(Cells[i]));
            }
        }
    }

    CStaticMesh* Fracture::BuildPieceMesh(const FFracturePiece& Piece, const TVector<TStrongObjectPtr<CMaterialInterface>>& Materials, const char* DebugName)
    {
        if (Piece.Vertices.IsEmpty() || Piece.Indices.IsEmpty())
        {
            return nullptr;
        }

        TUniquePtr<FMeshResource> Resource = MakeUnique<FMeshResource>();
        Resource->ReserveVertices(Piece.Vertices.Num());
        for (const FSourceVertex& V : Piece.Vertices)
        {
            // Callers place the entity at the piece center to reconstruct the original position.
            FSourceVertex Centered = V;
            Centered.Position -= Piece.Center;
            Resource->AppendVertex(Centered);
        }
        Resource->Indices = Piece.Indices;

        FGeometrySurface Surface;
        Surface.ID            = "Piece";
        Surface.IndexCount    = static_cast<uint32>(Piece.Indices.Num());
        Surface.StartIndex    = 0;
        Surface.MaterialIndex = 0;
        Resource->GeometrySurfaces.Add(Surface);

        CStaticMesh* Mesh = NewObject<CStaticMesh>(CPackage::GetTransientPackage(), DebugName);
        Mesh->Materials = Materials;
        if (Mesh->Materials.IsEmpty())
        {
            Mesh->Materials.SetNum(1);
        }
        Mesh->SetMeshResource(Move(Resource));
        return Mesh;
    }

    void CGeometryCollection::Serialize(FArchive& Ar)
    {
        LUMINA_MEMORY_SCOPE("Geometry Collection");
        Super::Serialize(Ar);
        Ar << Data;
    }

    void CGeometryCollection::PostLoad()
    {
        LUMINA_MEMORY_SCOPE("Geometry Collection");
        Super::PostLoad();
        BuildPieceMeshes();
    }

    void CGeometryCollection::BuildPieceMeshes()
    {
        if (!PieceMeshes.IsEmpty() || Data.Pieces.IsEmpty())
        {
            return;
        }

        // Matches the runtime fracture's choice, the baked collection materials else the source mesh's.
        const TVector<TStrongObjectPtr<CMaterialInterface>>& Mats =
            (!Materials.IsEmpty() || SourceMesh.Get() == nullptr) ? Materials : SourceMesh->Materials;

        PieceMeshes.Reserve(Data.Pieces.Num());
        for (const FFracturePiece& Piece : Data.Pieces)
        {
            PieceMeshes.Add(Fracture::BuildPieceMesh(Piece, Mats, "GCPiece"));
        }
    }

    const TVector<TStrongObjectPtr<CStaticMesh>>& CGeometryCollection::GetPieceMeshes()
    {
        BuildPieceMeshes();
        return PieceMeshes;
    }

    int32 CGeometryCollection::Rebuild()
    {
        PieceMeshes.Reset();   // invalidate the shared mesh cache; GetPieceMeshes() rebuilds on demand

        CStaticMesh* Source = SourceMesh.Get();
        if (Source == nullptr)
        {
            Data.Pieces.Reset();
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
