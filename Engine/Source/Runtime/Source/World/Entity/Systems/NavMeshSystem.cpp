#include "RuntimePCH.h"
#include "NavMeshSystem.h"

#include "AI/Navigation/NavMesh.h"
#include "AI/Navigation/NavMeshBuilder.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Core/Console/ConsoleVariable.h"
#include "Renderer/MeshData.h"
#include "Renderer/Vertex.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/NavMeshComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/World.h"
#include "World/WorldTypes.h"
#include "Log/Log.h"

namespace Lumina
{
    FSystemAccess SNavMeshSystem::Access = FSystemAccess{}
        .Write<SNavMeshComponent>()
        .Read<SBoxColliderComponent, SSphereColliderComponent, SMeshColliderComponent,
              SCapsuleColliderComponent, SCylinderColliderComponent, SCharacterPhysicsComponent,
              STerrainColliderComponent, STerrainComponent, STransformComponent, SStaticMeshComponent>();

    // NOLINTBEGIN(bugprone-throwing-static-initialization)

    // Master toggle. When false, no nav debug draws at all (sub-CVars are ignored).
    static TConsoleVar<bool>  CVarNavDrawDebug      ("Nav.DrawDebug",          false, "Master toggle for navmesh debug visualization.");

    // Per-layer toggles. Defaults aim for a useful "first look" when the master is flipped on.
    static TConsoleVar<bool>  CVarNavDebugSurface   ("Nav.Debug.Surface",      true,  "Translucent filled walkable surface.");
    static TConsoleVar<float> CVarNavDebugSurfAlpha ("Nav.Debug.SurfaceAlpha", 0.35f, "Opacity of the filled navmesh surface.");
    static TConsoleVar<bool>  CVarNavDebugEdges     ("Nav.Debug.Edges",        true,  "Draw poly boundary edges (thick).");
    static TConsoleVar<bool>  CVarNavDebugTriEdges  ("Nav.Debug.TriEdges",     false, "Draw interior detail-triangle edges (faint).");
    static TConsoleVar<bool>  CVarNavDebugColorArea ("Nav.Debug.ColorByArea",  true,  "Color edges/centers by ENavArea (ground/water/door/danger).");
    static TConsoleVar<bool>  CVarNavDebugVerts     ("Nav.Debug.Vertices",     false, "Sphere at every poly boundary vertex (intersections).");
    static TConsoleVar<bool>  CVarNavDebugCenters   ("Nav.Debug.Centers",      false, "Small sphere at every walkable triangle center.");
    static TConsoleVar<bool>  CVarNavDebugTiles     ("Nav.Debug.TileBounds",   false, "Wireframe box for each loaded nav tile.");
    static TConsoleVar<bool>  CVarNavDebugBounds    ("Nav.Debug.BakeBounds",   true,  "Wireframe box for the bake volume (Center +/- Extents).");
    static TConsoleVar<bool>  CVarNavDebugLinks     ("Nav.Debug.OffMeshLinks", true,  "Arrows for off-mesh connections.");
    static TConsoleVar<bool>  CVarNavDebugLog       ("Nav.Debug.LogStats",     false, "Log triangle/edge/tile counts on every cache refresh.");
    static TConsoleVar<float> CVarNavDebugLift      ("Nav.Debug.LiftY",        0.05f, "Vertical offset added to debug geometry to avoid Z-fighting.");
    static TConsoleVar<float> CVarNavDebugVertSize  ("Nav.Debug.VertexRadius", 0.08f, "Radius of vertex spheres (also drives center-sphere size).");

    // NOLINTEND(bugprone-throwing-static-initialization)

    namespace
    {
        // Per-area colors for at-a-glance area classification. Alpha currently unused by the line batcher,
        // but kept consistent so future translucent rendering doesn't need a second table.
        FORCEINLINE FVector4 NavAreaColor(uint8 Area)
        {
            switch ((ENavArea)Area)
            {
                case ENavArea::Ground: return FVector4(0.20f, 0.85f, 0.30f, 1.0f);
                case ENavArea::Water:  return FVector4(0.20f, 0.45f, 0.95f, 1.0f);
                case ENavArea::Door:   return FVector4(0.95f, 0.65f, 0.15f, 1.0f);
                case ENavArea::Danger: return FVector4(0.95f, 0.20f, 0.20f, 1.0f);
                case ENavArea::Null:   return FVector4(0.40f, 0.40f, 0.40f, 1.0f);
                default:               return FVector4(0.75f, 0.30f, 0.95f, 1.0f);
            }
        }

        void DrawNavDebug(const FSystemContext& Context, const SNavMeshComponent& Comp)
        {
            const FNavMesh& Mesh = *Comp.Runtime.Mesh;
            const FVector3 Lift(0.0f, CVarNavDebugLift.GetValue(), 0.0f);
            const bool bColorByArea = CVarNavDebugColorArea.GetValue();

            // Translucent filled walkable surface: the headline "is this walkable" read.
            if (CVarNavDebugSurface.GetValue())
            {
                const float Alpha = CVarNavDebugSurfAlpha.GetValue();
                const FNavDebugStats Stats = Mesh.GetDebugStats();
                TVector<FSimpleElementVertex> SurfaceVerts;
                SurfaceVerts.reserve((size_t)Stats.Triangles * 3);
                Mesh.ForEachTriangle([&](const FVector3& A, const FVector3& B, const FVector3& C, uint8 Area)
                {
                    FVector4 Color = bColorByArea ? NavAreaColor(Area) : FVector4(0.15f, 0.85f, 0.35f, 1.0f);
                    Color.w = Alpha;
                    const uint32 Packed = PackColor(Color);
                    SurfaceVerts.push_back({ A + Lift, Packed });
                    SurfaceVerts.push_back({ B + Lift, Packed });
                    SurfaceVerts.push_back({ C + Lift, Packed });
                });
                if (!SurfaceVerts.empty())
                {
                    Context.DrawDebugSolidTriangles(std::move(SurfaceVerts), true, -1.0f);
                }
            }

            if (CVarNavDebugBounds.GetValue())
            {
                Context.DrawDebugBox(Comp.Center, Comp.GetWorldExtents(), FQuat(1.0f, 0.0f, 0.0f, 0.0f),
                    FVector4(1.0f, 0.85f, 0.10f, 1.0f), 4.0f, -1.0f);
            }

            if (CVarNavDebugTiles.GetValue())
            {
                const FVector4 TileColor(0.20f, 0.65f, 1.0f, 1.0f);
                Mesh.ForEachLoadedTile([&Context, TileColor](const FNavTileBounds& T)
                {
                    const FVector3 Center = (T.Min + T.Max) * 0.5f;
                    const FVector3 Half   = (T.Max - T.Min) * 0.5f;
                    Context.DrawDebugBox(Center, Half, FQuat(1.0f, 0.0f, 0.0f, 0.0f), TileColor, 4.0f, -1.0f);
                });
            }

            // Faint interior triangle edges (off by default; the fill already conveys the surface).
            if (CVarNavDebugTriEdges.GetValue())
            {
                Mesh.ParallelForEachTriangle([&Context, &Lift, bColorByArea](const FVector3& A, const FVector3& B, const FVector3& C, uint8 Area)
                {
                    FVector4 Color = bColorByArea ? NavAreaColor(Area) : FVector4(0.05f, 1.0f, 0.15f, 1.0f);
                    Color *= FVector4(0.6f, 0.6f, 0.6f, 1.0f);
                    constexpr float Thickness = 1.0f;
                    Context.DrawDebugLine(A + Lift, B + Lift, Color, Thickness, -1.0f);
                    Context.DrawDebugLine(B + Lift, C + Lift, Color, Thickness, -1.0f);
                    Context.DrawDebugLine(C + Lift, A + Lift, Color, Thickness, -1.0f);
                });
            }

            // Poly perimeter: thick + bright so the nav polygon shape stands out over the fill.
            if (CVarNavDebugEdges.GetValue())
            {
                Mesh.ForEachBoundaryEdge([&Context, &Lift, bColorByArea](const FVector3& A, const FVector3& B, uint8 Area)
                {
                    const FVector4 Color = bColorByArea ? NavAreaColor(Area) : FVector4(0.0f, 0.95f, 1.0f, 1.0f);
                    Context.DrawDebugLine(A + Lift, B + Lift, Color, 3.0f, -1.0f);
                });
            }

            if (CVarNavDebugVerts.GetValue())
            {
                const float R = CVarNavDebugVertSize.GetValue();
                const FVector4 VColor(1.0f, 0.85f, 0.0f, 1.0f);
                // Boundary endpoints == poly vertices == the "intersections" users want to see.
                Mesh.ForEachBoundaryEdge([&Context, R, VColor, &Lift](const FVector3& A, const FVector3& B, uint8)
                {
                    Context.DrawDebugSphere(A + Lift, R, VColor, 8, 4.0f, -1.0f);
                    Context.DrawDebugSphere(B + Lift, R, VColor, 8, 4.0f, -1.0f);
                });
            }

            if (CVarNavDebugCenters.GetValue())
            {
                const float R = std::max(0.02f, CVarNavDebugVertSize.GetValue() * 0.5f);
                Mesh.ParallelForEachTriangle([&Context, R, &Lift, bColorByArea](const FVector3& A, const FVector3& B, const FVector3& C, uint8 Area)
                {
                    const FVector3 C0 = (A + B + C) * (1.0f / 3.0f) + Lift;
                    const FVector4 Color = bColorByArea ? NavAreaColor(Area) : FVector4(1.0f, 1.0f, 1.0f, 1.0f);
                    Context.DrawDebugSphere(C0, R, Color, 6, 4.0f, -1.0f);
                });
            }

            if (CVarNavDebugLinks.GetValue())
            {
                const FVector4 LinkColor(1.0f, 0.30f, 0.95f, 1.0f);
                Mesh.ForEachOffMeshLink([&Context, LinkColor, &Lift](const FVector3& A, const FVector3& B)
                {
                    const FVector3 Dir = B - A;
                    const float Len = Math::Length(Dir);
                    if (Len > 1e-4f)
                    {
                        Context.DrawDebugArrow(A + Lift, Dir / Len, Len, LinkColor, 2.5f, -1.0f, 0.25f);
                    }
                    Context.DrawDebugSphere(A + Lift, 0.15f, LinkColor, 10, 4.0f, -1.0f);
                    Context.DrawDebugSphere(B + Lift, 0.15f, LinkColor, 10, 4.0f, -1.0f);
                });
            }
        }
    }

    namespace
    {
        struct FGatherAccumulator
        {
            TVector<FVector3> Vertices;
            TVector<uint32>    Indices;
            FVector3          AABBMin = FVector3( FLT_MAX);
            FVector3          AABBMax = FVector3(-FLT_MAX);
        };

        // 8-bit local indices packed 3 per uint32; vertex indices local to Meshlet.VertexOffset.
        FORCEINLINE void UnpackTri(uint32 Packed, uint32& A, uint32& B, uint32& C)
        {
            A = (Packed >> 0)  & 0xFFu;
            B = (Packed >> 8)  & 0xFFu;
            C = (Packed >> 16) & 0xFFu;
        }


        bool TriIntersectsAABB(const FVector3& A, const FVector3& B, const FVector3& C, const FVector3& BMin, const FVector3& BMax)
        {
            const FVector3 TMin = Math::Min(Math::Min(A, B), C);
            const FVector3 TMax = Math::Max(Math::Max(A, B), C);
            return !(TMax.x < BMin.x || TMin.x > BMax.x ||
                     TMax.y < BMin.y || TMin.y > BMax.y ||
                     TMax.z < BMin.z || TMin.z > BMax.z);
        }

        // Tag-bit packed into cache key so one entity may track one collider of each type.
        enum class ENavColliderType : uint8 { Box = 0, Sphere = 1, Mesh = 2, CharacterCapsule = 3, Capsule = 4, Cylinder = 5, Terrain = 6 };

        FORCEINLINE uint64 PackSourceKey(entt::entity E, ENavColliderType T)
        {
            return ((uint64)(uint32)E << 8) | (uint64)T;
        }

        // Matches Jolt body placement so nav geometry overlaps physics exactly.
        FORCEINLINE FMatrix4 ColliderToWorld(const STransformComponent& X, const FVector3& TransOffset, const FVector3& EulerOffset)
        {
            const FMatrix4 LocalOffset = Math::Translate(FMatrix4(1.0f), TransOffset)
                                        * Math::ToMatrix4(FQuat(EulerOffset));
            return X.GetWorldMatrix() * LocalOffset;
        }

        FORCEINLINE void EmitTri(FGatherAccumulator& Acc, const FVector3& BakeMin, const FVector3& BakeMax, const FVector3& A, const FVector3& B, const FVector3& C)
        {
            if (!TriIntersectsAABB(A, B, C, BakeMin, BakeMax))
            {
                return;
            }
            const uint32 Base = (uint32)Acc.Vertices.size();
            Acc.Vertices.push_back(A);
            Acc.Vertices.push_back(B);
            Acc.Vertices.push_back(C);
            Acc.Indices.push_back(Base + 0);
            Acc.Indices.push_back(Base + 1);
            Acc.Indices.push_back(Base + 2);
            Acc.AABBMin = Math::Min(Acc.AABBMin, Math::Min(A, Math::Min(B, C)));
            Acc.AABBMax = Math::Max(Acc.AABBMax, Math::Max(A, Math::Max(B, C)));
        }

        // 12 tris (2 per face); world matrix precomputed by caller.
        void EmitBoxGeometry(const FMatrix4& W, const FVector3& HalfExtent, const FVector3& BakeMin, const FVector3& BakeMax, FGatherAccumulator& Acc)
        {
            const FVector3 H = HalfExtent;
            const FVector3 LocalCorners[8] = {
                {-H.x,-H.y,-H.z}, { H.x,-H.y,-H.z},
                { H.x,-H.y, H.z}, {-H.x,-H.y, H.z},
                {-H.x, H.y,-H.z}, { H.x, H.y,-H.z},
                { H.x, H.y, H.z}, {-H.x, H.y, H.z},
            };
            FVector3 V[8];
            for (int i = 0; i < 8; ++i)
            {
                V[i] = FVector3(W * FVector4(LocalCorners[i], 1.0f));
            }
            // Outward-wound for Recast's slope test (top face = walkable).
            EmitTri(Acc, BakeMin, BakeMax, V[0], V[1], V[2]); EmitTri(Acc, BakeMin, BakeMax, V[0], V[2], V[3]);
            EmitTri(Acc, BakeMin, BakeMax, V[4], V[6], V[5]); EmitTri(Acc, BakeMin, BakeMax, V[4], V[7], V[6]);
            EmitTri(Acc, BakeMin, BakeMax, V[0], V[5], V[1]); EmitTri(Acc, BakeMin, BakeMax, V[0], V[4], V[5]);
            EmitTri(Acc, BakeMin, BakeMax, V[3], V[2], V[6]); EmitTri(Acc, BakeMin, BakeMax, V[3], V[6], V[7]);
            EmitTri(Acc, BakeMin, BakeMax, V[0], V[7], V[4]); EmitTri(Acc, BakeMin, BakeMax, V[0], V[3], V[7]);
            EmitTri(Acc, BakeMin, BakeMax, V[1], V[6], V[2]); EmitTri(Acc, BakeMin, BakeMax, V[1], V[5], V[6]);
        }

        // Low-poly UV-sphere (12x8); nav doesn't need detail past tile resolution.
        void EmitSphereGeometry(const FMatrix4& W, float Radius, const FVector3& BakeMin, const FVector3& BakeMax, FGatherAccumulator& Acc)
        {
            constexpr int Segments = 12;
            constexpr int Stacks   = 8;
            FVector3 Verts[(Stacks + 1) * (Segments + 1)];
            for (int s = 0; s <= Stacks; ++s)
            {
                const float Phi = LE_PI_F * (float)s / (float)Stacks;
                const float SinP = std::sin(Phi);
                const float CosP = std::cos(Phi);
                for (int g = 0; g <= Segments; ++g)
                {
                    const float Theta = (2.0f * LE_PI_F) * (float)g / (float)Segments;
                    const FVector3 Local(Radius * SinP * std::cos(Theta), Radius * CosP, Radius * SinP * std::sin(Theta));
                    Verts[s * (Segments + 1) + g] = FVector3(W * FVector4(Local, 1.0f));
                }
            }
            for (int s = 0; s < Stacks; ++s)
            {
                for (int g = 0; g < Segments; ++g)
                {
                    const FVector3& A = Verts[(s + 0) * (Segments + 1) + g + 0];
                    const FVector3& B = Verts[(s + 1) * (Segments + 1) + g + 0];
                    const FVector3& C = Verts[(s + 1) * (Segments + 1) + g + 1];
                    const FVector3& D = Verts[(s + 0) * (Segments + 1) + g + 1];
                    EmitTri(Acc, BakeMin, BakeMax, A, D, C);
                    EmitTri(Acc, BakeMin, BakeMax, A, C, B);
                }
            }
        }

        // Cylinder + hemispheres along +Y (Jolt CapsuleShape). Total height = 2*HalfHeight + 2*Radius.
        void EmitCapsuleGeometry(const FMatrix4& W, float HalfHeight, float Radius, const FVector3& BakeMin, const FVector3& BakeMax, FGatherAccumulator& Acc)
        {
            constexpr int Segments = 12;
            constexpr int HemiStacks = 4;
            const int Rings = 2 * HemiStacks + 2;

            TVector<FVector3> Verts;
            Verts.resize(Rings * (Segments + 1));

            int RingIdx = 0;
            // Top hemisphere at +Y * HalfHeight.
            for (int s = 0; s <= HemiStacks; ++s, ++RingIdx)
            {
                const float Phi = (LE_PI_F * 0.5f) * (float)s / (float)HemiStacks;
                const float SinP = std::sin(Phi);
                const float CosP = std::cos(Phi);
                for (int g = 0; g <= Segments; ++g)
                {
                    const float Theta = (2.0f * LE_PI_F) * (float)g / (float)Segments;
                    const FVector3 Local(Radius * SinP * std::cos(Theta), HalfHeight + Radius * CosP, Radius * SinP * std::sin(Theta));
                    Verts[RingIdx * (Segments + 1) + g] = FVector3(W * FVector4(Local, 1.0f));
                }
            }
            // Bottom hemisphere at -Y * HalfHeight.
            for (int s = 1; s <= HemiStacks + 1; ++s, ++RingIdx)
            {
                const float Phi = (LE_PI_F * 0.5f) + (LE_PI_F * 0.5f) * (float)s / (float)(HemiStacks + 1);
                const float SinP = std::sin(Phi);
                const float CosP = std::cos(Phi);
                for (int g = 0; g <= Segments; ++g)
                {
                    const float Theta = (2.0f * LE_PI_F) * (float)g / (float)Segments;
                    const FVector3 Local(Radius * SinP * std::cos(Theta), -HalfHeight + Radius * CosP, Radius * SinP * std::sin(Theta));
                    Verts[RingIdx * (Segments + 1) + g] = FVector3(W * FVector4(Local, 1.0f));
                }
            }

            for (int s = 0; s < Rings - 1; ++s)
            {
                for (int g = 0; g < Segments; ++g)
                {
                    const FVector3& A = Verts[(s + 0) * (Segments + 1) + g + 0];
                    const FVector3& B = Verts[(s + 1) * (Segments + 1) + g + 0];
                    const FVector3& C = Verts[(s + 1) * (Segments + 1) + g + 1];
                    const FVector3& D = Verts[(s + 0) * (Segments + 1) + g + 1];
                    EmitTri(Acc, BakeMin, BakeMax, A, D, C);
                    EmitTri(Acc, BakeMin, BakeMax, A, C, B);
                }
            }
        }

        // Flat-capped cylinder along +Y (Jolt CylinderShape). CapRadius rounding is ignored for nav.
        void EmitCylinderGeometry(const FMatrix4& W, float HalfHeight, float Radius, const FVector3& BakeMin, const FVector3& BakeMax, FGatherAccumulator& Acc)
        {
            constexpr int Segments = 16;
            FVector3 Top[Segments + 1];
            FVector3 Bot[Segments + 1];
            for (int i = 0; i <= Segments; ++i)
            {
                const float Theta = (2.0f * LE_PI_F) * (float)i / (float)Segments;
                const float C = std::cos(Theta);
                const float S = std::sin(Theta);
                Top[i] = FVector3(W * FVector4(Radius * C,  HalfHeight, Radius * S, 1.0f));
                Bot[i] = FVector3(W * FVector4(Radius * C, -HalfHeight, Radius * S, 1.0f));
            }
            const FVector3 TopC = FVector3(W * FVector4(0.0f,  HalfHeight, 0.0f, 1.0f));
            const FVector3 BotC = FVector3(W * FVector4(0.0f, -HalfHeight, 0.0f, 1.0f));
            for (int i = 0; i < Segments; ++i)
            {
                EmitTri(Acc, BakeMin, BakeMax, Top[i], Bot[i], Bot[i + 1]);
                EmitTri(Acc, BakeMin, BakeMax, Top[i], Bot[i + 1], Top[i + 1]);
                EmitTri(Acc, BakeMin, BakeMax, TopC, Top[i + 1], Top[i]);
                EmitTri(Acc, BakeMin, BakeMax, BotC, Bot[i], Bot[i + 1]);
            }
        }

        // Explicit Mesh wins; falls back to StaticMeshComponent. Mirrors Jolt's resolution.
        CStaticMesh* ResolveMeshColliderAsset(const SMeshColliderComponent& MC, const SStaticMeshComponent* Fallback)
        {
            if (CStaticMesh* M = MC.Mesh.Get())
            {
                return M;
            }
            return Fallback ? Fallback->StaticMesh.Get() : nullptr;
        }

        void EmitMeshGeometry(const FMatrix4& W, CStaticMesh* Mesh, const FVector3& BakeMin, const FVector3& BakeMax, FGatherAccumulator& Acc)
        {
            if (!Mesh) return;
            const FMeshResource& Res = Mesh->GetMeshResource();
            const FMeshletData&  Md  = Res.MeshletData;
            if (Md.IsEmpty() || Res.bSkinnedMesh) return;

            const TVector<FMeshletVertex>& MV = Md.MeshletVertices;
            const TVector<uint32>&         MT = Md.MeshletTriangles;

            for (const FGeometrySurface& Surface : Res.GeometrySurfaces)
            {
                const uint32 First = Surface.LODMeshletOffset[0];
                const uint32 Count = Surface.LODMeshletCount[0];
                for (uint32 m = 0; m < Count; ++m)
                {
                    const FMeshlet& Meshlet = Md.Meshlets[First + m];

                    FVector3 LocalVerts[MESHLET_MAX_VERTICES];
                    const uint32 V0 = Meshlet.VertexOffset;
                    for (uint32 v = 0; v < Meshlet.VertexCount; ++v)
                    {
                        const FVector3 Local = MV[V0 + v].Position;
                        LocalVerts[v] = FVector3(W * FVector4(Local, 1.0f));
                    }

                    const uint32 T0 = Meshlet.TriangleOffset;
                    for (uint32 t = 0; t < Meshlet.TriangleCount; ++t)
                    {
                        uint32 A, B, C;
                        UnpackTri(MT[T0 + t], A, B, C);
                        EmitTri(Acc, BakeMin, BakeMax, LocalVerts[A], LocalVerts[B], LocalVerts[C]);
                    }
                }
            }
        }

        // Walk the heightfield grid (matching BuildTerrainHeightFieldShape's local layout), transform to
        // world, keep only tris overlapping the bake bounds. Subsampled so a quad ~= one cell.
        void TessellateTerrain(const STerrainComponent& T, const FMatrix4& W, const FVector3& BakeMin, const FVector3& BakeMax, float CellSize, TVector<FVector3>& Out)
        {
            const int32 Res = T.Resolution;
            if (Res < 2 || (int64)T.Heightmap.size() < (int64)Res * (int64)Res)
            {
                return;
            }

            const float Stride = T.TileWorldSize / float(Res - 1);
            const float Half   = T.TileWorldSize * 0.5f;

            // One nav cell per quad is the finest worth emitting; also cap the grid so huge terrains stay bounded.
            int32 Step = std::max(1, (int32)std::floor(std::max(CellSize, 0.01f) / std::max(Stride, 1e-4f)));
            Step = std::max(Step, (int32)std::ceil((float)(Res - 1) / 256.0f));

            auto Height = [&](int32 Col, int32 Row) -> float
            {
                return T.Heightmap[(size_t)Row * (size_t)Res + (size_t)Col] * T.MaxHeight;
            };
            auto Pos = [&](int32 Col, int32 Row) -> FVector3
            {
                const FVector3 Local(-Half + (float)Col * Stride, Height(Col, Row), -Half + (float)Row * Stride);
                return FVector3(W * FVector4(Local, 1.0f));
            };

            for (int32 Row = 0; Row < Res - 1; Row += Step)
            {
                const int32 RowN = std::min(Row + Step, Res - 1);
                for (int32 Col = 0; Col < Res - 1; Col += Step)
                {
                    const int32 ColN = std::min(Col + Step, Res - 1);
                    const FVector3 A = Pos(Col,  Row);
                    const FVector3 B = Pos(ColN, Row);
                    const FVector3 C = Pos(ColN, RowN);
                    const FVector3 D = Pos(Col,  RowN);
                    // Wound so the surface normal points +Y (Recast's slope test marks it walkable).
                    if (TriIntersectsAABB(A, C, B, BakeMin, BakeMax))
                    {
                        Out.push_back(A); Out.push_back(C); Out.push_back(B);
                    }
                    if (TriIntersectsAABB(A, D, C, BakeMin, BakeMax))
                    {
                        Out.push_back(A); Out.push_back(D); Out.push_back(C);
                    }
                }
            }
        }

        // POD shape snapshot; safe to ship to a bake worker (Mesh ptr / terrain tri-soup are read-only there).
        struct FNavSourcePrim
        {
            ENavColliderType                Type  = ENavColliderType::Box;
            FMatrix4                        World = FMatrix4(1.0f);
            FVector3                        Shape = FVector3(0.0f);   // Box: half-extent; Sphere: x=Radius; Capsule/Cylinder: x=Radius, y=HalfHeight
            CStaticMesh*                    Mesh  = nullptr;
            TSharedPtr<TVector<FVector3>>   TerrainTris;              // world-space tri soup (groups of 3); terrain only
        };

        struct FNavSourceEntry
        {
            uint64          Key = 0;
            FNavSourcePrim  Prim;
            FVector3        AABBMin = FVector3( FLT_MAX);
            FVector3        AABBMax = FVector3(-FLT_MAX);
        };

        void EmitNavSourcePrim(const FNavSourcePrim& P, const FVector3& BakeMin, const FVector3& BakeMax, FGatherAccumulator& Acc)
        {
            switch (P.Type)
            {
                case ENavColliderType::Box:    EmitBoxGeometry(P.World, P.Shape, BakeMin, BakeMax, Acc); break;
                case ENavColliderType::Sphere: EmitSphereGeometry(P.World, P.Shape.x, BakeMin, BakeMax, Acc); break;
                case ENavColliderType::Mesh:   EmitMeshGeometry(P.World, P.Mesh, BakeMin, BakeMax, Acc); break;
                case ENavColliderType::Capsule:
                case ENavColliderType::CharacterCapsule: EmitCapsuleGeometry(P.World, P.Shape.y, P.Shape.x, BakeMin, BakeMax, Acc); break;
                case ENavColliderType::Cylinder: EmitCylinderGeometry(P.World, P.Shape.y, P.Shape.x, BakeMin, BakeMax, Acc); break;
                case ENavColliderType::Terrain:
                    if (P.TerrainTris)
                    {
                        const TVector<FVector3>& Tris = *P.TerrainTris;
                        for (size_t i = 0; i + 2 < Tris.size(); i += 3)
                        {
                            EmitTri(Acc, BakeMin, BakeMax, Tris[i], Tris[i + 1], Tris[i + 2]);
                        }
                    }
                    break;
            }
        }

        // Conservative world AABBs. Change detector and cache rebuild MUST produce byte-identical values,
        // so all four consumers share this single traversal.
        void CollectNavSources(const FSystemContext& Context, const FVector3& BakeMin, const FVector3& BakeMax, bool bTessellateTerrain, float CellSize, TVector<FNavSourceEntry>& Out)
        {
            auto CornersAABB = [](const FMatrix4& W, const FVector3* Local, int32 N, FVector3& Mn, FVector3& Mx)
            {
                Mn = FVector3( FLT_MAX);
                Mx = FVector3(-FLT_MAX);
                for (int32 i = 0; i < N; ++i)
                {
                    const FVector3 Pt = FVector3(W * FVector4(Local[i], 1.0f));
                    Mn = Math::Min(Mn, Pt);
                    Mx = Math::Max(Mx, Pt);
                }
            };
            auto ScaledRadius = [](const FMatrix4& W, float Radius) -> float
            {
                const float Sx = Math::Length(FVector3(W[0]));
                const float Sy = Math::Length(FVector3(W[1]));
                const float Sz = Math::Length(FVector3(W[2]));
                return Radius * std::max(Sx, std::max(Sy, Sz));
            };

            auto BoxView = Context.CreateView<SBoxColliderComponent, STransformComponent>();
            for (entt::entity E : BoxView)
            {
                SBoxColliderComponent& Box = BoxView.get<SBoxColliderComponent>(E);
                if (!Box.bAffectsNavigation) continue;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Box);
                Entry.Prim.Type = ENavColliderType::Box;
                Entry.Prim.World = ColliderToWorld(BoxView.get<STransformComponent>(E), Box.TranslationOffset, Box.RotationOffset);
                Entry.Prim.Shape = Box.HalfExtent;
                const FVector3 H = Box.HalfExtent;
                const FVector3 Corners[8] = {
                    {-H.x,-H.y,-H.z}, { H.x,-H.y,-H.z}, { H.x,-H.y, H.z}, {-H.x,-H.y, H.z},
                    {-H.x, H.y,-H.z}, { H.x, H.y,-H.z}, { H.x, H.y, H.z}, {-H.x, H.y, H.z},
                };
                CornersAABB(Entry.Prim.World, Corners, 8, Entry.AABBMin, Entry.AABBMax);
                Out.push_back(std::move(Entry));
            }

            auto SphereView = Context.CreateView<SSphereColliderComponent, STransformComponent>();
            for (entt::entity E : SphereView)
            {
                SSphereColliderComponent& Sphere = SphereView.get<SSphereColliderComponent>(E);
                if (!Sphere.bAffectsNavigation) continue;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Sphere);
                Entry.Prim.Type = ENavColliderType::Sphere;
                Entry.Prim.World = ColliderToWorld(SphereView.get<STransformComponent>(E), Sphere.TranslationOffset, FVector3(0.0f));
                Entry.Prim.Shape = FVector3(Sphere.Radius, 0.0f, 0.0f);
                const FVector3 Center = FVector3(Entry.Prim.World * FVector4(0.0f, 0.0f, 0.0f, 1.0f));
                const float R = ScaledRadius(Entry.Prim.World, Sphere.Radius);
                Entry.AABBMin = Center - FVector3(R);
                Entry.AABBMax = Center + FVector3(R);
                Out.push_back(std::move(Entry));
            }

            auto MeshView = Context.CreateView<SMeshColliderComponent, STransformComponent>();
            for (entt::entity E : MeshView)
            {
                SMeshColliderComponent& MC = MeshView.get<SMeshColliderComponent>(E);
                if (!MC.bAffectsNavigation) continue;
                const SStaticMeshComponent* Fallback = Context.GetRegistry().try_get<SStaticMeshComponent>(E);
                CStaticMesh* Mesh = ResolveMeshColliderAsset(MC, Fallback);
                if (!Mesh || Mesh->GetMeshResource().bSkinnedMesh) continue;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Mesh);
                Entry.Prim.Type = ENavColliderType::Mesh;
                Entry.Prim.World = ColliderToWorld(MeshView.get<STransformComponent>(E), MC.TranslationOffset, MC.RotationOffset);
                Entry.Prim.Mesh = Mesh;
                const FAABB& Local = Mesh->GetAABB();
                const FVector3 Corners[8] = {
                    {Local.Min.x, Local.Min.y, Local.Min.z}, {Local.Max.x, Local.Min.y, Local.Min.z},
                    {Local.Min.x, Local.Max.y, Local.Min.z}, {Local.Max.x, Local.Max.y, Local.Min.z},
                    {Local.Min.x, Local.Min.y, Local.Max.z}, {Local.Max.x, Local.Min.y, Local.Max.z},
                    {Local.Min.x, Local.Max.y, Local.Max.z}, {Local.Max.x, Local.Max.y, Local.Max.z},
                };
                CornersAABB(Entry.Prim.World, Corners, 8, Entry.AABBMin, Entry.AABBMax);
                Out.push_back(std::move(Entry));
            }

            auto CapsuleView = Context.CreateView<SCapsuleColliderComponent, STransformComponent>();
            for (entt::entity E : CapsuleView)
            {
                SCapsuleColliderComponent& Cap = CapsuleView.get<SCapsuleColliderComponent>(E);
                if (!Cap.bAffectsNavigation) continue;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Capsule);
                Entry.Prim.Type = ENavColliderType::Capsule;
                Entry.Prim.World = ColliderToWorld(CapsuleView.get<STransformComponent>(E), Cap.TranslationOffset, Cap.RotationOffset);
                Entry.Prim.Shape = FVector3(Cap.Radius, Cap.HalfHeight, 0.0f);
                const FVector3 Top = FVector3(Entry.Prim.World * FVector4(0.0f,  Cap.HalfHeight, 0.0f, 1.0f));
                const FVector3 Bot = FVector3(Entry.Prim.World * FVector4(0.0f, -Cap.HalfHeight, 0.0f, 1.0f));
                const float R = ScaledRadius(Entry.Prim.World, Cap.Radius);
                Entry.AABBMin = Math::Min(Top, Bot) - FVector3(R);
                Entry.AABBMax = Math::Max(Top, Bot) + FVector3(R);
                Out.push_back(std::move(Entry));
            }

            auto CylinderView = Context.CreateView<SCylinderColliderComponent, STransformComponent>();
            for (entt::entity E : CylinderView)
            {
                SCylinderColliderComponent& Cyl = CylinderView.get<SCylinderColliderComponent>(E);
                if (!Cyl.bAffectsNavigation) continue;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Cylinder);
                Entry.Prim.Type = ENavColliderType::Cylinder;
                Entry.Prim.World = ColliderToWorld(CylinderView.get<STransformComponent>(E), Cyl.TranslationOffset, Cyl.RotationOffset);
                Entry.Prim.Shape = FVector3(Cyl.Radius, Cyl.HalfHeight, 0.0f);
                const FVector3 Top = FVector3(Entry.Prim.World * FVector4(0.0f,  Cyl.HalfHeight, 0.0f, 1.0f));
                const FVector3 Bot = FVector3(Entry.Prim.World * FVector4(0.0f, -Cyl.HalfHeight, 0.0f, 1.0f));
                const float R = ScaledRadius(Entry.Prim.World, Cyl.Radius);
                Entry.AABBMin = Math::Min(Top, Bot) - FVector3(R);
                Entry.AABBMax = Math::Max(Top, Bot) + FVector3(R);
                Out.push_back(std::move(Entry));
            }

            // Character capsule: opt-in (default off) since agents shouldn't normally carve the navmesh.
            auto CharView = Context.CreateView<SCharacterPhysicsComponent, STransformComponent>();
            for (entt::entity E : CharView)
            {
                SCharacterPhysicsComponent& Cap = CharView.get<SCharacterPhysicsComponent>(E);
                if (!Cap.bAffectsNavigation) continue;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::CharacterCapsule);
                Entry.Prim.Type = ENavColliderType::CharacterCapsule;
                Entry.Prim.World = CharView.get<STransformComponent>(E).GetWorldMatrix();
                Entry.Prim.Shape = FVector3(Cap.Radius, Cap.HalfHeight, 0.0f);
                const FVector3 Top = FVector3(Entry.Prim.World * FVector4(0.0f,  Cap.HalfHeight, 0.0f, 1.0f));
                const FVector3 Bot = FVector3(Entry.Prim.World * FVector4(0.0f, -Cap.HalfHeight, 0.0f, 1.0f));
                const float R = ScaledRadius(Entry.Prim.World, Cap.Radius);
                Entry.AABBMin = Math::Min(Top, Bot) - FVector3(R);
                Entry.AABBMax = Math::Max(Top, Bot) + FVector3(R);
                Out.push_back(std::move(Entry));
            }

            // Terrain heightfield: needs both the collider (collision present) and the source component.
            auto TerrainView = Context.CreateView<STerrainColliderComponent, STransformComponent>();
            for (entt::entity E : TerrainView)
            {
                STerrainColliderComponent& TC = TerrainView.get<STerrainColliderComponent>(E);
                if (!TC.bAffectsNavigation) continue;
                const STerrainComponent* Terrain = Context.GetRegistry().try_get<STerrainComponent>(E);
                if (!Terrain || Terrain->Heightmap.empty()) continue;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Terrain);
                Entry.Prim.Type = ENavColliderType::Terrain;
                Entry.Prim.World = TerrainView.get<STransformComponent>(E).GetWorldMatrix();
                const float Half = Terrain->TileWorldSize * 0.5f;
                const FVector3 Corners[8] = {
                    {-Half, 0.0f, -Half}, { Half, 0.0f, -Half}, { Half, 0.0f,  Half}, {-Half, 0.0f,  Half},
                    {-Half, Terrain->MaxHeight, -Half}, { Half, Terrain->MaxHeight, -Half}, { Half, Terrain->MaxHeight,  Half}, {-Half, Terrain->MaxHeight,  Half},
                };
                CornersAABB(Entry.Prim.World, Corners, 8, Entry.AABBMin, Entry.AABBMax);
                if (bTessellateTerrain)
                {
                    Entry.Prim.TerrainTris = MakeShared<TVector<FVector3>>();
                    TessellateTerrain(*Terrain, Entry.Prim.World, BakeMin, BakeMax, CellSize, *Entry.Prim.TerrainTris);
                }
                Out.push_back(std::move(Entry));
            }
        }

        // Collect nav source colliders from all entities with bAffectsNavigation set.
        void GatherSourceGeometry(const FSystemContext& Context, const FVector3& BakeMin, const FVector3& BakeMax, FGatherAccumulator& Acc, float CellSize)
        {
            TVector<FNavSourceEntry> Sources;
            CollectNavSources(Context, BakeMin, BakeMax, true, CellSize, Sources);
            for (const FNavSourceEntry& Entry : Sources)
            {
                EmitNavSourcePrim(Entry.Prim, BakeMin, BakeMax, Acc);
            }
        }

        FORCEINLINE void TilesForAABB(const FVector3& AABBMin, const FVector3& AABBMax, const FVector3& Origin, float TileWorldSize, int32 TilesX, int32 TilesY,
                                      int32& OutTX0, int32& OutTY0, int32& OutTX1, int32& OutTY1)
        {
            OutTX0 = std::clamp((int32)std::floor((AABBMin.x - Origin.x) / TileWorldSize), 0, TilesX - 1);
            OutTY0 = std::clamp((int32)std::floor((AABBMin.z - Origin.z) / TileWorldSize), 0, TilesY - 1);
            OutTX1 = std::clamp((int32)std::floor((AABBMax.x - Origin.x) / TileWorldSize), 0, TilesX - 1);
            OutTY1 = std::clamp((int32)std::floor((AABBMax.z - Origin.z) / TileWorldSize), 0, TilesY - 1);
        }

        FORCEINLINE uint64 PackTileKey(int32 TX, int32 TY) { return ((uint64)(uint32)TY << 32) | (uint32)TX; }

        // Snapshot AABBs at bake completion so next change-detector tick reports zero diff. Shares the one
        // CollectNavSources traversal (no terrain tessellation: the cheap AABB path only needs bounds).
        void RebuildEntityAABBCache(const FSystemContext& Context, const FVector3& BakeMin, const FVector3& BakeMax, THashMap<uint64, FNavSourceEntity>& OutCache)
        {
            OutCache.clear();
            TVector<FNavSourceEntry> Sources;
            CollectNavSources(Context, BakeMin, BakeMax, false, 0.0f, Sources);
            for (const FNavSourceEntry& Entry : Sources)
            {
                OutCache[Entry.Key] = FNavSourceEntity{ Entry.AABBMin, Entry.AABBMax };
            }
        }

        void FillBuildInput(const FSystemContext& Context, SNavMeshComponent& Comp, FNavBuildInput& Out)
        {
            Out.Settings  = Comp.Settings;
            Out.BoundsMin = Comp.Center - Comp.GetWorldExtents();
            Out.BoundsMax = Comp.Center + Comp.GetWorldExtents();

            FGatherAccumulator Acc;
            GatherSourceGeometry(Context, Out.BoundsMin, Out.BoundsMax, Acc, Comp.Settings.CellSize);
            Out.Vertices = std::move(Acc.Vertices);
            Out.Indices  = std::move(Acc.Indices);
        }

        void TickComponent(const FSystemContext& Context, entt::entity Entity, SNavMeshComponent& Comp)
        {
            // Mirror the entity transform into the bake volume: scale always (effective extents match
            // what was baked), location editor-only (runtime keeps the serialized Center). Auto-bake picks it up.
            if (const STransformComponent* X = Context.GetRegistry().try_get<STransformComponent>(Entity))
            {
                const FTransform& WT = X->GetWorldTransform();
                const FVector3 WS = WT.GetScale();
                Comp.Runtime.WorldScale = FVector3(std::fabs(WS.x), std::fabs(WS.y), std::fabs(WS.z));
                if (Context.GetWorldType() == EWorldType::Editor)
                {
                    Comp.Center = WT.GetLocation();
                }
            }

            // Auto-bake: once the bounds/settings stop changing (placed, moved, or scaled in the editor)
            // and differ from what's baked, request a bake. Debounced so dragging doesn't bake every frame.
            if (Comp.bAutoBake)
            {
                FNavMeshRuntime& RT = Comp.Runtime;
                const FVector3 WExt = Comp.GetWorldExtents();

                // A loaded/already-baked component starts in sync: don't re-bake until something changes.
                if (!RT.bAutoBuiltValid && Comp.HasBakedData())
                {
                    RT.AutoBuiltCenter   = Comp.Center;
                    RT.AutoBuiltExtents  = WExt;
                    RT.AutoBuiltSettings = Comp.Settings;
                    RT.bAutoBuiltValid   = true;
                }

                const bool bChangedThisTick =
                    !Math::IsNearlyEqual(Comp.Center, RT.AutoPrevCenter) ||
                    !Math::IsNearlyEqual(WExt, RT.AutoPrevExtents) ||
                    std::memcmp(&Comp.Settings, &RT.AutoPrevSettings, sizeof(FNavBuildSettings)) != 0;
                RT.AutoPrevCenter   = Comp.Center;
                RT.AutoPrevExtents  = WExt;
                RT.AutoPrevSettings = Comp.Settings;

                if (bChangedThisTick)
                {
                    RT.AutoSettleTimer = 0.0f;
                }
                else
                {
                    RT.AutoSettleTimer += (float)Context.GetDeltaTime();

                    const bool bDiffersFromBuilt =
                        !RT.bAutoBuiltValid ||
                        !Math::IsNearlyEqual(Comp.Center, RT.AutoBuiltCenter) ||
                        !Math::IsNearlyEqual(WExt, RT.AutoBuiltExtents) ||
                        std::memcmp(&Comp.Settings, &RT.AutoBuiltSettings, sizeof(FNavBuildSettings)) != 0;
                    const bool bIdle = !RT.ActiveBake && !RT.PendingInit;

                    if (RT.AutoSettleTimer >= 0.25f && bDiffersFromBuilt && bIdle)
                    {
                        RT.AutoBuiltCenter   = Comp.Center;
                        RT.AutoBuiltExtents  = WExt;
                        RT.AutoBuiltSettings = Comp.Settings;
                        RT.bAutoBuiltValid   = true;
                        Comp.bBakeRequested  = true;
                    }
                }
            }

            if (Comp.bBakeRequested)
            {
                Comp.bBakeRequested = false;
                SNavMeshSystem::RequestBake(Context, Comp);
            }

            // Drain finished bake; FNavMesh construction is offloaded via PendingInit.
            if (Comp.Runtime.ActiveBake && Comp.Runtime.ActiveBake->bDone.load(std::memory_order_acquire))
            {
                FNavBuildOutput& Out = Comp.Runtime.ActiveBake->Output;

                // Tally non-empty tiles so a zero-walkable bake is loud (usually bounds miss geometry).
                int32 NonEmptyTiles = 0;
                for (const FNavTileData& T : Out.Tiles)
                {
                    if (!T.Blob.empty()) ++NonEmptyTiles;
                }

                Comp.Tiles           = std::move(Out.Tiles);
                Comp.Origin          = Out.Origin;
                Comp.TileWorldSize   = Out.TileWorldSize;
                Comp.MaxPolysPerTile = Out.MaxPolysPerTile;
                Comp.Runtime.LiveLayout.Origin = Out.Origin;
                Comp.Runtime.LiveLayout.TileWorldSize = Out.TileWorldSize;
                Comp.Runtime.LiveLayout.MaxTiles = Out.MaxTiles;
                Comp.Runtime.LiveLayout.MaxPolysPerTile = Out.MaxPolysPerTile;
                Comp.Runtime.ActiveBake.reset();
                Comp.Runtime.DirtyTiles.clear();

                if (NonEmptyTiles == 0)
                {
                    // Fail explicitly. Leaving State=Building here (hydration only kicks when HasBakedData()
                    // is true, which an empty Tiles list is not) is what read as "stuck on Baking".
                    LOG_WARN("NavMesh bake produced no walkable tiles ({} total). Check the bounds overlap source geometry, and that the volume/scale isn't so large the bake was capped.", (int32)Comp.Tiles.size());
                    Comp.Runtime.bRuntimeDirty = false;
                    Comp.Runtime.Mesh.reset();
                    Comp.Runtime.State = ENavBakeState::Failed;
                }
                else
                {
                    LOG_INFO("NavMesh bake complete: {}/{} tiles walkable, origin=({:.2f}, {:.2f}, {:.2f}), tileSize={:.2f}.",
                        NonEmptyTiles, (int32)Comp.Tiles.size(), Comp.Origin.x, Comp.Origin.y, Comp.Origin.z, Comp.TileWorldSize);
                    Comp.Runtime.bRuntimeDirty = true;
                    RebuildEntityAABBCache(Context, Comp.Center - Comp.GetWorldExtents(), Comp.Center + Comp.GetWorldExtents(), Comp.Runtime.EntityAABBs);
                }
            }

            // Drain finished async hydration.
            if (Comp.Runtime.PendingInit && Comp.Runtime.PendingInit->bDone.load(std::memory_order_acquire))
            {
                Comp.Runtime.Mesh = std::move(Comp.Runtime.PendingInit->ResultMesh);
                Comp.Runtime.PendingInit.reset();
                // dtNavMesh::init or addTile can fail; without this branch state silently goes Ready.
                if (!Comp.Runtime.Mesh || !Comp.Runtime.Mesh->IsReady())
                {
                    LOG_ERROR("NavMesh hydration failed: dtNavMesh did not initialize (recast vendoring missing, or addTile rejected every blob). All Nav queries will return false.");
                    Comp.Runtime.State = ENavBakeState::Failed;
                    Comp.Runtime.Mesh.reset();
                }
                else
                {
                    Comp.Runtime.State = ENavBakeState::Ready;

                    if (CVarNavDebugLog.GetValue())
                    {
                        const FNavDebugStats Stats = Comp.Runtime.Mesh->GetDebugStats();
                        LOG_INFO("NavMesh ready: {} tiles, {} triangles, {} boundary edges, {} off-mesh links.",
                            Stats.LoadedTiles, Stats.Triangles, Stats.BoundaryEdges, Stats.OffMeshLinks);
                    }
                }
            }

            // Kick async hydration when tiles are present and mesh is missing or dirty.
            // bRuntimeDirty branch is essential: re-bake otherwise leaves old Mesh and state stuck Building.
            if (Comp.HasBakedData() && !Comp.Runtime.PendingInit && (Comp.Runtime.bRuntimeDirty || !Comp.Runtime.Mesh))
            {
                const FVector3 BakeMin = Comp.Center - Comp.GetWorldExtents();
                const FVector3 BakeMax = Comp.Center + Comp.GetWorldExtents();
                Comp.Runtime.TilesX = std::max(1, (int32)std::ceil((BakeMax.x - BakeMin.x) / Comp.TileWorldSize));
                Comp.Runtime.TilesY = std::max(1, (int32)std::ceil((BakeMax.z - BakeMin.z) / Comp.TileWorldSize));
                Comp.Runtime.LiveLayout.Origin          = Comp.Origin;
                Comp.Runtime.LiveLayout.TileWorldSize   = Comp.TileWorldSize;
                Comp.Runtime.LiveLayout.MaxTiles        = (int32)Comp.Tiles.size();
                Comp.Runtime.LiveLayout.MaxPolysPerTile = Comp.MaxPolysPerTile;

                auto Job = MakeShared<FNavInitJob>();
                Comp.Runtime.PendingInit = Job;
                Comp.Runtime.bRuntimeDirty = false;
                Comp.Runtime.State = ENavBakeState::Initializing;

                // Copy tiles so worker owns its data; Comp.Tiles stays serialized source of truth.
                TVector<FNavTileData> TilesCopy = Comp.Tiles;
                const FVector3 InitOrigin = Comp.Origin;
                const float InitTileSize = Comp.TileWorldSize;
                const int32 InitMaxTiles = (int32)TilesCopy.size();
                const int32 InitMaxPolys = Comp.MaxPolysPerTile;

                Task::AsyncTask(1, 1, [Job, Tiles = std::move(TilesCopy), InitOrigin, InitTileSize, InitMaxTiles, InitMaxPolys](uint32, uint32, uint32) mutable
                {
                    auto Mesh = MakeUnique<FNavMesh>();
                    Mesh->Initialize(InitOrigin, InitTileSize, InitMaxTiles, InitMaxPolys, std::move(Tiles));
                    Job->ResultMesh = std::move(Mesh);
                    Job->bDone.store(true, std::memory_order_release);
                });

                RebuildEntityAABBCache(Context, BakeMin, BakeMax, Comp.Runtime.EntityAABBs);
                Comp.Runtime.DirtyTiles.clear();
            }

            if (!Comp.Runtime.Mesh || !Comp.Runtime.Mesh->IsReady() || Comp.Runtime.State != ENavBakeState::Ready)
            {
                return;
            }

            // Debug draw runs first so it emits even when later steps early-return.
            if (CVarNavDrawDebug.GetValue())
            {
                DrawNavDebug(Context, Comp);
            }

            // Hot-swap completed per-tile rebakes.
            for (auto& Job : Comp.Runtime.PendingRebakes)
            {
                if (!Job || Job->bConsumed.load(std::memory_order_acquire))
                {
                    continue;
                }
                if (!Job->bDone.load(std::memory_order_acquire))
                {
                    continue;
                }

                // Persist into Comp.Tiles BEFORE the runtime mesh hand-off, or PIE clones/saves init from empty.
                bool bUpdatedExisting = false;
                for (FNavTileData& T : Comp.Tiles)
                {
                    if (T.X == Job->TileX && T.Y == Job->TileY)
                    {
                        T.Blob = Job->ResultBlob;
                        bUpdatedExisting = true;
                        break;
                    }
                }
                if (!bUpdatedExisting)
                {
                    FNavTileData NewTile;
                    NewTile.X = Job->TileX;
                    NewTile.Y = Job->TileY;
                    NewTile.Blob = Job->ResultBlob;
                    Comp.Tiles.push_back(std::move(NewTile));
                }

                Comp.Runtime.Mesh->RebuildTile(Job->TileX, Job->TileY, std::move(Job->ResultBlob));
                Job->bConsumed.store(true, std::memory_order_release);
            }
            
            Comp.Runtime.PendingRebakes.erase(
                eastl::remove_if(Comp.Runtime.PendingRebakes.begin(), Comp.Runtime.PendingRebakes.end(),
                    [](const TSharedPtr<FNavTileRebake>& J) { return !J || J->bConsumed.load(std::memory_order_acquire); }),
                Comp.Runtime.PendingRebakes.end());

            // Detect moved/added/removed source colliders and dirty their tiles.
            THashMap<uint64, FNavSourceEntity> CurrentAABBs;
            CurrentAABBs.reserve(Comp.Runtime.EntityAABBs.size());

            auto MarkDirtyForAABB = [&](const FVector3& Mn, const FVector3& Mx)
            {
                int32 TX0, TY0, TX1, TY1;
                TilesForAABB(Mn, Mx, Comp.Origin, Comp.TileWorldSize, Comp.Runtime.TilesX, Comp.Runtime.TilesY, TX0, TY0, TX1, TY1);
                for (int32 ty = TY0; ty <= TY1; ++ty)
                {
                    for (int32 tx = TX0; tx <= TX1; ++tx)
                    {
                        Comp.Runtime.DirtyTiles.insert(PackTileKey(tx, ty));
                    }
                }
            };

            auto VisitSource = [&](uint64 Key, const FVector3& Mn, const FVector3& Mx)
            {
                CurrentAABBs[Key] = FNavSourceEntity{ Mn, Mx };
                auto It = Comp.Runtime.EntityAABBs.find(Key);
                const bool bNew   = It == Comp.Runtime.EntityAABBs.end();
                const bool bMoved = !bNew && (!Math::IsNearlyEqual(It->second.AABBMin, Mn) || !Math::IsNearlyEqual(It->second.AABBMax, Mx));
                if (bNew || bMoved)
                {
                    if (bMoved)
                    {
                        // Old footprint also dirtied so vacated tris get re-evaluated.
                        MarkDirtyForAABB(It->second.AABBMin, It->second.AABBMax);
                    }
                    MarkDirtyForAABB(Mn, Mx);
                }
            };

            TVector<FNavSourceEntry> CurrentSources;
            CollectNavSources(Context, Comp.Center - Comp.GetWorldExtents(), Comp.Center + Comp.GetWorldExtents(), false, 0.0f, CurrentSources);
            for (const FNavSourceEntry& Src : CurrentSources)
            {
                VisitSource(Src.Key, Src.AABBMin, Src.AABBMax);
            }

            // Removed colliders dirty their last-known tiles.
            for (const auto& [Id, Snap] : Comp.Runtime.EntityAABBs)
            {
                if (CurrentAABBs.find(Id) == CurrentAABBs.end())
                {
                    MarkDirtyForAABB(Snap.AABBMin, Snap.AABBMax);
                }
            }

            Comp.Runtime.EntityAABBs = std::move(CurrentAABBs);

            // Cap concurrent rebake jobs; remaining dirty tiles wait for next tick.
            constexpr uint32 MaxConcurrent = 8;
            if (Comp.Runtime.DirtyTiles.empty() || Comp.Runtime.PendingRebakes.size() >= MaxConcurrent)
            {
                return;
            }

            // Main thread only snapshots params + world matrices; tessellation runs on a worker.
            const uint32 Capacity = MaxConcurrent - (uint32)Comp.Runtime.PendingRebakes.size();
            TVector<TSharedPtr<FNavTileRebake>> BatchJobs;
            BatchJobs.reserve(Capacity);
            for (auto It = Comp.Runtime.DirtyTiles.begin(); It != Comp.Runtime.DirtyTiles.end() && BatchJobs.size() < Capacity; )
            {
                const uint64 Key = *It;
                It = Comp.Runtime.DirtyTiles.erase(It);

                auto Job = MakeShared<FNavTileRebake>();
                Job->TileX = (int32)(Key & 0xFFFFFFFFu);
                Job->TileY = (int32)(Key >> 32);
                BatchJobs.push_back(Job);
                Comp.Runtime.PendingRebakes.push_back(std::move(Job));
            }

            // Snapshot all nav sources into POD prims (terrain pre-tessellated on this thread); the worker
            // re-emits + bakes the dirty tiles. One traversal, same as gather/cache/change-detect.
            struct FInputSnapshot
            {
                TVector<FNavSourcePrim> Prims;
                FVector3                BakeMin;
                FVector3                BakeMax;
                FNavBuildSettings       Settings;
                FNavBuildOutput         Layout;
            };
            auto Snap = MakeShared<FInputSnapshot>();
            Snap->BakeMin  = Comp.Center - Comp.GetWorldExtents();
            Snap->BakeMax  = Comp.Center + Comp.GetWorldExtents();
            Snap->Settings = Comp.Settings;
            Snap->Layout   = Comp.Runtime.LiveLayout;
            {
                TVector<FNavSourceEntry> Sources;
                CollectNavSources(Context, Snap->BakeMin, Snap->BakeMax, true, Comp.Settings.CellSize, Sources);
                Snap->Prims.reserve(Sources.size());
                for (FNavSourceEntry& Entry : Sources)
                {
                    Snap->Prims.push_back(std::move(Entry.Prim));
                }
            }

            // Coordinator: emit geometry once on a worker, ParallelFor the per-tile bakes.
            Task::AsyncTask(1, 1, [Snap, Jobs = std::move(BatchJobs)](uint32, uint32, uint32) mutable
            {
                FNavBuildInput Input;
                Input.BoundsMin = Snap->BakeMin;
                Input.BoundsMax = Snap->BakeMax;
                Input.Settings  = Snap->Settings;

                FGatherAccumulator Acc;
                for (const FNavSourcePrim& Prim : Snap->Prims)
                {
                    EmitNavSourcePrim(Prim, Snap->BakeMin, Snap->BakeMax, Acc);
                }
                Input.Vertices = std::move(Acc.Vertices);
                Input.Indices  = std::move(Acc.Indices);

                Task::ParallelFor((uint32)Jobs.size(), [&](uint32 i)
                {
                    FNavTileData Out;
                    if (NavMeshBuilder::BakeSingleTile(Input, Snap->Layout, Jobs[i]->TileX, Jobs[i]->TileY, Out))
                    {
                        Jobs[i]->ResultBlob = std::move(Out.Blob);
                    }
                    Jobs[i]->bDone.store(true, std::memory_order_release);
                });
            });

            (void)Entity;
        }

        FNavMesh* FirstReadyNavMesh(const FSystemContext& Context)
        {
            auto View = Context.CreateView<SNavMeshComponent>();
            for (entt::entity Entity : View)
            {
                SNavMeshComponent& Comp = View.get<SNavMeshComponent>(Entity);
                if (Comp.Runtime.Mesh && Comp.Runtime.Mesh->IsReady())
                {
                    return Comp.Runtime.Mesh.get();
                }
            }
            return nullptr;
        }
    }

    namespace
    {
        // Initializes Center from entity transform so bake volume defaults at entity location.
        void OnNavMeshConstructed(entt::registry& Reg, entt::entity Entity)
        {
            SNavMeshComponent& Nav = Reg.get<SNavMeshComponent>(Entity);
            if (auto* Xform = Reg.try_get<STransformComponent>(Entity))
            {
                Nav.Center = Xform->GetWorldTransform().GetLocation();
            }
        }
    }

    void SNavMeshSystem::Startup(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();

        // entt sinks dedupe on the same function pointer, so reconnect is a no-op.
        Context.GetRegistry().on_construct<SNavMeshComponent>().connect<&OnNavMeshConstructed>();

        auto View = Context.CreateView<SNavMeshComponent>();
        for (entt::entity Entity : View)
        {
            TickComponent(Context, Entity, View.get<SNavMeshComponent>(Entity));
        }
    }

    void SNavMeshSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();
        auto View = Context.CreateView<SNavMeshComponent>();
        for (entt::entity Entity : View)
        {
            TickComponent(Context, Entity, View.get<SNavMeshComponent>(Entity));
        }
    }

    void SNavMeshSystem::Teardown(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();
        auto View = Context.CreateView<SNavMeshComponent>();
        for (entt::entity Entity : View)
        {
            SNavMeshComponent& Comp = View.get<SNavMeshComponent>(Entity);
            if (Comp.Runtime.ActiveBake)
            {
                Comp.Runtime.ActiveBake->bCancelRequested.store(true, std::memory_order_release);
            }
            Comp.Runtime.Mesh.reset();
            Comp.Runtime.ActiveBake.reset();
            // Worker holds its own shared_ptr; clearing here just prevents consumption.
            Comp.Runtime.PendingInit.reset();
            Comp.Runtime.PendingRebakes.clear();
            Comp.Runtime.DirtyTiles.clear();
            Comp.Runtime.EntityAABBs.clear();
            Comp.Runtime.State = ENavBakeState::Idle;
        }
    }

    void SNavMeshSystem::RequestBake(const FSystemContext& Context, SNavMeshComponent& Comp)
    {
        if (Comp.Runtime.ActiveBake)
        {
            LOG_WARN("NavMesh bake requested while one is already in flight. Ignoring (the in-progress bake will complete first).");
            return;
        }

        // Catch zero/negative bounds up front; otherwise user gets a misleading "0 walkable" warning later.
        const FVector3 Span = Comp.GetWorldExtents() * 2.0f;
        if (Span.x <= 0.0f || Span.y <= 0.0f || Span.z <= 0.0f)
        {
            LOG_ERROR("NavMesh bake skipped: bounds extents must be positive on all axes (got {:.2f}, {:.2f}, {:.2f}).",
                Comp.Extents.x, Comp.Extents.y, Comp.Extents.z);
            return;
        }
        if (Comp.Settings.CellSize <= 0.0f || Comp.Settings.CellHeight <= 0.0f || Comp.Settings.TileSizeVoxels <= 0)
        {
            LOG_ERROR("NavMesh bake skipped: invalid voxel settings (CellSize={:.3f}, CellHeight={:.3f}, TileSizeVoxels={}).",
                Comp.Settings.CellSize, Comp.Settings.CellHeight, Comp.Settings.TileSizeVoxels);
            return;
        }

        FNavBuildInput Input;
        FillBuildInput(Context, Comp, Input);

        if (Input.Vertices.empty() || Input.Indices.empty())
        {
            LOG_WARN("NavMesh bake starting with no source geometry inside bounds. The result will be an empty navmesh; check that static meshes (non-character-controller) overlap the bounds volume.");
        }
        else
        {
            LOG_INFO("NavMesh bake starting: {} verts, {} tris, bounds=({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f}).",
                (int32)Input.Vertices.size(), (int32)(Input.Indices.size() / 3),
                Input.BoundsMin.x, Input.BoundsMin.y, Input.BoundsMin.z,
                Input.BoundsMax.x, Input.BoundsMax.y, Input.BoundsMax.z);
        }

        // EntityAABB cache populated at bake-completion drain (avoids tight-vs-conservative AABB mismatch storm).
        Comp.Runtime.EntityAABBs.clear();
        Comp.Runtime.DirtyTiles.clear();
        Comp.Runtime.PendingRebakes.clear();
        Comp.Runtime.ActiveBake = NavMeshBuilder::Bake(std::move(Input));
        Comp.Runtime.State = ENavBakeState::Building;
    }

    namespace Nav
    {
        FNavMesh* GetReadyNavMesh(const FSystemContext& Context)
        {
            return FirstReadyNavMesh(Context);
        }

        bool FindPath(const FSystemContext& Context, const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, FNavPath& Out)
        {
            FNavMesh* Mesh = FirstReadyNavMesh(Context);
            return Mesh && Mesh->FindPath(Start, End, Filter, Out);
        }

        bool ProjectPoint(const FSystemContext& Context, const FVector3& World, const FVector3& Extents, const FNavQueryFilter& Filter, FVector3& Out)
        {
            FNavMesh* Mesh = FirstReadyNavMesh(Context);
            return Mesh && Mesh->ProjectPoint(World, Extents, Filter, Out);
        }

        bool Raycast(const FSystemContext& Context, const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, FVector3& HitOut)
        {
            FNavMesh* Mesh = FirstReadyNavMesh(Context);
            return Mesh && Mesh->Raycast(Start, End, Filter, HitOut);
        }

        namespace
        {
            FNavMesh* FirstReadyNavMeshFromWorld(CWorld* World)
            {
                if (!World) return nullptr;
                auto View = World->View<SNavMeshComponent>();
                for (entt::entity E : View)
                {
                    SNavMeshComponent& Comp = View.get<SNavMeshComponent>(E);
                    if (Comp.Runtime.Mesh && Comp.Runtime.Mesh->IsReady())
                    {
                        return Comp.Runtime.Mesh.get();
                    }
                }
                return nullptr;
            }
        }

        bool IsReady(CWorld* World)
        {
            return FirstReadyNavMeshFromWorld(World) != nullptr;
        }

        int32 RequestRebuild(CWorld* World)
        {
            if (!World)
            {
                return 0;
            }
            int32 Count = 0;
            auto View = World->View<SNavMeshComponent>();
            for (entt::entity E : View)
            {
                View.get<SNavMeshComponent>(E).bBakeRequested = true;
                ++Count;
            }
            return Count;
        }

        bool FindPath(CWorld* World, const FVector3& Start, const FVector3& End, FNavPath& Out)
        {
            FNavMesh* Mesh = FirstReadyNavMeshFromWorld(World);
            FNavQueryFilter Filter;
            return Mesh && Mesh->FindPath(Start, End, Filter, Out);
        }

        bool ProjectPoint(CWorld* World, const FVector3& Point, const FVector3& Extents, FVector3& Out)
        {
            FNavMesh* Mesh = FirstReadyNavMeshFromWorld(World);
            FNavQueryFilter Filter;
            return Mesh && Mesh->ProjectPoint(Point, Extents, Filter, Out);
        }

        bool Raycast(CWorld* World, const FVector3& Start, const FVector3& End, FVector3& OutHit)
        {
            FNavMesh* Mesh = FirstReadyNavMeshFromWorld(World);
            FNavQueryFilter Filter;
            return Mesh && Mesh->Raycast(Start, End, Filter, OutHit);
        }

        bool FindRandomReachablePoint(CWorld* World, const FVector3& Origin, float Radius, FVector3& Out)
        {
            FNavMesh* Mesh = FirstReadyNavMeshFromWorld(World);
            FNavQueryFilter Filter;
            return Mesh && Mesh->FindRandomPoint(Origin, Radius, Filter, Out);
        }

        bool IsReachable(CWorld* World, const FVector3& From, const FVector3& To)
        {
            FNavPath Path;
            return FindPath(World, From, To, Path) && Path.bValid && !Path.bPartial;
        }

        float PathLength(CWorld* World, const FVector3& From, const FVector3& To)
        {
            FNavPath Path;
            if (!FindPath(World, From, To, Path) || !Path.bValid) return -1.0f;
            float Len = 0.0f;
            for (size_t i = 1; i < Path.Corners.size(); ++i)
            {
                Len += Math::Length(Path.Corners[i] - Path.Corners[i - 1]);
            }
            return Len;
        }

        void DrawPath(CWorld* World, const FNavPath& Path, const FVector4& Color, float Thickness, float Lift, float Duration)
        {
            if (!World || !Path.bValid || Path.Corners.size() < 2) return;

            const FVector3 LiftV(0.0f, Lift, 0.0f);
            for (size_t i = 1; i < Path.Corners.size(); ++i)
            {
                World->DrawLine(Path.Corners[i - 1] + LiftV, Path.Corners[i] + LiftV, Color, Thickness, true, Duration);
            }
            // Corner spheres so kinks read at a glance; goal sphere distinguished.
            const float R = 0.12f;
            for (size_t i = 0; i < Path.Corners.size(); ++i)
            {
                const bool bGoal = (i + 1 == Path.Corners.size());
                const FVector4 SphColor = bGoal ? FVector4(1.0f, 1.0f, 1.0f, 1.0f) : Color;
                World->DrawSphere(Path.Corners[i] + LiftV, bGoal ? R * 1.6f : R, SphColor, 10, 1.0f, true, Duration);
            }
            // Partial paths are easy to misread; flag them with a red marker on the last corner.
            if (Path.bPartial)
            {
                World->DrawSphere(Path.Corners.back() + LiftV, 0.25f, FVector4(1.0f, 0.2f, 0.2f, 1.0f), 12, 1.0f, true, Duration);
            }
        }

        bool DrawDebugPath(CWorld* World, const FVector3& From, const FVector3& To, const FVector4& Color, float Duration)
        {
            FNavPath Path;
            if (!FindPath(World, From, To, Path) || !Path.bValid) return false;
            DrawPath(World, Path, Color, 3.0f, 0.15f, Duration);
            return true;
        }
    }
}
