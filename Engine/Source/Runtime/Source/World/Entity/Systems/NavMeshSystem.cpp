#include "RuntimePCH.h"
#include "SignificanceSystem.h"
#include "NavMeshSystem.h"
#include "BlockoutSystem.h"
#include "FoliageTerrainSystem.h"

#include <algorithm>

#include "Platform/Time/PlatformTime.h"
#include "World/ECS/Registry.h"

#include "AI/Navigation/NavMesh.h"
#include "AI/Navigation/NavMeshBuilder.h"
#include "AI/Navigation/NavBuildPool.h"
#include "AI/Navigation/NavOctree.h"
#include "AI/Navigation/NavTileStreamer.h"
#include "Config/NavigationSettings.h"
#include "World/Entity/Systems/SignificanceSystem.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/Mesh/MeshCollision.h"
#include "Core/Console/ConsoleVariable.h"
#include "Physics/CollisionGen.h"
#include "Renderer/MeshData.h"
#include "Renderer/Vertex.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/DynamicMeshComponent.h"
#include "World/Entity/Components/FoliageComponent.h"
#include "World/Entity/Components/NavLinkComponent.h"
#include "World/Entity/Components/NavMeshComponent.h"
#include "World/Entity/Components/NavModifierComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/StaticMeshComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/EntityUtils.h"
#include "World/World.h"
#include "World/WorldTypes.h"
#include "Log/Log.h"
#include "Renderer/MeshQuantization.h"

namespace Lumina
{
    void SNavMeshSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);
        RequireUpdate(EUpdateStage::Paused);
        // Rebuilt tiles read the terrain, foliage and blockout meshes those systems settle.
        RunsAfter<SFoliageTerrainSystem, SBlockoutSystem, SSignificanceSystem>();
    }

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
    static TConsoleVar<bool>  CVarNavDebugModifiers ("Nav.Debug.Modifiers",    true,  "Wireframe box for each nav area modifier volume, colored by its area.");
    static TConsoleVar<bool>  CVarNavDebugLinks     ("Nav.Debug.OffMeshLinks", true,  "Arrows for off-mesh connections.");
    static TConsoleVar<bool>  CVarNavDebugLog       ("Nav.Debug.LogStats",     false, "Log triangle/edge/tile counts on every cache refresh.");
    static TConsoleVar<bool>  CVarNavTimings        ("Nav.Debug.Timings",      false, "Log where navmesh gather, bake and hydration wall-clock goes.");
    static TConsoleVar<float> CVarNavDebugLift      ("Nav.Debug.LiftY",        0.05f, "Vertical offset added to debug geometry to avoid Z-fighting.");
    static TConsoleVar<float> CVarNavDebugVertSize  ("Nav.Debug.VertexRadius", 0.08f, "Radius of vertex spheres (also drives center-sphere size).");


    // NOLINTEND(bugprone-throwing-static-initialization)

    namespace NavMeshSystemInternal
    {
        // Alpha is unused by the line batcher but kept consistent for future translucent rendering.
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

        void RetireNavMesh(FNavMeshRuntime& RT)
        {
            if (RT.Mesh)
            {
                RT.RetiredEpoch = Math::Max(RT.RetiredEpoch, RT.Mesh->GetTopologyEpoch());
                RT.Mesh.reset();
            }
        }

        void DrawNavDebug(const SSystemContext& Context, SNavMeshSystem::FSourceQueries& Queries, SNavMeshComponent& Comp)
        {
            const FNavMesh& Mesh = *Comp.Runtime.Mesh;
            const FVector3 Lift(0.0f, CVarNavDebugLift.GetValue(), 0.0f);
            const bool bColorByArea = CVarNavDebugColorArea.GetValue();

            // Translucent filled walkable surface, the headline is-this-walkable read.
            if (CVarNavDebugSurface.GetValue())
            {
                const float Alpha = CVarNavDebugSurfAlpha.GetValue();
                FNavMeshRuntime& RT = Comp.Runtime;

                // Hundreds of thousands of vertices, so rebuilding it per frame cost more than the bake.
                const uint64 Epoch = Mesh.GetTopologyEpoch();
                if (!RT.bDebugSurfaceValid || RT.DebugSurfaceEpoch != Epoch
                    || RT.DebugSurfaceAlpha != Alpha || RT.bDebugSurfaceByArea != bColorByArea)
                {
                    const FNavDebugStats Stats = Mesh.GetDebugStats();
                    RT.DebugSurface.Reset();
                    RT.DebugSurface.Reserve(IntCastChecked<int32>(static_cast<int64>(Stats.Triangles) * 3));
                    Mesh.ForEachTriangle([&](const FVector3& A, const FVector3& B, const FVector3& C, uint8 Area)
                    {
                        FVector4 Color = bColorByArea ? NavAreaColor(Area) : FVector4(0.15f, 0.85f, 0.35f, 1.0f);
                        Color.w = Alpha;
                        const uint32 Packed = PackColor(Color);
                        RT.DebugSurface.Emplace(A + Lift, Packed);
                        RT.DebugSurface.Emplace(B + Lift, Packed);
                        RT.DebugSurface.Emplace(C + Lift, Packed);
                    });
                    RT.DebugSurfaceEpoch   = Epoch;
                    RT.DebugSurfaceAlpha   = Alpha;
                    RT.bDebugSurfaceByArea = bColorByArea;
                    RT.bDebugSurfaceValid  = true;
                }

                if (!RT.DebugSurface.IsEmpty())
                {
                    // The batcher takes ownership, so the cache is copied rather than moved out of.
                    TVector<FSimpleElementVertex> Frame(RT.DebugSurface);
                    Context.DrawDebugSolidTriangles(std::move(Frame), ESolidDrawMode::Translucent, -1.0f);
                }
            }

            if (CVarNavDebugBounds.GetValue())
            {
                Context.DrawDebugBox(Comp.Center, Comp.GetWorldExtents(), FQuat(1.0f, 0.0f, 0.0f, 0.0f),
                    FVector4(1.0f, 0.85f, 0.10f, 1.0f), 4.0f, -1.0f);
            }

            // Modifiers are authored, not baked, so they draw from their components rather than the navmesh.
            if (CVarNavDebugModifiers.GetValue())
            {
                Queries.Modifiers.ForEach([&](const SNavModifierComponent& Modifier, const STransformComponent& Xform)
                {
                    if (!Modifier.bEnabled) return;

                    const FTransform& WT = Xform.GetWorldTransform();
                    const FVector3 Scale = WT.GetScale();
                    const FVector3 Center = WT.GetLocation() + Math::Rotate(WT.GetRotation(), Modifier.Offset * Scale);
                    const FVector3 Half(Modifier.Extents.x * std::fabs(Scale.x), Modifier.Extents.y * std::fabs(Scale.y), Modifier.Extents.z * std::fabs(Scale.z));
                    Context.DrawDebugBox(Center, Half, WT.GetRotation(), NavAreaColor((uint8)Modifier.Area), 3.0f, -1.0f);
                });
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

            // Poly perimeter drawn thick and bright so the shape stands out over the fill.
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
                const float R = Math::Max(0.02f, CVarNavDebugVertSize.GetValue() * 0.5f);
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

    namespace NavMeshSystemInternal
    {
        const CNavigationSettings& NavSettings()
        {
            return *GetDefault<CNavigationSettings>();
        }

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
        enum class ENavColliderType : uint8 { Box = 0, Sphere = 1, Mesh = 2, CharacterCapsule = 3, Capsule = 4, Cylinder = 5, Terrain = 6, TriangleSoup = 7, DynamicMesh = 8, AreaVolume = 9, OffMeshLink = 10, Foliage = 11 };

        // Compound children reuse the primitive types, so their sub-indices sit above any mesh collision primitive's.
        constexpr uint32 CompoundSubIndexBase = 1u << 23;

        FORCEINLINE uint64 PackSourceKey(ECS::FEntity E, ENavColliderType T)
        {
            return ((uint64)(uint32)E << 8) | (uint64)T;
        }

        // SubIndex 0 is byte-identical to the plain key, which the change detector relies on.
        FORCEINLINE uint64 PackSourceKey(ECS::FEntity E, ENavColliderType T, uint32 SubIndex)
        {
            return ((uint64)SubIndex << 40) | ((uint64)(uint32)E << 8) | (uint64)T;
        }

        // Catches geometry that changed under a bounding box that did not move.
        FORCEINLINE uint64 MakeContentId(const void* Asset, size_t CountA, size_t CountB)
        {
            size_t Seed = (uintptr_t)Asset;
            Hash::HashCombine(Seed, CountA);
            Hash::HashCombine(Seed, CountB);
            return (uint64)Seed;
        }

        // Retuning a link leaves its AABB identical, so the settings ride in the key to dirty the tile.
        FORCEINLINE uint32 PackLinkSubIndex(const SNavLinkComponent& Link)
        {
            return (uint32)Link.Flag | ((uint32)Link.Area << 16) | ((uint32)(Link.bBidirectional ? 1 : 0) << 22);
        }

        // Matches body placement so nav geometry overlaps physics exactly.
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
            const uint32 Base = (uint32)Acc.Vertices.Num();
            Acc.Vertices.Add(A);
            Acc.Vertices.Add(B);
            Acc.Vertices.Add(C);
            Acc.Indices.Add(Base + 0);
            Acc.Indices.Add(Base + 1);
            Acc.Indices.Add(Base + 2);
            Acc.AABBMin = Math::Min(Acc.AABBMin, Math::Min(A, Math::Min(B, C)));
            Acc.AABBMax = Math::Max(Acc.AABBMax, Math::Max(A, Math::Max(B, C)));
        }

        // Background work is never run by a waiting thread, so a gather the game thread blocks on has to outrank a bake in flight.
        Jobs::EJobPriority GatherPriority()
        {
            return Threading::IsMainThread() ? Jobs::EJobPriority::High : Jobs::EJobPriority::Background;
        }

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
                const float Phi = Math::Pi<float>() * (float)s / (float)Stacks;
                const float SinP = std::sin(Phi);
                const float CosP = std::cos(Phi);
                for (int g = 0; g <= Segments; ++g)
                {
                    const float Theta = Math::TwoPi<float>() * (float)g / (float)Segments;
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

        // Cylinder + hemispheres along +Y (capsule shape). Total height = 2*HalfHeight + 2*Radius.
        void EmitCapsuleGeometry(const FMatrix4& W, float HalfHeight, float Radius, const FVector3& BakeMin, const FVector3& BakeMax, FGatherAccumulator& Acc)
        {
            constexpr int Segments = 12;
            constexpr int HemiStacks = 4;
            const int Rings = 2 * HemiStacks + 2;

            TVector<FVector3> Verts;
            Verts.SetNum(Rings * (Segments + 1));

            int RingIdx = 0;
            // Top hemisphere at +Y * HalfHeight.
            for (int s = 0; s <= HemiStacks; ++s, ++RingIdx)
            {
                const float Phi = Math::HalfPi<float>() * (float)s / (float)HemiStacks;
                const float SinP = std::sin(Phi);
                const float CosP = std::cos(Phi);
                for (int g = 0; g <= Segments; ++g)
                {
                    const float Theta = Math::TwoPi<float>() * (float)g / (float)Segments;
                    const FVector3 Local(Radius * SinP * std::cos(Theta), HalfHeight + Radius * CosP, Radius * SinP * std::sin(Theta));
                    Verts[RingIdx * (Segments + 1) + g] = FVector3(W * FVector4(Local, 1.0f));
                }
            }
            // Bottom hemisphere at -Y * HalfHeight.
            for (int s = 1; s <= HemiStacks + 1; ++s, ++RingIdx)
            {
                const float Phi = Math::HalfPi<float>() + Math::HalfPi<float>() * (float)s / (float)(HemiStacks + 1);
                const float SinP = std::sin(Phi);
                const float CosP = std::cos(Phi);
                for (int g = 0; g <= Segments; ++g)
                {
                    const float Theta = Math::TwoPi<float>() * (float)g / (float)Segments;
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

        // Flat-capped cylinder along +Y (cylinder hull). CapRadius rounding is ignored for nav.
        void EmitCylinderGeometry(const FMatrix4& W, float HalfHeight, float Radius, const FVector3& BakeMin, const FVector3& BakeMax, FGatherAccumulator& Acc)
        {
            constexpr int Segments = 16;
            FVector3 Top[Segments + 1];
            FVector3 Bot[Segments + 1];
            for (int i = 0; i <= Segments; ++i)
            {
                const float Theta = Math::TwoPi<float>() * (float)i / (float)Segments;
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

        // Monotone chain over the XZ projection, which survives the top and bottom of a box landing on the same points.
        void BuildXZHull(const FVector3* Pts, int32 N, TVector<FVector3>& Out)
        {
            Out.Reset();
            TVector<FVector3> Sorted;
            Sorted.Reserve(N);
            for (int32 i = 0; i < N; ++i)
            {
                Sorted.Emplace(Pts[i].x, 0.0f, Pts[i].z);
            }
            Algo::Sort(Sorted, [](const FVector3& A, const FVector3& B) { return A.x < B.x || (A.x == B.x && A.z < B.z); });

            auto Turn = [](const FVector3& O, const FVector3& A, const FVector3& B)
            {
                return (A.x - O.x) * (B.z - O.z) - (A.z - O.z) * (B.x - O.x);
            };
            for (int32 Pass = 0; Pass < 2; ++Pass)
            {
                const int32 ChainStart = Out.Num();
                for (int32 k = 0; k < Sorted.Num(); ++k)
                {
                    const FVector3& P = Sorted[Pass == 0 ? k : Sorted.Num() - 1 - k];
                    while (Out.Num() >= ChainStart + 2 && Turn(Out[Out.Num() - 2], Out.Last(), P) <= 0.0f)
                    {
                        Out.Pop();
                    }
                    Out.Add(P);
                }
                // Each chain ends on the point the other starts from.
                Out.Pop();
            }
            if (Out.Num() < 3)
            {
                Out.Reset();
            }
        }

        // Explicit Mesh wins; falls back to StaticMeshComponent. Mirrors the collider resolution.
        CStaticMesh* ResolveMeshColliderAsset(const SMeshColliderComponent& MC, const SStaticMeshComponent* Fallback)
        {
            if (CStaticMesh* M = MC.Mesh.Get())
            {
                return M;
            }
            return Fallback ? Fallback->StaticMesh.Get() : nullptr;
        }

        void EmitMeshGeometry(const FMatrix4& W, const FMeshResource& Res, const FVector3& BakeMin, const FVector3& BakeMax, FGatherAccumulator& Acc, bool bCoarsestLOD = false)
        {
            const FMeshletData&  Md  = Res.MeshletData;
            if (Md.IsEmpty() || Res.bSkinnedMesh) return;

            const TVector<uint32>& MT = Md.MeshletTriangles;

            for (const FGeometrySurface& Surface : Res.GeometrySurfaces)
            {
                uint32 Lod = 0;
                if (bCoarsestLOD)
                {
                    for (uint32 Candidate = 1; Candidate < MAX_MESH_LODS; ++Candidate)
                    {
                        Lod = Surface.LODMeshletCount[Candidate] != 0u ? Candidate : Lod;
                    }
                }
                const uint32 First = Surface.LODMeshletOffset[Lod];
                const uint32 Count = Surface.LODMeshletCount[Lod];
                for (uint32 m = 0; m < Count; ++m)
                {
                    const FMeshlet& Meshlet = Md.Meshlets[First + m];

                    FVector3 LocalVerts[MESHLET_MAX_VERTICES];
                    for (uint32 v = 0; v < Meshlet.VertexCount; ++v)
                    {
                        const FVector3 Local = GetMeshletVertexPosition(Md, Meshlet, v, false);
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

        // A window of a heightfield copied on the game thread, so a bake worker tessellates it without touching the live component.
        struct FNavTerrainPatch
        {
            TVector<float> Heights;
            FMatrix4       World = FMatrix4(1.0f);
            FVector3       BakeMin = FVector3(0.0f);
            FVector3       BakeMax = FVector3(0.0f);
            float          Stride = 1.0f;
            float          Half = 0.0f;
            int32          ColBegin = 0;
            int32          ColEnd = 0;
            int32          RowBegin = 0;
            int32          RowEnd = 0;
            int32          Step = 1;
        };

        // Clipped to the bake box first, or a volume covering a slice of a huge terrain still pays for the whole thing and gets coarsened by it.
        TSharedPtr<FNavTerrainPatch> CaptureTerrainPatch(const STerrainComponent& T, const FMatrix4& W, const FVector3& BakeMin, const FVector3& BakeMax, float CellSize)
        {
            const int32 Res = T.Resolution;
            if (Res < 2 || T.Heightmap.Num() < static_cast<int64>(Res) * Res)
            {
                return nullptr;
            }

            auto Patch = MakeShared<FNavTerrainPatch>();
            Patch->World = W;
            Patch->BakeMin = BakeMin;
            Patch->BakeMax = BakeMax;
            Patch->Stride = T.TileWorldSize / float(Res - 1);
            Patch->Half = T.TileWorldSize * 0.5f;

            const FMatrix4 ToLocal = Math::Inverse(W);
            FVector3 LocalMin( FLT_MAX);
            FVector3 LocalMax(-FLT_MAX);
            const FVector3 Corners[8] = {
                {BakeMin.x, BakeMin.y, BakeMin.z}, {BakeMax.x, BakeMin.y, BakeMin.z},
                {BakeMin.x, BakeMax.y, BakeMin.z}, {BakeMax.x, BakeMax.y, BakeMin.z},
                {BakeMin.x, BakeMin.y, BakeMax.z}, {BakeMax.x, BakeMin.y, BakeMax.z},
                {BakeMin.x, BakeMax.y, BakeMax.z}, {BakeMax.x, BakeMax.y, BakeMax.z},
            };
            for (const FVector3& Corner : Corners)
            {
                const FVector3 L = FVector3(ToLocal * FVector4(Corner, 1.0f));
                LocalMin = Math::Min(LocalMin, L);
                LocalMax = Math::Max(LocalMax, L);
            }

            // One quad of slack each way, so the clipped edge still shares vertices with its neighbor.
            Patch->ColBegin = Math::Clamp((int32)std::floor((LocalMin.x + Patch->Half) / Patch->Stride) - 1, 0, Res - 2);
            Patch->ColEnd   = Math::Clamp((int32)std::ceil ((LocalMax.x + Patch->Half) / Patch->Stride) + 1, Patch->ColBegin + 1, Res - 1);
            Patch->RowBegin = Math::Clamp((int32)std::floor((LocalMin.z + Patch->Half) / Patch->Stride) - 1, 0, Res - 2);
            Patch->RowEnd   = Math::Clamp((int32)std::ceil ((LocalMax.z + Patch->Half) / Patch->Stride) + 1, Patch->RowBegin + 1, Res - 1);

            // One cell per quad is the finest worth emitting, and only the emitted span is capped, since coarser quads turn relief into unwalkable facets.
            Patch->Step = Math::Max(1, (int32)std::floor(Math::Max(CellSize, 0.01f) / Math::Max(Patch->Stride, 1e-4f)));
            const int32 SpanQuads = Math::Max(Patch->ColEnd - Patch->ColBegin, Patch->RowEnd - Patch->RowBegin);
            Patch->Step = Math::Max(Patch->Step, (int32)std::ceil((float)SpanQuads / 1024.0f));

            const int32 Cols = Patch->ColEnd - Patch->ColBegin + 1;
            const int32 Rows = Patch->RowEnd - Patch->RowBegin + 1;
            Patch->Heights.SetNum(IntCastChecked<int32>(static_cast<int64>(Cols) * Rows));
            for (int32 Row = 0; Row < Rows; ++Row)
            {
                const float* Source = &T.Heightmap[(Patch->RowBegin + Row) * Res + Patch->ColBegin];
                float* Dest = &Patch->Heights[Row * Cols];
                for (int32 Col = 0; Col < Cols; ++Col)
                {
                    Dest[Col] = Source[Col] * T.MaxHeight;
                }
            }
            return Patch;
        }

        // Subsampled so a quad is about one cell, keeping only tris overlapping the bake bounds.
        void TessellateTerrainPatch(const FNavTerrainPatch& Patch, TVector<FVector3>& Out, FNavBuildPool* Pool)
        {
            const int32 Cols = Patch.ColEnd - Patch.ColBegin + 1;
            auto Pos = [&](int32 Col, int32 Row) -> FVector3
            {
                const float Height = Patch.Heights[(Row - Patch.RowBegin) * Cols + (Col - Patch.ColBegin)];
                const FVector3 Local(-Patch.Half + (float)Col * Patch.Stride, Height, -Patch.Half + (float)Row * Patch.Stride);
                return FVector3(Patch.World * FVector4(Local, 1.0f));
            };

            const int32 Step = Patch.Step;
            const int32 RowStrips = (Patch.RowEnd - Patch.RowBegin + Step - 1) / Step;
            if (RowStrips <= 0)
            {
                return;
            }

            // Rows are independent, and on a world-sized bake this is the one source worth fanning out.
            TVector<TVector<FVector3>> Strips((size_t)RowStrips);
            NavParallelFor(Pool, (uint32)RowStrips, [&](uint32 StripIndex)
            {
                const int32 Row  = Patch.RowBegin + (int32)StripIndex * Step;
                const int32 RowN = Math::Min(Row + Step, Patch.RowEnd);
                TVector<FVector3>& Strip = Strips[StripIndex];

                for (int32 Col = Patch.ColBegin; Col < Patch.ColEnd; Col += Step)
                {
                    const int32 ColN = Math::Min(Col + Step, Patch.ColEnd);
                    const FVector3 A = Pos(Col,  Row);
                    const FVector3 B = Pos(ColN, Row);
                    const FVector3 C = Pos(ColN, RowN);
                    const FVector3 D = Pos(Col,  RowN);
                    // Wound so the surface normal points +Y (Recast's slope test marks it walkable).
                    if (TriIntersectsAABB(A, C, B, Patch.BakeMin, Patch.BakeMax))
                    {
                        Strip.Add(A); Strip.Add(C); Strip.Add(B);
                    }
                    if (TriIntersectsAABB(A, D, C, Patch.BakeMin, Patch.BakeMax))
                    {
                        Strip.Add(A); Strip.Add(D); Strip.Add(C);
                    }
                }
            }, 1, GatherPriority());

            size_t Total = 0;
            for (const TVector<FVector3>& Strip : Strips)
            {
                Total += Strip.Num();
            }
            Out.Reserve(IntCastChecked<int32>(Out.Num() + Total));
            for (TVector<FVector3>& Strip : Strips)
            {
                Out.Append(Strip);
            }
        }

        // POD shape snapshot; safe to ship to a bake worker (Mesh ptr / terrain tri-soup are read-only there).
        struct FNavSourcePrim
        {
            ENavColliderType                Type  = ENavColliderType::Box;
            FMatrix4                        World = FMatrix4(1.0f);
            FVector3                        Shape = FVector3(0.0f);   // Box half-extent, or Radius and HalfHeight
            CStaticMesh*                    Mesh  = nullptr;
            TSharedPtr<FDynamicMeshRenderData> DynamicMesh;          // held so a re-commit on the game thread cannot free it under the bake
            TSharedPtr<TVector<FVector3>>   TriangleSoup;
            TSharedPtr<FNavTerrainPatch>    TerrainPatch;
            TSharedPtr<FNavAreaVolume>      AreaVolume;
            TSharedPtr<FNavOffMeshLink>     Link;
            // Foliage type, shared by every instance of a species in its local space and placed by World.
            TSharedPtr<const TVector<FNavSourcePrim>> Children;
            // Reads the mesh's coarsest LOD, enough for a footprint at a fraction of the triangles.
            bool                            bCoarsestLOD = false;
        };

        bool IsAnnotation(ENavColliderType Type)
        {
            return Type == ENavColliderType::AreaVolume || Type == ENavColliderType::OffMeshLink;
        }

        struct FNavSourceEntry
        {
            uint64          Key = 0;
            FNavSourcePrim  Prim;
            FVector3        AABBMin = FVector3( FLT_MAX);
            FVector3        AABBMax = FVector3(-FLT_MAX);

            // Identifies the geometry behind the AABB; a change dirties the tiles exactly like a move.
            uint64          ContentId = 0;
        };

        void EmitNavSourcePrim(const FNavSourcePrim& P, const FVector3& BakeMin, const FVector3& BakeMax, FGatherAccumulator& Acc)
        {
            switch (P.Type)
            {
                case ENavColliderType::Box:    EmitBoxGeometry(P.World, P.Shape, BakeMin, BakeMax, Acc); break;
                case ENavColliderType::Sphere: EmitSphereGeometry(P.World, P.Shape.x, BakeMin, BakeMax, Acc); break;
                case ENavColliderType::Mesh:   if (P.Mesh) EmitMeshGeometry(P.World, P.Mesh->GetMeshResource(), BakeMin, BakeMax, Acc, P.bCoarsestLOD); break;
                case ENavColliderType::DynamicMesh: if (P.DynamicMesh) EmitMeshGeometry(P.World, P.DynamicMesh->Resource, BakeMin, BakeMax, Acc); break;
                case ENavColliderType::Capsule:
                case ENavColliderType::CharacterCapsule: EmitCapsuleGeometry(P.World, P.Shape.y, P.Shape.x, BakeMin, BakeMax, Acc); break;
                case ENavColliderType::Cylinder: EmitCylinderGeometry(P.World, P.Shape.y, P.Shape.x, BakeMin, BakeMax, Acc); break;
                case ENavColliderType::Terrain:
                case ENavColliderType::TriangleSoup:
                    if (P.TriangleSoup)
                    {
                        const TVector<FVector3>& Tris = *P.TriangleSoup;
                        for (int32 i = 0; i + 2 < Tris.Num(); i += 3)
                        {
                            EmitTri(Acc, BakeMin, BakeMax, Tris[i], Tris[i + 1], Tris[i + 2]);
                        }
                    }
                    break;
                case ENavColliderType::Foliage:
                    if (P.Children)
                    {
                        for (const FNavSourcePrim& Child : *P.Children)
                        {
                            if (Child.TriangleSoup)
                            {
                                const TVector<FVector3>& Tris = *Child.TriangleSoup;
                                for (int32 i = 0; i + 2 < Tris.Num(); i += 3)
                                {
                                    EmitTri(Acc, BakeMin, BakeMax, FVector3(P.World * FVector4(Tris[i], 1.0f)),
                                        FVector3(P.World * FVector4(Tris[i + 1], 1.0f)), FVector3(P.World * FVector4(Tris[i + 2], 1.0f)));
                                }
                                continue;
                            }
                            FNavSourcePrim Placed = Child;
                            Placed.World = P.World * Child.World;
                            EmitNavSourcePrim(Placed, BakeMin, BakeMax, Acc);
                        }
                    }
                    break;
                case ENavColliderType::AreaVolume:
                case ENavColliderType::OffMeshLink:
                    break;
            }
        }

        // Runs on the bake worker, turning each copied heightfield window into the triangles emission reads.
        void TessellateTerrainPatches(TVector<FNavSourcePrim>& Prims, FNavBuildPool* Pool = nullptr)
        {
            for (FNavSourcePrim& Prim : Prims)
            {
                if (Prim.TerrainPatch && !Prim.TriangleSoup)
                {
                    Prim.TriangleSoup = MakeShared<TVector<FVector3>>();
                    TessellateTerrainPatch(*Prim.TerrainPatch, *Prim.TriangleSoup, Pool);
                    Prim.TerrainPatch.reset();
                }
            }
        }

        // Emission is pure per prim, so it fans out into per-chunk buffers and concatenates with the indices rebased.
        void EmitNavSourcePrims(const FNavSourcePrim* Prims, size_t Count, const FVector3& EmitMin,
            const FVector3& EmitMax, TVector<FVector3>& OutVertices, TVector<uint32>& OutIndices, FNavBuildPool* Pool = nullptr)
        {
            if (Count == 0)
            {
                return;
            }

            const uint32 Workers   = Math::Max(1u, Jobs::GetNumWorkers());
            const uint32 NumChunks = (uint32)Math::Min<size_t>(Count, (size_t)Workers * 2u);
            const size_t PerChunk  = (Count + NumChunks - 1) / NumChunks;

            TVector<FGatherAccumulator> Chunks((size_t)NumChunks);
            NavParallelFor(Pool, NumChunks, [&](uint32 ChunkIndex)
            {
                const size_t Begin = (size_t)ChunkIndex * PerChunk;
                const size_t End   = Math::Min(Begin + PerChunk, Count);
                FGatherAccumulator& Acc = Chunks[ChunkIndex];
                for (size_t i = Begin; i < End; ++i)
                {
                    EmitNavSourcePrim(Prims[i], EmitMin, EmitMax, Acc);
                }
            }, 1, GatherPriority());

            size_t TotalVerts = 0;
            size_t TotalIndices = 0;
            for (const FGatherAccumulator& Acc : Chunks)
            {
                TotalVerts += Acc.Vertices.Num();
                TotalIndices += Acc.Indices.Num();
            }
            OutVertices.Reserve(IntCastChecked<int32>(OutVertices.Num() + TotalVerts));
            OutIndices.Reserve(IntCastChecked<int32>(OutIndices.Num() + TotalIndices));

            for (FGatherAccumulator& Acc : Chunks)
            {
                const uint32 Base = (uint32)OutVertices.Num();
                OutVertices.Append(Acc.Vertices);
                for (uint32 Index : Acc.Indices)
                {
                    OutIndices.Add(Base + Index);
                }
            }
        }

        // Annotations travel with the geometry prims so every bake path sees the same authored set.
        void AppendAnnotation(const FNavSourcePrim& Prim, TVector<FNavAreaVolume>& OutVolumes, TVector<FNavOffMeshLink>& OutLinks)
        {
            if (Prim.AreaVolume)
            {
                OutVolumes.Add(*Prim.AreaVolume);
            }
            else if (Prim.Link)
            {
                OutLinks.Add(*Prim.Link);
            }
        }

        // Change detector and cache rebuild MUST agree byte for byte, so all four share this walk.
        void CornersAABB(const FMatrix4& W, const FVector3* Local, int32 N, FVector3& Mn, FVector3& Mx)
        {
            Mn = FVector3( FLT_MAX);
            Mx = FVector3(-FLT_MAX);
            for (int32 i = 0; i < N; ++i)
            {
                const FVector3 Pt = FVector3(W * FVector4(Local[i], 1.0f));
                Mn = Math::Min(Mn, Pt);
                Mx = Math::Max(Mx, Pt);
            }
        }

        float ScaledRadius(const FMatrix4& W, float Radius)
        {
            const float Sx = Math::Length(FVector3(W[0]));
            const float Sy = Math::Length(FVector3(W[1]));
            const float Sz = Math::Length(FVector3(W[2]));
            return Radius * Math::Max(Sx, Math::Max(Sy, Sz));
        }

        // One source per collision piece, so nav sees the same decomposed shape physics does. Emit takes the key type and sub-index.
        template<typename FnEmit>
        void ForEachMeshCollisionPrim(const SMeshCollision& Asset, const FMatrix4& ColliderWorld, FnEmit&& Emit)
        {
            if (Asset.IsConcave())
            {
                FNavSourceEntry Entry;
                Entry.Prim.Type = ENavColliderType::TriangleSoup;
                Entry.Prim.World = ColliderWorld;
                Entry.Prim.TriangleSoup = MakeShared<TVector<FVector3>>();

                Entry.Prim.TriangleSoup->Reserve(Asset.TriangleIndices.Num());
                for (uint32 Index : Asset.TriangleIndices)
                {
                    const FVector3 P = FVector3(ColliderWorld * FVector4(Asset.TriangleVertices[Index], 1.0f));
                    Entry.Prim.TriangleSoup->Add(P);
                    Entry.AABBMin = Math::Min(Entry.AABBMin, P);
                    Entry.AABBMax = Math::Max(Entry.AABBMax, P);
                }

                Emit(ENavColliderType::TriangleSoup, 0u, std::move(Entry));
                return;
            }

            for (int32 i = 0; i < Asset.Primitives.Num(); ++i)
            {
                const SCollisionPrimitive& Primitive = Asset.Primitives[i];

                FNavSourceEntry Entry;
                Entry.Prim.World = ColliderWorld * Math::Translate(FMatrix4(1.0f), Primitive.Center)
                                                 * Math::ToMatrix4(FQuat(Math::Radians(Primitive.Rotation)));

                switch (Primitive.Type)
                {
                case ECollisionPrimitiveType::Box:
                    {
                        Entry.Prim.Type = ENavColliderType::Box;
                        Entry.Prim.Shape = Primitive.HalfExtent;

                        const FVector3 H = Primitive.HalfExtent;
                        const FVector3 Corners[8] = {
                            {-H.x,-H.y,-H.z}, { H.x,-H.y,-H.z}, { H.x,-H.y, H.z}, {-H.x,-H.y, H.z},
                            {-H.x, H.y,-H.z}, { H.x, H.y,-H.z}, { H.x, H.y, H.z}, {-H.x, H.y, H.z},
                        };
                        CornersAABB(Entry.Prim.World, Corners, 8, Entry.AABBMin, Entry.AABBMax);
                    }
                    break;

                case ECollisionPrimitiveType::Sphere:
                    {
                        Entry.Prim.Type = ENavColliderType::Sphere;
                        Entry.Prim.Shape = FVector3(Primitive.Radius, 0.0f, 0.0f);

                        const FVector3 Center = FVector3(Entry.Prim.World * FVector4(0.0f, 0.0f, 0.0f, 1.0f));
                        const float R = ScaledRadius(Entry.Prim.World, Primitive.Radius);
                        Entry.AABBMin = Center - FVector3(R);
                        Entry.AABBMax = Center + FVector3(R);
                    }
                    break;

                case ECollisionPrimitiveType::Capsule:
                    {
                        Entry.Prim.Type = ENavColliderType::Capsule;
                        Entry.Prim.Shape = FVector3(Primitive.Radius, Primitive.HalfHeight, 0.0f);

                        const FVector3 Center = FVector3(Entry.Prim.World * FVector4(0.0f, 0.0f, 0.0f, 1.0f));
                        const float R = ScaledRadius(Entry.Prim.World, Primitive.Radius + Primitive.HalfHeight);
                        Entry.AABBMin = Center - FVector3(R);
                        Entry.AABBMax = Center + FVector3(R);
                    }
                    break;

                case ECollisionPrimitiveType::ConvexHull:
                    {
                        TVector<FVector3> HullVertices;
                        TVector<uint32> HullIndices;
                        if (!Physics::CollisionGen::BuildHullTriangles(Primitive.HullPoints, HullVertices, HullIndices))
                        {
                            continue;
                        }

                        Entry.Prim.Type = ENavColliderType::TriangleSoup;
                        Entry.Prim.TriangleSoup = MakeShared<TVector<FVector3>>();

                        Entry.Prim.TriangleSoup->Reserve(HullIndices.Num());
                        for (uint32 Index : HullIndices)
                        {
                            const FVector3 P = FVector3(Entry.Prim.World * FVector4(HullVertices[Index], 1.0f));
                            Entry.Prim.TriangleSoup->Add(P);
                            Entry.AABBMin = Math::Min(Entry.AABBMin, P);
                            Entry.AABBMax = Math::Max(Entry.AABBMax, P);
                        }
                    }
                    break;
                }

                Emit(Entry.Prim.Type, i, std::move(Entry));
            }
        }

        // A species' collision in its own space, built once per scan and placed per instance.
        struct FFoliageNavTemplate
        {
            TSharedPtr<const TVector<FNavSourcePrim>> Children;
            FVector3 LocalMin = FVector3( FLT_MAX);
            FVector3 LocalMax = FVector3(-FLT_MAX);
            uint64   ContentId = 0;
        };

        // Mirrors SFoliageCollisionSystem, an authored shape first and the mesh otherwise, read as triangles like a mesh collider.
        FFoliageNavTemplate BuildFoliageNavTemplate(const SFoliageType& Type)
        {
            FFoliageNavTemplate Template;
            if (!Type.bEnableCollision)
            {
                return Template;
            }

            auto Children = MakeShared<TVector<FNavSourcePrim>>();
            if (const SMeshCollision* Shape = Type.GetCollision())
            {
                ForEachMeshCollisionPrim(*Shape, FMatrix4(1.0f), [&](ENavColliderType, uint32, FNavSourceEntry&& Entry)
                {
                    Template.LocalMin = Math::Min(Template.LocalMin, Entry.AABBMin);
                    Template.LocalMax = Math::Max(Template.LocalMax, Entry.AABBMax);
                    Children->Add(std::move(Entry.Prim));
                });
                Template.ContentId = MakeContentId(Shape, Shape->TriangleIndices.Num(), Shape->Primitives.Num());
            }
            else if (CStaticMesh* Mesh = Type.Mesh.Get(); Mesh != nullptr && !Mesh->GetMeshResource().bSkinnedMesh)
            {
                FNavSourcePrim& Child = Children->EmplaceGetRef();
                Child.Type = ENavColliderType::Mesh;
                Child.Mesh = Mesh;
                Child.bCoarsestLOD = true;
                const FMeshletData& MeshletData = Mesh->GetMeshResource().MeshletData;
                Template.ContentId = MakeContentId(Mesh, MeshletData.VertexPositions.Num(), MeshletData.MeshletTriangles.Num());
                Template.LocalMin = Mesh->GetAABB().Min;
                Template.LocalMax = Mesh->GetAABB().Max;
            }

            if (!Children->IsEmpty())
            {
                Template.Children = Children;
            }
            return Template;
        }

        uint64 FoliageNavSignature(const SFoliageComponent& Foliage)
        {
            size_t Seed = Foliage.InstancesVersion;
            Hash::HashCombine(Seed, Foliage.Types.Num());
            for (const SFoliageType& Type : Foliage.Types)
            {
                Hash::HashCombine(Seed, (size_t)Type.bEnableCollision);
                const SMeshCollision* Shape = Type.GetCollision();
                Hash::HashCombine(Seed, (uintptr_t)Shape);
                if (Shape != nullptr)
                {
                    Hash::HashCombine(Seed, Shape->TriangleIndices.Num());
                    Hash::HashCombine(Seed, Shape->Primitives.Num());
                }
                CStaticMesh* Mesh = Type.Mesh.Get();
                Hash::HashCombine(Seed, (uintptr_t)Mesh);
                if (Mesh != nullptr)
                {
                    Hash::HashCombine(Seed, Mesh->GetMeshResource().MeshletData.MeshletTriangles.Num());
                }
            }
            return (uint64)Seed;
        }
    }

    struct FNavFoliageEntity
    {
        uint64                       Signature = 0;
        bool                         bBuilt    = false;
        bool                         bSeen     = false;
        TVector<NavMeshSystemInternal::FFoliageNavTemplate> Templates;
        // Per instance, with Min above Max where the instance has no collision to bake.
        TVector<FVector3>            Min;
        TVector<FVector3>            Max;

        // What the last change scan saw, so a changed entity is diffed instance by instance against it.
        bool                         bDetected = false;
        uint64                       DetectedSignature = 0;
        TVector<FVector3>            DetectedMin;
        TVector<FVector3>            DetectedMax;
    };

    struct FNavFoliageCache
    {
        THashMap<uint32, FNavFoliageEntity> Entities;
    };

    namespace NavMeshSystemInternal
    {
        bool HasFoliageBounds(const FVector3& Mn, const FVector3& Mx)
        {
            return Mn.x <= Mx.x;
        }

        // Rebuilt only when the instances or a species' collision change, so a steady scan never re-places instances.
        FNavFoliageEntity& RefreshFoliageEntity(FNavFoliageCache& Cache, ECS::FEntity E, const SFoliageComponent& Foliage)
        {
            FNavFoliageEntity& Entry = Cache.Entities[(uint32)E];
            Entry.bSeen = true;

            const uint64 Signature = FoliageNavSignature(Foliage);
            if (Entry.bBuilt && Entry.Signature == Signature)
            {
                return Entry;
            }
            Entry.Signature = Signature;
            Entry.bBuilt    = true;

            Entry.Templates.Reset();
            Entry.Templates.Reserve(Foliage.Types.Num());
            for (const SFoliageType& Type : Foliage.Types)
            {
                Entry.Templates.Add(BuildFoliageNavTemplate(Type));
            }

            constexpr uint32 MaxKeyedInstances = 1u << 24;
            const uint32 NumInstances = Math::Min<uint32>(Foliage.Instances.Num(), MaxKeyedInstances);
            Entry.Min.Init(FVector3( FLT_MAX), NumInstances);
            Entry.Max.Init(FVector3(-FLT_MAX), NumInstances);
            for (uint32 Index = 0; Index < NumInstances; ++Index)
            {
                const SFoliageInstance& Instance = Foliage.Instances[Index];
                if (!Foliage.IsValidType(Instance.TypeIndex) || !Entry.Templates[Instance.TypeIndex].Children)
                {
                    continue;
                }

                const FFoliageNavTemplate& Template = Entry.Templates[Instance.TypeIndex];
                const FVector3 Mn = Template.LocalMin;
                const FVector3 Mx = Template.LocalMax;
                const FVector3 Corners[8] = {
                    {Mn.x, Mn.y, Mn.z}, {Mx.x, Mn.y, Mn.z}, {Mn.x, Mx.y, Mn.z}, {Mx.x, Mx.y, Mn.z},
                    {Mn.x, Mn.y, Mx.z}, {Mx.x, Mn.y, Mx.z}, {Mn.x, Mx.y, Mx.z}, {Mx.x, Mx.y, Mx.z},
                };
                CornersAABB(Instance.GetMatrix(), Corners, 8, Entry.Min[Index], Entry.Max[Index]);
            }
            return Entry;
        }

        // Visits every source of one type, or only Only's; a null cache leaves foliage out for the change scan that diffs it per entity.
        template<typename TComponent, typename FnEmit>
        void ForEachNavSource(SNavMeshSystem::FSourceQueries::TSource<TComponent>& Query, const ECS::FEntity* Only, FnEmit&& Emit)
        {
            if (Only != nullptr)
            {
                const TComponent* Component = Query.template TryGet<TComponent>(*Only);
                const STransformComponent* Transform = Component != nullptr ? Query.template TryGet<STransformComponent>(*Only) : nullptr;
                if (Transform != nullptr)
                {
                    Emit(*Only, *Component, *Transform);
                }
                return;
            }

            Query.ForEach([&](ECS::FEntity E, const TComponent& Component, const STransformComponent& Transform)
            {
                Emit(E, Component, Transform);
            });
        }

        template<typename FnEmit>
        void ForEachFoliageSource(SNavMeshSystem::FSourceQueries& Queries, const ECS::FEntity* Only, FnEmit&& Emit)
        {
            if (Only != nullptr)
            {
                if (const SFoliageComponent* Foliage = Queries.Foliage.TryGet<SFoliageComponent>(*Only))
                {
                    Emit(*Only, *Foliage);
                }
                return;
            }

            Queries.Foliage.ForEach([&](ECS::FEntity E, const SFoliageComponent& Foliage)
            {
                Emit(E, Foliage);
            });
        }

        void CollectNavSources(SNavMeshSystem::FSourceQueries& Queries, const FVector3& BakeMin, const FVector3& BakeMax, TVector<FNavSourceEntry>& Out, FNavFoliageCache* FoliageCache, const ECS::FEntity* Only = nullptr)
        {
            // A simulated body is loose debris rather than level geometry, and baking it would rebuild tiles every time it settles. Concave colliders never simulate.
            auto IsSimulated = [&Queries](ECS::FEntity Entity)
            {
                const SRigidBodyComponent* Body = Queries.RigidBodies.TryGet<SRigidBodyComponent>(Entity);
                return Body != nullptr && Body->BodyType == EBodyType::Dynamic;
            };

            ForEachNavSource<SBoxColliderComponent>(Queries.Boxes, Only, [&](ECS::FEntity E, const SBoxColliderComponent& Box, const STransformComponent& X)
            {
                if (!Box.bAffectsNavigation || IsSimulated(E)) return;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Box);
                Entry.Prim.Type = ENavColliderType::Box;
                Entry.Prim.World = ColliderToWorld(X, Box.TranslationOffset, Box.RotationOffset);
                Entry.Prim.Shape = Box.HalfExtent;
                const FVector3 H = Box.HalfExtent;
                const FVector3 Corners[8] = {
                    {-H.x,-H.y,-H.z}, { H.x,-H.y,-H.z}, { H.x,-H.y, H.z}, {-H.x,-H.y, H.z},
                    {-H.x, H.y,-H.z}, { H.x, H.y,-H.z}, { H.x, H.y, H.z}, {-H.x, H.y, H.z},
                };
                CornersAABB(Entry.Prim.World, Corners, 8, Entry.AABBMin, Entry.AABBMax);
                Out.Add(std::move(Entry));
            });

            ForEachNavSource<SSphereColliderComponent>(Queries.Spheres, Only, [&](ECS::FEntity E, const SSphereColliderComponent& Sphere, const STransformComponent& X)
            {
                if (!Sphere.bAffectsNavigation || IsSimulated(E)) return;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Sphere);
                Entry.Prim.Type = ENavColliderType::Sphere;
                Entry.Prim.World = ColliderToWorld(X, Sphere.TranslationOffset, FVector3(0.0f));
                Entry.Prim.Shape = FVector3(Sphere.Radius, 0.0f, 0.0f);
                const FVector3 Center = FVector3(Entry.Prim.World * FVector4(0.0f, 0.0f, 0.0f, 1.0f));
                const float R = ScaledRadius(Entry.Prim.World, Sphere.Radius);
                Entry.AABBMin = Center - FVector3(R);
                Entry.AABBMax = Center + FVector3(R);
                Out.Add(std::move(Entry));
            });

            // Painted instances are world space, and erasing one shifts or swaps indices, which the detector sees as a move.
            if (FoliageCache != nullptr)
            {
                ForEachFoliageSource(Queries, Only, [&](ECS::FEntity E, const SFoliageComponent& Foliage)
                {
                    const FNavFoliageEntity& Cached = RefreshFoliageEntity(*FoliageCache, E, Foliage);
                    for (int32 Index = 0; Index < Cached.Min.Num(); ++Index)
                    {
                        const FVector3& Mn = Cached.Min[Index];
                        const FVector3& Mx = Cached.Max[Index];
                        if (!HasFoliageBounds(Mn, Mx) ||
                            Mn.x > BakeMax.x || Mx.x < BakeMin.x || Mn.y > BakeMax.y || Mx.y < BakeMin.y || Mn.z > BakeMax.z || Mx.z < BakeMin.z)
                        {
                            continue;
                        }
    
                        const SFoliageInstance& Instance = Foliage.Instances[Index];
                        const FFoliageNavTemplate& Template = Cached.Templates[Instance.TypeIndex];
                        FNavSourceEntry Entry;
                        Entry.Key = PackSourceKey(E, ENavColliderType::Foliage, Index);
                        Entry.ContentId = Template.ContentId;
                        Entry.Prim.Type = ENavColliderType::Foliage;
                        Entry.Prim.World = Instance.GetMatrix();
                        Entry.Prim.Children = Template.Children;
                        Entry.AABBMin = Mn;
                        Entry.AABBMax = Mx;
                        Out.Add(std::move(Entry));
                    }
                });
            }

            ForEachNavSource<SMeshColliderComponent>(Queries.MeshColliders, Only, [&](ECS::FEntity E, const SMeshColliderComponent& MC, const STransformComponent& X)
            {
                if (!MC.bAffectsNavigation) return;
                const SStaticMeshComponent* Fallback = Queries.StaticMeshes.TryGet<SStaticMeshComponent>(E);
                CStaticMesh* Mesh = ResolveMeshColliderAsset(MC, Fallback);
                if (!Mesh || Mesh->GetMeshResource().bSkinnedMesh) return;
                if (IsSimulated(E) && !Physics::CollisionGen::IsConcaveMeshCollider(*Mesh, MC.bConvex)) return;

                // The mesh's simplified collision replaces its render triangles, as it does for the physics body.
                if (const SMeshCollision* Collision = Mesh->GetCollision())
                {
                    const FMatrix4 ColliderWorld = ColliderToWorld(X, MC.TranslationOffset, MC.RotationOffset);
                    const uint64 AssetContentId = MakeContentId(Collision, Collision->TriangleIndices.Num(), Collision->Primitives.Num());
                    ForEachMeshCollisionPrim(*Collision, ColliderWorld, [&](ENavColliderType KeyType, uint32 SubIndex, FNavSourceEntry&& Entry)
                    {
                        Entry.Key = PackSourceKey(E, KeyType, SubIndex);
                        Entry.ContentId = AssetContentId;
                        Out.Add(std::move(Entry));
                    });
                    return;
                }

                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Mesh);
                Entry.Prim.Type = ENavColliderType::Mesh;
                Entry.Prim.World = ColliderToWorld(X, MC.TranslationOffset, MC.RotationOffset);
                Entry.Prim.Mesh = Mesh;
                const FMeshletData& MeshletData = Mesh->GetMeshResource().MeshletData;
                Entry.ContentId = MakeContentId(Mesh, MeshletData.VertexPositions.Num(), MeshletData.MeshletTriangles.Num());
                const SAABB& Local = Mesh->GetAABB();
                const FVector3 Corners[8] = {
                    {Local.Min.x, Local.Min.y, Local.Min.z}, {Local.Max.x, Local.Min.y, Local.Min.z},
                    {Local.Min.x, Local.Max.y, Local.Min.z}, {Local.Max.x, Local.Max.y, Local.Min.z},
                    {Local.Min.x, Local.Min.y, Local.Max.z}, {Local.Max.x, Local.Min.y, Local.Max.z},
                    {Local.Min.x, Local.Max.y, Local.Max.z}, {Local.Max.x, Local.Max.y, Local.Max.z},
                };
                CornersAABB(Entry.Prim.World, Corners, 8, Entry.AABBMin, Entry.AABBMax);
                Out.Add(std::move(Entry));
            });

            // Fingerprinted on the render-data version so a re-commit with unchanged bounds still dirties the tile.
            ForEachNavSource<SDynamicMeshColliderComponent>(Queries.DynamicMeshColliders, Only, [&](ECS::FEntity E, const SDynamicMeshColliderComponent& DMC, const STransformComponent& X)
            {
                if (!DMC.bAffectsNavigation || (DMC.bConvex && IsSimulated(E))) return;
                const SDynamicMeshComponent* DMFound = Queries.DynamicMeshes.TryGet<SDynamicMeshComponent>(E);
                if (DMFound == nullptr) return;
                const SDynamicMeshComponent& DM = *DMFound;
                TSharedPtr<FDynamicMeshRenderData> MeshData = DM.LoadRenderData();
                if (!MeshData || MeshData->Resource.MeshletData.IsEmpty()) return;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::DynamicMesh);
                Entry.ContentId = DM.LoadRenderDataVersion();
                Entry.Prim.Type = ENavColliderType::DynamicMesh;
                Entry.Prim.World = X.GetWorldMatrix();
                Entry.Prim.DynamicMesh = MeshData;
                const FVector3& Mn = MeshData->LocalMin;
                const FVector3& Mx = MeshData->LocalMax;
                const FVector3 Corners[8] = {
                    {Mn.x, Mn.y, Mn.z}, {Mx.x, Mn.y, Mn.z}, {Mn.x, Mx.y, Mn.z}, {Mx.x, Mx.y, Mn.z},
                    {Mn.x, Mn.y, Mx.z}, {Mx.x, Mn.y, Mx.z}, {Mn.x, Mx.y, Mx.z}, {Mx.x, Mx.y, Mx.z},
                };
                CornersAABB(Entry.Prim.World, Corners, 8, Entry.AABBMin, Entry.AABBMax);
                Out.Add(std::move(Entry));
            });

            ForEachNavSource<SCapsuleColliderComponent>(Queries.Capsules, Only, [&](ECS::FEntity E, const SCapsuleColliderComponent& Cap, const STransformComponent& X)
            {
                if (!Cap.bAffectsNavigation) return;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Capsule);
                Entry.Prim.Type = ENavColliderType::Capsule;
                Entry.Prim.World = ColliderToWorld(X, Cap.TranslationOffset, Cap.RotationOffset);
                Entry.Prim.Shape = FVector3(Cap.Radius, Cap.HalfHeight, 0.0f);
                const FVector3 Top = FVector3(Entry.Prim.World * FVector4(0.0f,  Cap.HalfHeight, 0.0f, 1.0f));
                const FVector3 Bot = FVector3(Entry.Prim.World * FVector4(0.0f, -Cap.HalfHeight, 0.0f, 1.0f));
                const float R = ScaledRadius(Entry.Prim.World, Cap.Radius);
                Entry.AABBMin = Math::Min(Top, Bot) - FVector3(R);
                Entry.AABBMax = Math::Max(Top, Bot) + FVector3(R);
                Out.Add(std::move(Entry));
            });

            ForEachNavSource<SCylinderColliderComponent>(Queries.Cylinders, Only, [&](ECS::FEntity E, const SCylinderColliderComponent& Cyl, const STransformComponent& X)
            {
                if (!Cyl.bAffectsNavigation || IsSimulated(E)) return;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Cylinder);
                Entry.Prim.Type = ENavColliderType::Cylinder;
                Entry.Prim.World = ColliderToWorld(X, Cyl.TranslationOffset, Cyl.RotationOffset);
                Entry.Prim.Shape = FVector3(Cyl.Radius, Cyl.HalfHeight, 0.0f);
                const FVector3 Top = FVector3(Entry.Prim.World * FVector4(0.0f,  Cyl.HalfHeight, 0.0f, 1.0f));
                const FVector3 Bot = FVector3(Entry.Prim.World * FVector4(0.0f, -Cyl.HalfHeight, 0.0f, 1.0f));
                const float R = ScaledRadius(Entry.Prim.World, Cyl.Radius);
                Entry.AABBMin = Math::Min(Top, Bot) - FVector3(R);
                Entry.AABBMax = Math::Max(Top, Bot) + FVector3(R);
                Out.Add(std::move(Entry));
            });

            // Character capsules are opt-in, since agents should not normally carve the navmesh.
            ForEachNavSource<SCharacterPhysicsComponent>(Queries.Characters, Only, [&](ECS::FEntity E, const SCharacterPhysicsComponent& Cap, const STransformComponent& X)
            {
                if (!Cap.bAffectsNavigation) return;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::CharacterCapsule);
                Entry.Prim.Type = ENavColliderType::CharacterCapsule;
                FMatrix4 CapsuleWorld = X.GetWorldMatrix();
                CapsuleWorld[3] = CapsuleWorld * FVector4(Cap.TranslationOffset.x, Cap.TranslationOffset.y, Cap.TranslationOffset.z, 1.0f);
                Entry.Prim.World = CapsuleWorld;
                Entry.Prim.Shape = FVector3(Cap.Radius, Cap.HalfHeight, 0.0f);
                const FVector3 Top = FVector3(Entry.Prim.World * FVector4(0.0f,  Cap.HalfHeight, 0.0f, 1.0f));
                const FVector3 Bot = FVector3(Entry.Prim.World * FVector4(0.0f, -Cap.HalfHeight, 0.0f, 1.0f));
                const float R = ScaledRadius(Entry.Prim.World, Cap.Radius);
                Entry.AABBMin = Math::Min(Top, Bot) - FVector3(R);
                Entry.AABBMax = Math::Max(Top, Bot) + FVector3(R);
                Out.Add(std::move(Entry));
            });

            // A compound body is several primitives under one transform, so each child is its own source.
            ForEachNavSource<SCompoundColliderComponent>(Queries.Compounds, Only, [&](ECS::FEntity E, const SCompoundColliderComponent& Compound, const STransformComponent& X)
            {
                if (!Compound.bAffectsNavigation || IsSimulated(E)) return;

                const FMatrix4 BodyWorld = X.GetWorldMatrix();
                for (int32 i = 0; i < Compound.Shapes.Num(); ++i)
                {
                    const SCompoundSubShape& Child = Compound.Shapes[i];

                    FNavSourceEntry Entry;
                    Entry.Prim.World = BodyWorld * Math::Translate(FMatrix4(1.0f), Child.Offset) * Math::ToMatrix4(FQuat(Child.Rotation));

                    const uint32 SubIndex = CompoundSubIndexBase + i;
                    switch (Child.Type)
                    {
                    case ECompoundShapeType::Sphere:
                        {
                            Entry.Key = PackSourceKey(E, ENavColliderType::Sphere, SubIndex);
                            Entry.Prim.Type = ENavColliderType::Sphere;
                            Entry.Prim.Shape = FVector3(Child.Radius, 0.0f, 0.0f);

                            const FVector3 Center = FVector3(Entry.Prim.World * FVector4(0.0f, 0.0f, 0.0f, 1.0f));
                            const float R = ScaledRadius(Entry.Prim.World, Child.Radius);
                            Entry.AABBMin = Center - FVector3(R);
                            Entry.AABBMax = Center + FVector3(R);
                        }
                        break;

                    case ECompoundShapeType::Capsule:
                    case ECompoundShapeType::Cylinder:
                        {
                            const bool bCapsule = Child.Type == ECompoundShapeType::Capsule;
                            Entry.Key = PackSourceKey(E, bCapsule ? ENavColliderType::Capsule : ENavColliderType::Cylinder, SubIndex);
                            Entry.Prim.Type = bCapsule ? ENavColliderType::Capsule : ENavColliderType::Cylinder;
                            Entry.Prim.Shape = FVector3(Child.Radius, Child.HalfHeight, 0.0f);

                            const FVector3 Top = FVector3(Entry.Prim.World * FVector4(0.0f,  Child.HalfHeight, 0.0f, 1.0f));
                            const FVector3 Bot = FVector3(Entry.Prim.World * FVector4(0.0f, -Child.HalfHeight, 0.0f, 1.0f));
                            const float R = ScaledRadius(Entry.Prim.World, Child.Radius);
                            Entry.AABBMin = Math::Min(Top, Bot) - FVector3(R);
                            Entry.AABBMax = Math::Max(Top, Bot) + FVector3(R);
                        }
                        break;

                    default:
                        {
                            Entry.Key = PackSourceKey(E, ENavColliderType::Box, SubIndex);
                            Entry.Prim.Type = ENavColliderType::Box;
                            Entry.Prim.Shape = Child.HalfExtent;

                            const FVector3 H = Child.HalfExtent;
                            const FVector3 Corners[8] = {
                                {-H.x,-H.y,-H.z}, { H.x,-H.y,-H.z}, { H.x,-H.y, H.z}, {-H.x,-H.y, H.z},
                                {-H.x, H.y,-H.z}, { H.x, H.y,-H.z}, { H.x, H.y, H.z}, {-H.x, H.y, H.z},
                            };
                            CornersAABB(Entry.Prim.World, Corners, 8, Entry.AABBMin, Entry.AABBMax);
                        }
                        break;
                    }

                    Out.Add(std::move(Entry));
                }
            });

            // Area modifiers carry no triangles; the bake stamps their footprint onto the voxelized surface.
            ForEachNavSource<SNavModifierComponent>(Queries.Modifiers, Only, [&](ECS::FEntity E, const SNavModifierComponent& Modifier, const STransformComponent& X)
            {
                if (!Modifier.bEnabled) return;
                if (Modifier.Extents.x <= 0.0f || Modifier.Extents.y <= 0.0f || Modifier.Extents.z <= 0.0f) return;

                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::AreaVolume, (uint32)Modifier.Area);
                Entry.Prim.Type = ENavColliderType::AreaVolume;
                Entry.Prim.World = X.GetWorldMatrix() * Math::Translate(FMatrix4(1.0f), Modifier.Offset);

                const FVector3 H = Modifier.Extents;
                const FVector3 LocalCorners[8] = {
                    {-H.x,-H.y,-H.z}, { H.x,-H.y,-H.z}, { H.x,-H.y, H.z}, {-H.x,-H.y, H.z},
                    {-H.x, H.y,-H.z}, { H.x, H.y,-H.z}, { H.x, H.y, H.z}, {-H.x, H.y, H.z},
                };
                FVector3 WorldCorners[8];
                for (int32 i = 0; i < 8; ++i)
                {
                    WorldCorners[i] = FVector3(Entry.Prim.World * FVector4(LocalCorners[i], 1.0f));
                    Entry.AABBMin = Math::Min(Entry.AABBMin, WorldCorners[i]);
                    Entry.AABBMax = Math::Max(Entry.AABBMax, WorldCorners[i]);
                }

                auto Volume = MakeShared<FNavAreaVolume>();
                BuildXZHull(WorldCorners, 8, Volume->Hull);
                Volume->MinY = Entry.AABBMin.y;
                Volume->MaxY = Entry.AABBMax.y;
                Volume->Area = (uint8)Modifier.Area;
                Entry.Prim.AreaVolume = std::move(Volume);
                Out.Add(std::move(Entry));
            });

            // Off-mesh links are stitched in by the bake; each endpoint has to land on walkable surface.
            ForEachNavSource<SNavLinkComponent>(Queries.Links, Only, [&](ECS::FEntity E, const SNavLinkComponent& LinkComp, const STransformComponent& X)
            {
                if (!LinkComp.bEnabled) return;

                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::OffMeshLink, PackLinkSubIndex(LinkComp));
                Entry.Prim.Type = ENavColliderType::OffMeshLink;
                Entry.Prim.World = X.GetWorldMatrix();

                auto Link = MakeShared<FNavOffMeshLink>();
                Link->Start = FVector3(Entry.Prim.World * FVector4(LinkComp.Start, 1.0f));
                Link->End   = FVector3(Entry.Prim.World * FVector4(LinkComp.End,   1.0f));
                Link->Radius = ScaledRadius(Entry.Prim.World, LinkComp.Radius);
                Link->bBidirectional = LinkComp.bBidirectional;
                Link->Area   = (uint8)LinkComp.Area;
                Link->Flags  = (uint16)LinkComp.Flag;
                Link->UserId = (uint32)E;

                const FVector3 Pad(Link->Radius);
                Entry.AABBMin = Math::Min(Link->Start, Link->End) - Pad;
                Entry.AABBMax = Math::Max(Link->Start, Link->End) + Pad;
                Entry.Prim.Link = std::move(Link);
                Out.Add(std::move(Entry));
            });

            // Terrain heightfield needs both the collider and the source component.
            ForEachNavSource<STerrainColliderComponent>(Queries.TerrainColliders, Only, [&](ECS::FEntity E, const STerrainColliderComponent& TC, const STransformComponent& X)
            {
                if (!TC.bAffectsNavigation) return;
                const STerrainComponent* Terrain = Queries.Terrains.TryGet<STerrainComponent>(E);
                if (!Terrain || Terrain->Heightmap.IsEmpty()) return;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Terrain);
                Entry.ContentId = Terrain->CPUState.HeightmapVersion;
                Entry.Prim.Type = ENavColliderType::Terrain;
                Entry.Prim.World = X.GetWorldMatrix();
                const float Half = Terrain->TileWorldSize * 0.5f;
                const FVector3 Corners[8] = {
                    {-Half, 0.0f, -Half}, { Half, 0.0f, -Half}, { Half, 0.0f,  Half}, {-Half, 0.0f,  Half},
                    {-Half, Terrain->MaxHeight, -Half}, { Half, Terrain->MaxHeight, -Half}, { Half, Terrain->MaxHeight,  Half}, {-Half, Terrain->MaxHeight,  Half},
                };
                CornersAABB(Entry.Prim.World, Corners, 8, Entry.AABBMin, Entry.AABBMax);
                Out.Add(std::move(Entry));
            });

            // Without this the caller's box only culls per triangle, after emit has already transformed geometry it is about to throw away.
            Out.RemoveAll([&BakeMin, &BakeMax](const FNavSourceEntry& Entry)
            {
                return Entry.AABBMin.x > BakeMax.x || Entry.AABBMax.x < BakeMin.x
                    || Entry.AABBMin.y > BakeMax.y || Entry.AABBMax.y < BakeMin.y
                    || Entry.AABBMin.z > BakeMax.z || Entry.AABBMax.z < BakeMin.z;
            });
        }


        constexpr float kUnbounded = 1.0e30f;

        FORCEINLINE ECS::FEntity EntityOfSourceKey(uint64 Key)
        {
            return static_cast<ECS::FEntity>((uint32)(Key >> 8));
        }
    }

    struct FPolledVersion
    {
        ECS::FEntity Entity;
        uint32       Version = 0;
    };

    // Every nav source in the world in a loose octree, so a tile gathers what lies under it and a change touches only what moved.
    struct FNavSourceTracker
    {
        FNavOctree                              Octree;
        TVector<NavMeshSystemInternal::FNavSourceEntry> Records;
        TVector<uint32>                         RecordElement;
        TVector<uint32>                         FreeRecords;
        THashMap<uint64, uint32>                RecordOfKey;
        THashMap<ECS::FEntity, TVector<uint64>> KeysOfEntity;
        FNavFoliageCache                        Foliage;

        // Raised by component signals, which a parallel script's patch can fire off the game thread.
        FMutex                                  PendingLock;
        THashSet<ECS::FEntity>                  Pending;
        THashSet<ECS::FEntity>                  WorkScratch;

        TVector<ECS::FEntity>                   MovedScratch;
        TVector<TVector<ECS::FEntity>>          PollBuckets;
        // Last version polled per dense slot of each pool, so an unchanged source costs no record lookup.
        TVector<FPolledVersion>                 PolledTerrain;
        TVector<FPolledVersion>                 PolledDynamicMeshes;
        TVector<FPolledVersion>                 PolledFoliage;
        TVector<NavMeshSystemInternal::FNavSourceEntry> EntryScratch;
        TVector<ECS::FEntity>                   SweepOrder;
        int32                                   SweepCursor = 0;
        bool                                    bSeeded = false;

        // Footprints that changed this tick, the old one and the new, which every volume turns into dirty tiles.
        TVector<FNavSourceBounds>               Changes;
        int32                                   Counts[4] = {};
        int32                                   Refreshed = 0;

        // Last, so it joins its threads before anything a queued rebake might still read goes away.
        TUniquePtr<FNavBuildPool>               BuildPool;
    };

    namespace NavMeshSystemInternal
    {
        void MarkSourcePending(FNavSourceTracker& Sources, ECS::FEntity Entity)
        {
            FScopeLock Lock(Sources.PendingLock);
            Sources.Pending.Add(Entity);
        }

        template<typename TComponent>
        void WatchSourceComponent(ECS::FRegistry& Registry, FNavSourceTracker* Sources, bool bWatch)
        {
            ECS::FComponentSignals& Signals = Registry.GetSignals<TComponent>();
            for (ECS::FComponentSignal* Signal : { &Signals.OnConstruct, &Signals.OnDestroy, &Signals.OnUpdate })
            {
                if (bWatch)
                {
                    Signal->template Connect<&MarkSourcePending>(Sources);
                }
                else
                {
                    Signal->template Disconnect<&MarkSourcePending>(Sources);
                }
            }
        }

        void WatchSourceComponents(ECS::FRegistry& Registry, FNavSourceTracker* Sources, bool bWatch)
        {
            [&]<typename... TComponents>(ECS::TTypeList<TComponents...>)
            {
                (WatchSourceComponent<TComponents>(Registry, Sources, bWatch), ...);
            }(ECS::TTypeList<SBoxColliderComponent, SSphereColliderComponent, SMeshColliderComponent, SCapsuleColliderComponent, SCylinderColliderComponent,
                SCharacterPhysicsComponent, STerrainColliderComponent, STerrainComponent, SStaticMeshComponent, SDynamicMeshColliderComponent, SDynamicMeshComponent,
                SCompoundColliderComponent, SNavModifierComponent, SNavLinkComponent, SFoliageComponent, SRigidBodyComponent>{});
        }

        void AddSourceRecord(FNavSourceTracker& Sources, FNavSourceEntry&& Entry)
        {
            uint32 Index;
            if (!Sources.FreeRecords.IsEmpty())
            {
                Index = Sources.FreeRecords.Last();
                Sources.FreeRecords.Pop();
            }
            else
            {
                Index = (uint32)Sources.Records.Num();
                Sources.Records.Emplace();
                Sources.RecordElement.Add(FNavOctree::kNone);
            }
            Sources.RecordOfKey[Entry.Key] = Index;
            Sources.RecordElement[Index] = Sources.Octree.Add(Entry.AABBMin, Entry.AABBMax, Index);
            Sources.Records[Index] = std::move(Entry);
        }

        void RemoveSourceRecord(FNavSourceTracker& Sources, uint32 Index)
        {
            Sources.Octree.Remove(Sources.RecordElement[Index]);
            Sources.RecordOfKey.Remove(Sources.Records[Index].Key);
            Sources.Records[Index] = FNavSourceEntry{};
            Sources.RecordElement[Index] = FNavOctree::kNone;
            Sources.FreeRecords.Add(Index);
        }

        void RebuildSweepOrder(FNavSourceTracker& Sources)
        {
            Sources.SweepOrder.Reset();
            Sources.SweepOrder.Reserve(Sources.KeysOfEntity.Num());
            for (const auto& [Entity, Keys] : Sources.KeysOfEntity)
            {
                Sources.SweepOrder.Add(Entity);
            }
            Sources.SweepCursor = 0;
        }

        void SeedSources(SNavMeshSystem::FSourceQueries& Queries, FNavSourceTracker& Sources)
        {
            TVector<FNavSourceEntry> All;
            CollectNavSources(Queries, FVector3(-kUnbounded), FVector3(kUnbounded), All, &Sources.Foliage);
            Sources.Records.Reserve(All.Num());
            Sources.RecordElement.Reserve(All.Num());
            for (FNavSourceEntry& Entry : All)
            {
                const ECS::FEntity Owner = EntityOfSourceKey(Entry.Key);
                Sources.KeysOfEntity[Owner].Add(Entry.Key);
                Queries.Moved.Watch(Owner, true);
                AddSourceRecord(Sources, std::move(Entry));
            }
            RebuildSweepOrder(Sources);
            Sources.bSeeded = true;
        }

        // Diffs what the entity contributes now against what was recorded, and logs the footprint of anything that changed.
        void RefreshSourceEntity(const SSystemContext& Context, SNavMeshSystem::FSourceQueries& Queries, FNavSourceTracker& Sources, ECS::FEntity Entity)
        {
            ++Sources.Refreshed;
            TVector<FNavSourceEntry>& Current = Sources.EntryScratch;
            Current.Reset();
            const bool bValid = Context.IsValidEntity(Entity);
            if (bValid)
            {
                CollectNavSources(Queries, FVector3(-kUnbounded), FVector3(kUnbounded), Current, &Sources.Foliage, &Entity);
            }
            if (!bValid || Queries.Foliage.TryGet<SFoliageComponent>(Entity) == nullptr)
            {
                Sources.Foliage.Entities.Remove((uint32)Entity);
            }

            auto KeysIt = Sources.KeysOfEntity.Find(Entity);
            TVector<uint64> Previous = KeysIt != nullptr ? std::move(*KeysIt) : TVector<uint64>();

            TVector<uint64> Kept;
            Kept.Reserve(Current.Num());
            for (FNavSourceEntry& Entry : Current)
            {
                Kept.Add(Entry.Key);
                auto Found = Sources.RecordOfKey.Find(Entry.Key);
                if (Found == nullptr)
                {
                    ++Sources.Counts[0];
                    Sources.Changes.Emplace(Entry.AABBMin, Entry.AABBMax, !IsAnnotation(Entry.Prim.Type));
                    AddSourceRecord(Sources, std::move(Entry));
                    continue;
                }

                FNavSourceEntry& Record = Sources.Records[(*Found)];
                const bool bMoved = !Math::IsNearlyEqual(Record.AABBMin, Entry.AABBMin) || !Math::IsNearlyEqual(Record.AABBMax, Entry.AABBMax);

                // A re-imported, swapped or sculpted mesh keeps its bounds, so the box alone misses it.
                const bool bReshaped = Record.ContentId != Entry.ContentId;
                if (bMoved || bReshaped)
                {
                    ++Sources.Counts[bMoved ? 1 : 2];
                    Sources.Changes.Emplace(Record.AABBMin, Record.AABBMax, !IsAnnotation(Record.Prim.Type));
                    Sources.Changes.Emplace(Entry.AABBMin, Entry.AABBMax, !IsAnnotation(Entry.Prim.Type));
                    Sources.Octree.Update(Sources.RecordElement[(*Found)], Entry.AABBMin, Entry.AABBMax);
                }
                Record = std::move(Entry);
            }

            // A foliage entity keeps tens of thousands of keys, so the vanished ones are found with a set rather than a scan.
            THashSet<uint64> KeptSet;
            const bool bUseSet = Kept.Num() > 32;
            if (bUseSet)
            {
                KeptSet.Append(Kept);
            }
            for (uint64 Key : Previous)
            {
                const bool bStillThere = bUseSet ? KeptSet.Contains(Key) : Algo::Contains(Kept, Key);
                auto Found = bStillThere ? nullptr : Sources.RecordOfKey.Find(Key);
                if (Found != nullptr)
                {
                    ++Sources.Counts[3];
                    const FNavSourceEntry& Record = Sources.Records[(*Found)];
                    Sources.Changes.Emplace(Record.AABBMin, Record.AABBMax, !IsAnnotation(Record.Prim.Type));
                    RemoveSourceRecord(Sources, (*Found));
                }
            }

            if (Kept.IsEmpty())
            {
                if (KeysIt != nullptr)
                {
                    Sources.KeysOfEntity.Remove(Entity);
                }
                Queries.Moved.Watch(Entity, false);
                return;
            }
            if (KeysIt == nullptr)
            {
                Sources.SweepOrder.Add(Entity);
            }
            Sources.KeysOfEntity[Entity] = std::move(Kept);
            Queries.Moved.Watch(Entity, true);
        }

        // Spread over the workers since a streamed world carries tens of thousands, and only a version not seen last poll reaches IsStale.
        template<typename T, typename FnVersion, typename FnStale>
        void QueueStaleSources(const ECS::TQuery<ECS::Read<T>>& Pool, FNavSourceTracker& Sources, TVector<FPolledVersion>& Polled, THashSet<ECS::FEntity>& Work,
                               FnVersion&& Version, FnStale&& IsStale)
        {
            const auto View = Pool.View();
            const TSpan<const ECS::FEntity> DenseSlots = View.GetDenseEntities();
            if (DenseSlots.IsEmpty())
            {
                Polled.Reset();
                return;
            }

            const uint32 Num = (uint32)DenseSlots.Num();
            Polled.SetNum(Num);

            const uint32 NumBuckets = Math::Max(1u, Jobs::IsRuntimeRunning() ? Jobs::GetNumThreadSlots() : 1u);
            if ((uint32)Sources.PollBuckets.Num() < NumBuckets)
            {
                Sources.PollBuckets.SetNum(NumBuckets);
            }

            constexpr uint32 MinSlotsToSplit = 4096;
            Jobs::ParallelForOrSerial(Num, MinSlotsToSplit, [&](const Jobs::FParallelRange& Range)
            {
                TVector<ECS::FEntity>& Bucket = Sources.PollBuckets[Range.Thread];
                const ECS::FEntity* Dense = DenseSlots.GetData();
                for (uint32 i = Range.Start; i < Range.End; ++i)
                {
                    const ECS::FEntity E = Dense[i];
                    if (E.IsTombstone())
                    {
                        continue;
                    }
                    const T& Component = View.template Get<ECS::Read<T>>(E);
                    uint32 Current = 0;
                    if (!Version(E, Component, Current))
                    {
                        continue;
                    }

                    FPolledVersion& Seen = Polled[i];
                    if (Seen.Entity == E && Seen.Version == Current)
                    {
                        continue;
                    }
                    if (IsStale(E, Component, Current))
                    {
                        Bucket.Add(E);
                    }
                    Seen = FPolledVersion{ E, Current };
                }
            }, 1024);

            for (TVector<ECS::FEntity>& Bucket : Sources.PollBuckets)
            {
                Work.Append(Bucket);
                Bucket.Reset();
            }
        }

        // Content that changes without a move or a signal, a sculpted heightmap or a re-committed dynamic mesh, polled by version.
        void QueueVersionedSources(SNavMeshSystem::FSourceQueries& Queries, FNavSourceTracker& Sources, THashSet<ECS::FEntity>& Work)
        {
            auto RecordedContent = [&Sources](uint64 Key, uint64 ContentId)
            {
                auto Found = Sources.RecordOfKey.Find(Key);
                return Found == nullptr || Sources.Records[(*Found)].ContentId != ContentId;
            };

            QueueStaleSources<STerrainColliderComponent>(Queries.TerrainPool, Sources, Sources.PolledTerrain, Work,
                [&](ECS::FEntity E, const STerrainColliderComponent&, uint32& Out)
                {
                    const STerrainComponent* Terrain = Queries.Terrains.TryGet<STerrainComponent>(E);
                    Out = Terrain != nullptr ? Terrain->CPUState.HeightmapVersion : 0u;
                    return Terrain != nullptr;
                },
                [&](ECS::FEntity E, const STerrainColliderComponent&, uint32 Current)
                {
                    return RecordedContent(PackSourceKey(E, ENavColliderType::Terrain), Current);
                });

            QueueStaleSources<SDynamicMeshColliderComponent>(Queries.DynamicMeshPool, Sources, Sources.PolledDynamicMeshes, Work,
                [&](ECS::FEntity E, const SDynamicMeshColliderComponent&, uint32& Out)
                {
                    const SDynamicMeshComponent* Mesh = Queries.DynamicMeshes.TryGet<SDynamicMeshComponent>(E);
                    Out = Mesh != nullptr ? Mesh->LoadRenderDataVersion() : 0u;
                    return Mesh != nullptr;
                },
                [&](ECS::FEntity E, const SDynamicMeshColliderComponent&, uint32 Current)
                {
                    return RecordedContent(PackSourceKey(E, ENavColliderType::DynamicMesh), Current);
                });

            // Gated on the instance version, so a re-imported species shape with no instance edit waits for the sweep.
            QueueStaleSources<SFoliageComponent>(Queries.Foliage, Sources, Sources.PolledFoliage, Work,
                [](ECS::FEntity, const SFoliageComponent& Foliage, uint32& Out)
                {
                    Out = Foliage.InstancesVersion;
                    return true;
                },
                [&](ECS::FEntity E, const SFoliageComponent& Foliage, uint32)
                {
                    auto Cached = Sources.Foliage.Entities.Find((uint32)E);
                    return Cached == nullptr || Cached->Signature != FoliageNavSignature(Foliage);
                });
        }

        void UpdateSources(const SSystemContext& Context, SNavMeshSystem::FSourceQueries& Queries, FNavSourceTracker& Sources)
        {
            LUMINA_PROFILE_SCOPE();
            Sources.Changes.Reset();
            Sources.Refreshed = 0;
            for (int32& Count : Sources.Counts)
            {
                Count = 0;
            }

            if (!Sources.bSeeded)
            {
                SeedSources(Queries, Sources);
                return;
            }

            // Characters and projectiles move every frame, so only entities that already contribute a source are worth a look.
            Sources.MovedScratch.Reset();
            Queries.Moved.Drain(Sources.MovedScratch);

            THashSet<ECS::FEntity>& Work = Sources.WorkScratch;
            Work.Reset();
            {
                FScopeLock Lock(Sources.PendingLock);
                swap(Work, Sources.Pending);
            }
            for (ECS::FEntity Moved : Sources.MovedScratch)
            {
                if (Sources.KeysOfEntity.Contains(Moved))
                {
                    Work.Add(Moved);
                }
            }

            QueueVersionedSources(Queries, Sources, Work);

            // A collider written in place raises no signal, so a slice is checked each tick, counted in sources since one foliage entity carries thousands.
            int32 Remaining = Math::Max(0, NavSettings().SourceVerifyBudget);
            while (Remaining > 0 && !Sources.SweepOrder.IsEmpty())
            {
                if (Sources.SweepCursor >= Sources.SweepOrder.Num())
                {
                    RebuildSweepOrder(Sources);
                    // Every source may have unloaded since the last pass, which leaves nothing to sweep.
                    if (Sources.SweepOrder.IsEmpty())
                    {
                        break;
                    }
                }
                const ECS::FEntity Entity = Sources.SweepOrder[Sources.SweepCursor++];
                auto Keys = Sources.KeysOfEntity.Find(Entity);
                Remaining -= Keys != nullptr ? Math::Max(1, Keys->Num()) : 1;
                Work.Add(Entity);
            }

            for (ECS::FEntity Entity : Work)
            {
                RefreshSourceEntity(Context, Queries, Sources, Entity);
            }
        }

        // What lies under a box, read from the index, with terrain tessellated only inside it.
        void GatherSourcePrims(SNavMeshSystem::FSourceQueries& Queries, const FNavSourceTracker& Sources, const FVector3& Min, const FVector3& Max, float CellSize, TVector<FNavSourcePrim>& Out,
                               bool bAnnotationsOnly = false)
        {
            Sources.Octree.Query(Min, Max, [&](uint32 Index, const FVector3&, const FVector3&)
            {
                const FNavSourceEntry& Record = Sources.Records[Index];
                if (bAnnotationsOnly && !IsAnnotation(Record.Prim.Type))
                {
                    return;
                }
                if (Record.Prim.Type != ENavColliderType::Terrain)
                {
                    Out.Add(Record.Prim);
                    return;
                }

                const STerrainComponent* Terrain = Queries.Terrains.TryGet<STerrainComponent>(EntityOfSourceKey(Record.Key));
                if (Terrain == nullptr || Terrain->Heightmap.IsEmpty())
                {
                    return;
                }
                FNavSourcePrim& Prim = Out.EmplaceGetRef(Record.Prim);
                Prim.TerrainPatch = CaptureTerrainPatch(*Terrain, Prim.World, Min, Max, CellSize);
            });
        }

        FORCEINLINE void TilesForAABB(const FVector3& AABBMin, const FVector3& AABBMax, const FVector3& Origin, float TileWorldSize, int32 TilesX, int32 TilesY,
                                      int32& OutTX0, int32& OutTY0, int32& OutTX1, int32& OutTY1)
        {
            OutTX0 = Math::Clamp((int32)std::floor((AABBMin.x - Origin.x) / TileWorldSize), 0, TilesX - 1);
            OutTY0 = Math::Clamp((int32)std::floor((AABBMin.z - Origin.z) / TileWorldSize), 0, TilesY - 1);
            OutTX1 = Math::Clamp((int32)std::floor((AABBMax.x - Origin.x) / TileWorldSize), 0, TilesX - 1);
            OutTY1 = Math::Clamp((int32)std::floor((AABBMax.z - Origin.z) / TileWorldSize), 0, TilesY - 1);
        }

        FORCEINLINE uint64 PackTileKey(int32 TX, int32 TY) { return ((uint64)(uint32)TY << 32) | (uint32)TX; }

        // One walk feeds geometry and annotations, so a full bake and a hot rebake see the same authored set.
        void SnapshotBuildInput(SNavMeshSystem::FSourceQueries& Queries, const FNavSourceTracker& Sources, SNavMeshComponent& Comp, FNavBuildInput& Out, TVector<FNavSourcePrim>& OutPrims)
        {
            Out.Settings  = Comp.Settings;
            Out.BoundsMin = Comp.Center - Comp.GetWorldExtents();
            Out.BoundsMax = Comp.Center + Comp.GetWorldExtents();

            GatherSourcePrims(Queries, Sources, Out.BoundsMin, Out.BoundsMax, Comp.Settings.CellSize, OutPrims);
            for (const FNavSourcePrim& Prim : OutPrims)
            {
                AppendAnnotation(Prim, Out.AreaVolumes, Out.Links);
            }
        }

        // Kicks an async bake; a no-op if one is already in flight.
        void RequestBake(SNavMeshSystem::FSourceQueries& Queries, const FNavSourceTracker& Sources, SNavMeshComponent& Comp)
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

            // The snapshot reads the registry so it stays here, while emitting its triangles moves to the bake worker.
            FNavBuildInput Input;
            TVector<FNavSourcePrim> Prims;
            SnapshotBuildInput(Queries, Sources, Comp, Input, Prims);

            // EntityAABB cache populated at bake-completion drain (avoids tight-vs-conservative AABB mismatch storm).
            Comp.Runtime.PendingSourceChanges.Reset();
            Comp.Runtime.DirtyTiles.Reset();
            Comp.Runtime.SettlingTiles.Reset();
            Comp.Runtime.PendingRebakes.Reset();
            Comp.Runtime.TileCache.reset();
            Comp.Runtime.ActiveBake = NavMeshBuilder::Bake(std::move(Input), [Prims = std::move(Prims)](FNavBuildInput& In) mutable
            {
                TessellateTerrainPatches(Prims);
                EmitNavSourcePrims(Prims.GetData(), Prims.Num(), In.BoundsMin, In.BoundsMax, In.Vertices, In.Indices);
                if (In.Vertices.IsEmpty() || In.Indices.IsEmpty())
                {
                    LOG_WARN("NavMesh bake starting with no source geometry inside bounds. The result will be an empty navmesh; check that static meshes (non-character-controller) overlap the bounds volume.");
                }
                else
                {
                    LOG_INFO("NavMesh bake starting: {} verts, {} tris, bounds=({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f}).",
                        In.Vertices.Num(), In.Indices.Num() / 3,
                        In.BoundsMin.x, In.BoundsMin.y, In.BoundsMin.z,
                        In.BoundsMax.x, In.BoundsMax.y, In.BoundsMax.z);
                }
            });
            Comp.Runtime.State = ENavBakeState::Building;
        }

        void TickComponent(const SSystemContext& Context, SNavMeshSystem::FSourceQueries& Queries, const FSignificanceState& SignificanceState,
                           SNavMeshComponent& Comp, const STransformComponent* Transform, FNavSourceTracker& Sources)
        {
            // Held until the mesh can take them, so a change made while it baked or hydrated still rebakes its tiles.
            if (Comp.bDynamicRebuild && !Sources.Changes.IsEmpty())
            {
                const FVector3 VolumeMin = Comp.Center - Comp.GetWorldExtents();
                const FVector3 VolumeMax = Comp.Center + Comp.GetWorldExtents();
                for (const FNavSourceBounds& Change : Sources.Changes)
                {
                    if (Change.Min.x <= VolumeMax.x && Change.Max.x >= VolumeMin.x && Change.Min.z <= VolumeMax.z && Change.Max.z >= VolumeMin.z)
                    {
                        Comp.Runtime.PendingSourceChanges.Add(Change);
                    }
                }
            }

            // Scale always mirrors, location only in the editor, since runtime keeps the serialized Center.
            if (Transform)
            {
                const FTransform& WT = Transform->GetWorldTransform();
                const FVector3 WS = WT.GetScale();
                Comp.Runtime.WorldScale = FVector3(std::fabs(WS.x), std::fabs(WS.y), std::fabs(WS.z));
                if (Context.GetWorldType() == EWorldType::Editor)
                {
                    Comp.Center = WT.GetLocation();
                }
            }

            // A play world hydrates the editor bake; its geometry streams in after load, so a bake there is empty.
            const bool bAutoBakeHere = Context.GetWorldType() == EWorldType::Editor || !Comp.HasBakedData();
            if (Comp.bAutoBake && bAutoBakeHere)
            {
                FNavMeshRuntime& RT = Comp.Runtime;
                const FVector3 WExt = Comp.GetWorldExtents();

                // A loaded, already-baked component starts in sync, so do not re-bake until something changes.
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
                    std::memcmp(&Comp.Settings, &RT.AutoPrevSettings, sizeof(SNavBuildSettings)) != 0;
                RT.AutoPrevCenter   = Comp.Center;
                RT.AutoPrevExtents  = WExt;
                RT.AutoPrevSettings = Comp.Settings;

                if (bChangedThisTick)
                {
                    RT.AutoSettleTimer = 0.0f;
                }
                else
                {
                    RT.AutoSettleTimer += Context.GetDeltaTime();

                    const bool bDiffersFromBuilt =
                        !RT.bAutoBuiltValid ||
                        !Math::IsNearlyEqual(Comp.Center, RT.AutoBuiltCenter) ||
                        !Math::IsNearlyEqual(WExt, RT.AutoBuiltExtents) ||
                        std::memcmp(&Comp.Settings, &RT.AutoBuiltSettings, sizeof(SNavBuildSettings)) != 0;
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
                RequestBake(Queries, Sources, Comp);
            }

            // Drain finished bake; FNavMesh construction is offloaded via PendingInit.
            if (Comp.Runtime.ActiveBake && Comp.Runtime.ActiveBake->bDone.load(std::memory_order_acquire))
            {
                FNavBuildOutput& Out = Comp.Runtime.ActiveBake->Output;

                // Tally non-empty tiles so a zero-walkable bake is loud (usually bounds miss geometry).
                int32 NonEmptyTiles = 0;
                for (const SNavTileData& T : Out.Tiles)
                {
                    if (!T.Blob.IsEmpty()) ++NonEmptyTiles;
                }

                Comp.Tiles           = std::move(Out.Tiles);
                Comp.Origin          = Out.Origin;
                Comp.TileWorldSize   = Out.TileWorldSize;
                Comp.MaxPolysPerTile = Out.MaxPolysPerTile;
                Comp.TilesX          = Out.TilesX;
                Comp.TilesY          = Out.TilesY;
                Comp.Runtime.LiveLayout = Out;
                Comp.Runtime.LiveLayout.Tiles.Reset();
                Comp.Runtime.ActiveBake.reset();
                Comp.Runtime.DirtyTiles.Reset();
                Comp.Runtime.SettlingTiles.Reset();

                if (NonEmptyTiles == 0)
                {
                    // Fail explicitly, since leaving State=Building here read as stuck on Baking.
                    LOG_WARN("NavMesh bake produced no walkable tiles ({} total). Check the bounds overlap source geometry, and that the volume/scale isn't so large the bake was capped.", Comp.Tiles.Num());
                    Comp.Runtime.bRuntimeDirty = false;
                    RetireNavMesh(Comp.Runtime);
                    Comp.Runtime.State = ENavBakeState::Failed;
                }
                else
                {
                    LOG_INFO("NavMesh bake complete: {}/{} tiles walkable, origin=({:.2f}, {:.2f}, {:.2f}), tileSize={:.2f}.",
                        NonEmptyTiles, Comp.Tiles.Num(), Comp.Origin.x, Comp.Origin.y, Comp.Origin.z, Comp.TileWorldSize);
                    Comp.Runtime.bRuntimeDirty = true;
                }
            }

            // Drain finished async hydration.
            if (Comp.Runtime.PendingInit && Comp.Runtime.PendingInit->bDone.load(std::memory_order_acquire))
            {
                TUniquePtr<FNavMesh> Hydrated = std::move(Comp.Runtime.PendingInit->ResultMesh);
                Comp.Runtime.PendingInit.reset();
                RetireNavMesh(Comp.Runtime);
                if (Hydrated)
                {
                    Hydrated->ContinueTopologyEpochFrom(Comp.Runtime.RetiredEpoch);
                }
                Comp.Runtime.Mesh = std::move(Hydrated);
                // dtNavMesh::init or addTile can fail; without this branch state silently goes Ready.
                if (!Comp.Runtime.Mesh || !Comp.Runtime.Mesh->IsReady())
                {
                    LOG_ERROR("NavMesh hydration failed: dtNavMesh did not initialize (recast vendoring missing, or addTile rejected every blob). All Nav queries will return false.");
                    Comp.Runtime.State = ENavBakeState::Failed;
                    RetireNavMesh(Comp.Runtime);
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

            // bRuntimeDirty is essential or a re-bake leaves the old Mesh stuck Building; the state test stops a failed hydration re-kicking forever.
            const bool bHydrateWanted = Comp.Runtime.bRuntimeDirty
                                     || (!Comp.Runtime.Mesh && Comp.Runtime.State != ENavBakeState::Failed);
            if (Comp.HasBakedData() && !Comp.Runtime.PendingInit && bHydrateWanted)
            {
                // Tiles can persist with every blob empty, and Detour cannot be handed nothing.
                const bool bAnyWalkableTile = Algo::AnyOf(Comp.Tiles,
                    [](const SNavTileData& Tile) { return !Tile.Blob.IsEmpty(); });
                if (!bAnyWalkableTile)
                {
                    LOG_ERROR("NavMesh has {} tiles but none carry walkable geometry, so there is nothing to hydrate. "
                              "Re-bake the volume, and check it covers ground the agent radius and slope accept.",
                        Comp.Tiles.Num());
                    Comp.Runtime.bRuntimeDirty = false;
                    Comp.Runtime.State = ENavBakeState::Failed;
                    return;
                }

                Comp.Runtime.LiveLayout.Origin          = Comp.Origin;
                Comp.Runtime.LiveLayout.TileWorldSize   = Comp.TileWorldSize;
                Comp.Runtime.LiveLayout.TilesX          = Comp.TilesX;
                Comp.Runtime.LiveLayout.TilesY          = Comp.TilesY;
                Comp.Runtime.LiveLayout.MaxTiles        = Comp.Tiles.Num();
                Comp.Runtime.LiveLayout.MaxPolysPerTile = Comp.MaxPolysPerTile;

                auto Job = MakeShared<FNavInitJob>();
                Comp.Runtime.PendingInit = Job;
                Comp.Runtime.bRuntimeDirty = false;
                Comp.Runtime.State = ENavBakeState::Initializing;

                // With streaming on the mesh starts empty and the streamer pages tiles in; otherwise it is seeded with everything.
                const bool bStreaming = NavSettings().StreamLoadRadius > 0.0f;
                const int32 TileCount = Comp.Tiles.Num();
                const int32 InitMaxTiles = bStreaming
                    ? Math::Min(Math::Max(1, NavSettings().ResidentTileBudget), Math::Max(1, TileCount))
                    : Math::Max(1, TileCount);

                // Copy tiles so worker owns its data; Comp.Tiles stays serialized source of truth.
                TVector<SNavTileData> TilesCopy;
                if (!bStreaming)
                {
                    TilesCopy = Comp.Tiles;
                }
                const FVector3 InitOrigin = Comp.Origin;
                const float InitTileSize = Comp.TileWorldSize;
                const int32 InitMaxPolys = Comp.MaxPolysPerTile;

                Comp.Runtime.bStreamedInit = bStreaming;
                Comp.Runtime.Streamer.Reset(Comp.Origin, Comp.TileWorldSize);

                const bool bLogTimings = CVarNavTimings.GetValue();
                Jobs::Spawn(Jobs::EJobPriority::Background, [](TSharedPtr<FNavInitJob> HydrateJob, TVector<SNavTileData> Tiles, FVector3 Origin, float TileSize, int32 MaxTiles, int32 MaxPolys, bool bLog) -> Jobs::TTask<void>
                {
                    const int32 NumHydratedTiles = Tiles.Num();
                    PlatformTime::FStopwatch Watch;
                    auto Mesh = MakeUnique<FNavMesh>();
                    Mesh->Initialize(Origin, TileSize, MaxTiles, MaxPolys, std::move(Tiles));
                    if (bLog)
                    {
                        LOG_INFO("NavTiming hydrate: {} tiles, maxTiles={}, {:.1f} ms.", NumHydratedTiles, MaxTiles, Watch.ElapsedMilliseconds());
                    }
                    HydrateJob->ResultMesh = std::move(Mesh);
                    HydrateJob->bDone.store(true, std::memory_order_release);
                    co_return;
                }, Job, std::move(TilesCopy), InitOrigin, InitTileSize, InitMaxTiles, InitMaxPolys, bLogTimings);

                // Cache left empty, so a source loaded before a serialized bake reads as new and gets its tiles.
                Comp.Runtime.DirtyTiles.Reset();
                Comp.Runtime.SettlingTiles.Reset();
            }

            if (!Comp.Runtime.Mesh || !Comp.Runtime.Mesh->IsReady() || Comp.Runtime.State != ENavBakeState::Ready)
            {
                return;
            }

            // Page tiles around the view before anything reads the mesh this tick.
            {
                const CNavigationSettings& Settings = NavSettings();
                FNavTileStreamer::FSettings Stream;
                Stream.MaxResidentTiles = Math::Max(1, Settings.ResidentTileBudget);
                Stream.LoadRadius       = Settings.StreamLoadRadius;
                Stream.KeepRadiusScale  = Settings.KeepRadiusScale;
                Stream.MaxOpsPerTick    = Math::Max(1, Settings.MaxTileOpsPerTick);

                TVector<FVector3> Focus;
                if (Stream.LoadRadius > 0.0f)
                {
                    if (SignificanceState.bHasView)
                    {
                        Focus.Add(SignificanceState.ViewOrigin);
                    }
                }

                // A mesh seeded with every tile is already resident, so the streamer would only retry adds.
                const bool bRunStreamer = Comp.Runtime.Streamer.IsConfigured()
                                       && (Comp.Runtime.bStreamedInit || Stream.LoadRadius > 0.0f);
                if (bRunStreamer)
                {
                    const FNavTileStreamer::FStats Stats =
                        Comp.Runtime.Streamer.Update(*Comp.Runtime.Mesh, Comp.Tiles, Focus, Stream);

                    if (CVarNavDebugLog.GetValue() && (Stats.Added > 0 || Stats.Removed > 0 || Stats.Adopted > 0))
                    {
                        LOG_INFO("NavMesh streaming: +{} -{} ~{}, {} resident of {} wanted.",
                            Stats.Added, Stats.Removed, Stats.Adopted, Stats.Resident, Stats.Wanted);
                    }
                }
            }

            // Debug draw runs first so it emits even when later steps early-return.
            if (CVarNavDrawDebug.GetValue())
            {
                DrawNavDebug(Context, Queries, Comp);
            }

            // Hot-swap completed per-tile rebakes.
            PlatformTime::FStopwatch SwapWatch;
            TVector<FNavMesh::FTileSwap> Swaps;
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

                // Persist into Comp.Tiles before the runtime mesh hand-off, or PIE clones/saves init from empty.
                bool bUpdatedExisting = false;
                for (SNavTileData& T : Comp.Tiles)
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
                    SNavTileData NewTile;
                    NewTile.X = Job->TileX;
                    NewTile.Y = Job->TileY;
                    NewTile.Blob = Job->ResultBlob;
                    Comp.Tiles.Add(std::move(NewTile));
                }

                // Swapping in a tile the streamer never paged in would leave it resident and unevictable.
                const bool bStreamerOwnsResidency = Comp.Runtime.bStreamedInit;
                if (!bStreamerOwnsResidency || Comp.Runtime.Streamer.IsResident(Job->TileX, Job->TileY))
                {
                    Swaps.Emplace(Job->TileX, Job->TileY, &Job->ResultBlob);
                }
                Job->bConsumed.store(true, std::memory_order_release);
            }
            Comp.Runtime.Mesh->RebuildTiles(Swaps);
            if (!Swaps.IsEmpty() && CVarNavTimings.GetValue())
            {
                LOG_INFO("NavTiming swap: {} tiles in {:.2f} ms.", Swaps.Num(), SwapWatch.ElapsedMilliseconds());
            }

            // Swaps only merge the planner's island labels, so an exact pass runs on the build pool now and then and lands between swaps.
            constexpr uint32 kRelabelTicks = 15;
            Comp.Runtime.Mesh->AdoptClusterLabels();
            if (++Comp.Runtime.TicksSinceRelabel >= kRelabelTicks && Comp.Runtime.Mesh->TryBeginClusterRelabel())
            {
                Comp.Runtime.TicksSinceRelabel = 0;
                Sources.BuildPool->Enqueue([Mesh = Comp.Runtime.Mesh.get()] { Mesh->RelabelClusters(); });
            }
            
            Comp.Runtime.PendingRebakes.RemoveAll([](const TSharedPtr<FNavTileRebake>& J) { return !J || J->bConsumed.load(std::memory_order_acquire); });

            // Held between scans so a long interval still measures real elapsed time.
            Comp.Runtime.DynamicScanTimer = Comp.bDynamicRebuild
                ? Comp.Runtime.DynamicScanTimer + Context.GetDeltaTime()
                : 0.0f;

            // Tiles dirtied before the setting was turned off still rebake below.
            if (Comp.bDynamicRebuild && Comp.Runtime.DynamicScanTimer >= Comp.DynamicRebuildInterval)
            {
                Comp.Runtime.DynamicScanTimer = 0.0f;

                // What the previous scan dirtied has now been seen twice, so it is ready to rebake.
                for (uint64 TileKey : Comp.Runtime.SettlingTiles)
                {
                    Comp.Runtime.DirtyTiles.Add(TileKey);
                }
                Comp.Runtime.SettlingTiles.Reset();

                const int32 DirtyBefore = Comp.Runtime.DirtyTiles.Num() + Comp.Runtime.SettlingTiles.Num();
                const int32 CacheBudgetMB = NavSettings().TileCacheBudgetMB;
                if (!Comp.Runtime.TileCache && CacheBudgetMB > 0)
                {
                    Comp.Runtime.TileCache = MakeShared<FNavTileCache>((size_t)CacheBudgetMB << 20);
                }
                for (const FNavSourceBounds& Change : Comp.Runtime.PendingSourceChanges)
                {
                    int32 TX0, TY0, TX1, TY1;
                    TilesForAABB(Change.Min, Change.Max, Comp.Origin, Comp.TileWorldSize, Comp.TilesX, Comp.TilesY, TX0, TY0, TX1, TY1);
                    for (int32 TY = TY0; TY <= TY1; ++TY)
                    {
                        for (int32 TX = TX0; TX <= TX1; ++TX)
                        {
                            if (Change.bGeometry && Comp.Runtime.TileCache)
                            {
                                Comp.Runtime.TileCache->Invalidate(TX, TY);
                            }
                            // A tile already waiting rebakes from the geometry as it stands when it is kicked, so it needs no second pass.
                            const uint64 TileKey = PackTileKey(TX, TY);
                            if (!Comp.Runtime.DirtyTiles.Contains(TileKey))
                            {
                                Comp.Runtime.SettlingTiles.Add(TileKey);
                            }
                        }
                    }
                }

                if (CVarNavTimings.GetValue() && !Comp.Runtime.PendingSourceChanges.IsEmpty())
                {
                    LOG_INFO("NavTiming scan [{}]: {} changed footprints, dirty {} -> {}, pending {}.",
                        Context.GetWorldType() == EWorldType::Editor ? "editor" : "play", Comp.Runtime.PendingSourceChanges.Num(),
                        DirtyBefore, Comp.Runtime.DirtyTiles.Num() + Comp.Runtime.SettlingTiles.Num(), Comp.Runtime.PendingRebakes.Num());
                }
                Comp.Runtime.PendingSourceChanges.Reset();
            }

            // With scanning off nothing would promote them, so the last scan's tiles go straight through.
            if (!Comp.bDynamicRebuild && !Comp.Runtime.SettlingTiles.IsEmpty())
            {
                for (uint64 TileKey : Comp.Runtime.SettlingTiles)
                {
                    Comp.Runtime.DirtyTiles.Add(TileKey);
                }
                Comp.Runtime.SettlingTiles.Reset();
            }

            // Batches keep starting until the cap is reached, since tiles scattered over a big world each make a small batch of their own.
            const uint32 MaxConcurrent = (uint32)Math::Max(1, NavSettings().MaxConcurrentTileRebakes);
            while (!Comp.Runtime.DirtyTiles.IsEmpty() && static_cast<uint32>(Comp.Runtime.PendingRebakes.Num()) < MaxConcurrent)
            {
                // Main thread only snapshots params + world matrices; tessellation runs on a worker.
                const uint32 Capacity = MaxConcurrent - (uint32)Comp.Runtime.PendingRebakes.Num();

                // A batch gathers the union of its tiles, so scattered tiles re-tessellate everything between.
                TVector<uint64> Candidates(Comp.Runtime.DirtyTiles.begin(), Comp.Runtime.DirtyTiles.end());
                int32 SeedX, SeedY;
                NavTile::UnpackKey(Candidates[0], SeedX, SeedY);

                // Seeded at the dirty tile nearest the view, so what the player can see rebakes first.
                if (SignificanceState.bHasView)
                {
                    const int32 ViewX = (int32)std::floor((SignificanceState.ViewOrigin.x - Comp.Origin.x) / Comp.TileWorldSize);
                    const int32 ViewY = (int32)std::floor((SignificanceState.ViewOrigin.z - Comp.Origin.z) / Comp.TileWorldSize);
                    auto ViewDistance = [ViewX, ViewY](uint64 Key)
                    {
                        int32 X, Y;
                        NavTile::UnpackKey(Key, X, Y);
                        return Math::Max(Math::Abs(X - ViewX), Math::Abs(Y - ViewY));
                    };
                    const uint64 Nearest = *Algo::MinElement(Candidates, [&ViewDistance](uint64 A, uint64 B) { return ViewDistance(A) < ViewDistance(B); });
                    NavTile::UnpackKey(Nearest, SeedX, SeedY);
                }
                auto SeedDistance = [SeedX, SeedY](uint64 Key)
                {
                    int32 X, Y;
                    NavTile::UnpackKey(Key, X, Y);
                    return Math::Max(Math::Abs(X - SeedX), Math::Abs(Y - SeedY));
                };
                int32 Take = Math::Min(static_cast<int32>(Capacity), Candidates.Num());
                Algo::PartialSort(Candidates.begin(), Candidates.begin() + Take, Candidates.end(),
                    [&SeedDistance](uint64 A, uint64 B) { return SeedDistance(A) < SeedDistance(B); });

                // Tiles past this radius wait for a batch seeded near them, since one far tile widens the gather to everything in between.
                constexpr int32 kMaxBatchRadiusTiles = 4;
                while (Take > 1 && SeedDistance(Candidates[Take - 1]) > kMaxBatchRadiusTiles)
                {
                    --Take;
                }

                TVector<TSharedPtr<FNavTileRebake>> BatchJobs;
                BatchJobs.Reserve(Take);
                for (int32 i = 0; i < Take; ++i)
                {
                    Comp.Runtime.DirtyTiles.Remove(Candidates[i]);

                    auto Job = MakeShared<FNavTileRebake>();
                    NavTile::UnpackKey(Candidates[i], Job->TileX, Job->TileY);
                    if (Comp.Runtime.TileCache)
                    {
                        Job->CacheGeneration = Comp.Runtime.TileCache->GetGeneration(Job->TileX, Job->TileY);
                        Job->bCachePinned = Comp.Runtime.TileCache->Pin(Job->TileX, Job->TileY, Job->CacheGeneration);
                    }
                    BatchJobs.Add(Job);
                    Comp.Runtime.PendingRebakes.Add(std::move(Job));
                }

                // Terrain is pre-tessellated on this thread; the worker re-emits and bakes the dirty tiles.
                struct FInputSnapshot
                {
                    TVector<FNavSourcePrim> Prims;
                    FVector3                BakeMin;
                    FVector3                BakeMax;
                    FVector3                GatherMin;
                    FVector3                GatherMax;
                    SNavBuildSettings       Settings;
                    FNavBuildOutput         Layout;
                };
                auto Snap = MakeShared<FInputSnapshot>();
                Snap->BakeMin  = Comp.Center - Comp.GetWorldExtents();
                Snap->BakeMax  = Comp.Center + Comp.GetWorldExtents();
                Snap->Settings = Comp.Settings;
                Snap->Layout   = Comp.Runtime.LiveLayout;

                // Gathering the whole volume for a few dirty tiles costs a full bake, so narrow it to their footprint.
                {
                    const int32 BorderVoxels = (int32)std::ceil(Comp.Settings.AgentRadius / Comp.Settings.CellSize) + 3;
                    const float Border = (float)BorderVoxels * Comp.Settings.CellSize;
                    FVector3 Mn( FLT_MAX, Snap->BakeMin.y,  FLT_MAX);
                    FVector3 Mx(-FLT_MAX, Snap->BakeMax.y, -FLT_MAX);
                    for (const TSharedPtr<FNavTileRebake>& Job : BatchJobs)
                    {
                        const float TileMinX = Comp.Origin.x + (float)Job->TileX * Comp.TileWorldSize;
                        const float TileMinZ = Comp.Origin.z + (float)Job->TileY * Comp.TileWorldSize;
                        Mn.x = Math::Min(Mn.x, TileMinX - Border);
                        Mn.z = Math::Min(Mn.z, TileMinZ - Border);
                        Mx.x = Math::Max(Mx.x, TileMinX + Comp.TileWorldSize + Border);
                        Mx.z = Math::Max(Mx.z, TileMinZ + Comp.TileWorldSize + Border);
                    }
                    Snap->GatherMin = Math::Max(Mn, Snap->BakeMin);
                    Snap->GatherMax = Math::Min(Mx, Snap->BakeMax);
                }

                // Every tile already holding its heightfield means only the areas and links move, so no geometry is gathered at all.
                const bool bAnnotationsOnly = Algo::AllOf(BatchJobs, [](const TSharedPtr<FNavTileRebake>& Job) { return Job->bCachePinned; });
                PlatformTime::FStopwatch GatherWatch;
                GatherSourcePrims(Queries, Sources, Snap->GatherMin, Snap->GatherMax, Comp.Settings.CellSize, Snap->Prims, bAnnotationsOnly);
                const bool bLogRebakeTimings = CVarNavTimings.GetValue();
                if (bLogRebakeTimings)
                {
                    const FVector3 GatherSpan = Snap->GatherMax - Snap->GatherMin;
                    LOG_INFO("NavTiming rebake gather: {} tiles, box {:.0f}x{:.0f}, {} prims in {:.2f} ms, {} still dirty.",
                        BatchJobs.Num(), GatherSpan.x, GatherSpan.z, Snap->Prims.Num(),
                        GatherWatch.ElapsedMilliseconds(), Comp.Runtime.DirtyTiles.Num());
                }

                // The whole batch runs on the build pool, so a tile that takes milliseconds never holds a worker the frame waits on.
                FNavBuildPool* Pool = Sources.BuildPool.get();
                Pool->Enqueue([Snap, Jobs = std::move(BatchJobs), bLogRebakeTimings, Pool, Cache = Comp.Runtime.TileCache]() mutable
                {
                    PlatformTime::FStopwatch EmitWatch;
                    FNavBuildInput Input;
                    Input.BoundsMin = Snap->BakeMin;
                    Input.BoundsMax = Snap->BakeMax;
                    Input.Settings  = Snap->Settings;

                    for (const FNavSourcePrim& Prim : Snap->Prims)
                    {
                        AppendAnnotation(Prim, Input.AreaVolumes, Input.Links);
                    }
                    TessellateTerrainPatches(Snap->Prims, Pool);
                    EmitNavSourcePrims(Snap->Prims.GetData(), Snap->Prims.Num(), Snap->GatherMin, Snap->GatherMax,
                        Input.Vertices, Input.Indices, Pool);

                    // One binning pass feeds every dirty tile, rather than each tile rescanning the soup.
                    TVector<FNavTileCoord> Coords;
                    Coords.Reserve(Jobs.Num());
                    for (const TSharedPtr<FNavTileRebake>& Job : Jobs)
                    {
                        Coords.Emplace(Job->TileX, Job->TileY, Job->CacheGeneration);
                    }

                    const double EmitMs = EmitWatch.ElapsedMilliseconds();
                    const int32 EmittedTris = Input.Indices.Num() / 3;

                    PlatformTime::FStopwatch BakeWatch;
                    TVector<SNavTileData> Baked;
                    NavMeshBuilder::BakeTiles(Input, Snap->Layout, Coords, Baked, Pool, Cache.get());
                    if (bLogRebakeTimings)
                    {
                        LOG_INFO("NavTiming rebake work: {} tiles, emit {} tris in {:.2f} ms, bake {:.2f} ms.",
                            Coords.Num(), EmittedTris, EmitMs, BakeWatch.ElapsedMilliseconds());
                    }

                    for (int32 i = 0; i < Jobs.Num(); ++i)
                    {
                        if (Jobs[i]->bCachePinned)
                        {
                            Cache->Unpin(Jobs[i]->TileX, Jobs[i]->TileY);
                        }
                        if (i < Baked.Num())
                        {
                            Jobs[i]->ResultBlob = std::move(Baked[i].Blob);
                        }
                        Jobs[i]->bDone.store(true, std::memory_order_release);
                    }
                });
            }
        }

        template<typename TView>
        FNavMesh* FindReadyNavMesh(TView& View, FName Agent)
        {
            FNavMesh* Fallback = nullptr;
            for (ECS::FEntity Entity : View)
            {
                const SNavMeshComponent& Comp = View.template Get<ECS::Read<SNavMeshComponent>>(Entity);
                if (!Comp.Runtime.Mesh || !Comp.Runtime.Mesh->IsReady())
                {
                    continue;
                }
                if (Comp.Agent == Agent)
                {
                    return Comp.Runtime.Mesh.get();
                }
                Fallback = Fallback != nullptr ? Fallback : Comp.Runtime.Mesh.get();
            }
            return Agent.IsNone() ? Fallback : nullptr;
        }

        FNavMesh* FirstReadyNavMesh(const Nav::FNavMeshQuery& NavMeshes, FName Agent = FName())
        {
            auto View = NavMeshes.View();
            return FindReadyNavMesh(View, Agent);
        }
    }

    namespace NavMeshSystemInternal
    {
        // Initializes Center from entity transform so bake volume defaults at entity location.
        void OnNavMeshConstructed(ECS::FRegistry& Reg, ECS::FEntity Entity)
        {
            SNavMeshComponent& Nav = Reg.Get<ECS::Write<SNavMeshComponent>>(Entity);
            if (auto* Xform = Reg.TryGet<ECS::Read<STransformComponent>>(Entity))
            {
                Nav.Center = Xform->GetWorldTransform().GetLocation();
            }
        }

        FNavMesh* FirstReadyNavMeshFromWorld(CWorld* World, FName Agent = FName())
        {
            if (!World) return nullptr;
            auto View = World->View<ECS::Read<SNavMeshComponent>>();
            return FindReadyNavMesh(View, Agent);
        }
    }

    SNavMeshSystem::SNavMeshSystem() = default;
    SNavMeshSystem::~SNavMeshSystem() = default;

    void SNavMeshSystem::OnStartup(const SExclusiveContext& Context)
    {
        LUMINA_PROFILE_SCOPE();

        // A sink binds by handler identity, so reconnecting the same one is a no-op.
        ECS::FRegistry& Registry = Context.GetRegistry();
        Registry.GetSignals<SNavMeshComponent>().OnConstruct.Connect<&NavMeshSystemInternal::OnNavMeshConstructed>();

        if (!Tracker)
        {
            Tracker = MakeUnique<FNavSourceTracker>();
            // A few threads keep up with a world full of moving obstacles, and every extra one is a thread the frame's workers share cores with.
            Tracker->BuildPool = MakeUnique<FNavBuildPool>(Math::Clamp(Jobs::GetNumWorkers() / 8, 1u, 4u));
            NavMeshSystemInternal::WatchSourceComponents(Registry, Tracker.get(), true);
        }

        NavMeshSystemInternal::UpdateSources(Context, SourceQueries, *Tracker);

        Volumes.ForEach([&](ECS::TMut<SNavMeshComponent> CompMut, const STransformComponent* Transform)
        {
            SNavMeshComponent& Comp = CompMut.GetMut();
            NavMeshSystemInternal::TickComponent(Context, SourceQueries, *SignificanceState, Comp, Transform, *Tracker);
        });
    }

    void SNavMeshSystem::OnUpdate(const SSystemContext& Context)
    {
        if (!Tracker)
        {
            return;
        }

        FNavSourceTracker& Sources = *Tracker;
        PlatformTime::FStopwatch SourceWatch;
        NavMeshSystemInternal::UpdateSources(Context, SourceQueries, Sources);
        if (CVarNavTimings.GetValue() && (Sources.Refreshed > 0 || !Sources.Changes.IsEmpty()))
        {
            LOG_INFO("NavTiming sources: {} tracked, {} entities refreshed in {:.2f} ms, {} new, {} moved, {} reshaped, {} removed.",
                Sources.RecordOfKey.Num(), Sources.Refreshed, SourceWatch.ElapsedMilliseconds(),
                Sources.Counts[0], Sources.Counts[1], Sources.Counts[2], Sources.Counts[3]);
        }

        Volumes.ForEach([&](ECS::TMut<SNavMeshComponent> CompMut, const STransformComponent* Transform)
        {
            SNavMeshComponent& Comp = CompMut.GetMut();
            NavMeshSystemInternal::TickComponent(Context, SourceQueries, *SignificanceState, Comp, Transform, Sources);
        });
    }

    void SNavMeshSystem::OnTeardown(const SExclusiveContext& Context)
    {
        LUMINA_PROFILE_SCOPE();
        Volumes.ForEach([&](ECS::TMut<SNavMeshComponent> CompMut, const STransformComponent*)
        {
            SNavMeshComponent& Comp = CompMut.GetMut();
            if (Comp.Runtime.ActiveBake)
            {
                Comp.Runtime.ActiveBake->bCancelRequested.store(true, std::memory_order_release);
            }
            Comp.Runtime.Mesh.reset();
            Comp.Runtime.ActiveBake.reset();
            // Worker holds its own shared_ptr; clearing here just prevents consumption.
            Comp.Runtime.PendingInit.reset();
            Comp.Runtime.PendingRebakes.Reset();
            Comp.Runtime.TileCache.reset();
            Comp.Runtime.DirtyTiles.Reset();
            Comp.Runtime.SettlingTiles.Reset();
            Comp.Runtime.PendingSourceChanges.Reset();
            Comp.Runtime.State = ENavBakeState::Idle;
        });

        if (Tracker)
        {
            ECS::FRegistry& Registry = Context.GetRegistry();
            NavMeshSystemInternal::WatchSourceComponents(Registry, Tracker.get(), false);
            Tracker.reset();
        }
    }

    namespace Nav
    {
        FNavMesh* GetReadyNavMesh(const FNavMeshQuery& NavMeshes, FName Agent)
        {
            return NavMeshSystemInternal::FirstReadyNavMesh(NavMeshes, Agent);
        }

        bool FindPath(const FNavMeshQuery& NavMeshes, const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, FNavPath& Out, FName Agent)
        {
            FNavMesh* Mesh = NavMeshSystemInternal::FirstReadyNavMesh(NavMeshes, Agent);
            if (!Mesh)
            {
                // Reset rather than leave a caller's reused path reading as its previous outcome.
                Out = {};
                Out.Result = ENavPathResult::NoNavMesh;
                return false;
            }
            return Mesh->FindPath(Start, End, Filter, Out);
        }

        bool ProjectPoint(const FNavMeshQuery& NavMeshes, const FVector3& World, const FVector3& Extents, const FNavQueryFilter& Filter, FVector3& Out, FName Agent)
        {
            FNavMesh* Mesh = NavMeshSystemInternal::FirstReadyNavMesh(NavMeshes, Agent);
            return Mesh && Mesh->ProjectPoint(World, Extents, Filter, Out);
        }

        bool Raycast(const FNavMeshQuery& NavMeshes, const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, SNavRaycastResult& Out, FName Agent)
        {
            FNavMesh* Mesh = NavMeshSystemInternal::FirstReadyNavMesh(NavMeshes, Agent);
            return Mesh && Mesh->Raycast(Start, End, Filter, Out);
        }

        bool IsReady(CWorld* World, FName Agent)
        {
            return NavMeshSystemInternal::FirstReadyNavMeshFromWorld(World, Agent) != nullptr;
        }

        int32 RequestRebuild(CWorld* World)
        {
            if (!World)
            {
                return 0;
            }
            int32 Count = 0;
            auto View = World->View<ECS::Write<SNavMeshComponent>>();
            for (ECS::FEntity E : View)
            {
                View.Get<ECS::Write<SNavMeshComponent>>(E).bBakeRequested = true;
                ++Count;
            }
            return Count;
        }

        bool FindPath(CWorld* World, const FVector3& Start, const FVector3& End, FNavPath& Out, FName Agent)
        {
            return FindPath(World, Start, End, 0, Out, Agent);
        }

        bool FindPath(CWorld* World, const FVector3& Start, const FVector3& End, int32 MaxCorners, FNavPath& Out, FName Agent)
        {
            FNavMesh* Mesh = NavMeshSystemInternal::FirstReadyNavMeshFromWorld(World, Agent);
            if (!Mesh)
            {
                Out = {};
                Out.Result = ENavPathResult::NoNavMesh;
                return false;
            }
            FNavQueryFilter Filter;
            Filter.MaxCorners = MaxCorners;
            return Mesh->FindPath(Start, End, Filter, Out);
        }

        bool ProjectPoint(CWorld* World, const FVector3& Point, const FVector3& Extents, FVector3& Out, FName Agent)
        {
            FNavMesh* Mesh = NavMeshSystemInternal::FirstReadyNavMeshFromWorld(World, Agent);
            FNavQueryFilter Filter;
            return Mesh && Mesh->ProjectPoint(Point, Extents, Filter, Out);
        }

        bool Raycast(CWorld* World, const FVector3& Start, const FVector3& End, SNavRaycastResult& Out, FName Agent)
        {
            FNavMesh* Mesh = NavMeshSystemInternal::FirstReadyNavMeshFromWorld(World, Agent);
            FNavQueryFilter Filter;
            return Mesh && Mesh->Raycast(Start, End, Filter, Out);
        }

        bool IsWalkableLine(CWorld* World, const FVector3& From, const FVector3& To, FName Agent)
        {
            SNavRaycastResult Result;
            return Raycast(World, From, To, Result, Agent) && !Result.bHit;
        }

        bool FindRandomReachablePoint(CWorld* World, const FVector3& Origin, float Radius, FVector3& Out, FName Agent)
        {
            FNavMesh* Mesh = NavMeshSystemInternal::FirstReadyNavMeshFromWorld(World, Agent);
            FNavQueryFilter Filter;
            return Mesh && Mesh->FindRandomPoint(Origin, Radius, Filter, Out);
        }

        bool IsReachable(CWorld* World, const FVector3& From, const FVector3& To, FName Agent)
        {
            FNavPath Path;
            return FindPath(World, From, To, Path, Agent) && Path.bValid && !Path.bPartial;
        }

        float PathLength(CWorld* World, const FVector3& From, const FVector3& To, FName Agent)
        {
            FNavPath Path;
            if (!FindPath(World, From, To, Path, Agent) || !Path.bValid) return -1.0f;
            float Len = 0.0f;
            for (int32 i = 1; i < Path.Corners.Num(); ++i)
            {
                Len += Math::Length(Path.Corners[i] - Path.Corners[i - 1]);
            }
            return Len;
        }

        void DrawPath(CWorld* World, const FNavPath& Path, const FVector4& Color, float Thickness, float Lift, float Duration)
        {
            if (!World || !Path.bValid || Path.Corners.Num() < 2) return;

            const FVector3 LiftV(0.0f, Lift, 0.0f);
            for (int32 i = 1; i < Path.Corners.Num(); ++i)
            {
                World->GetDebugInterface()->DrawLine(Path.Corners[i - 1] + LiftV, Path.Corners[i] + LiftV, Color, Thickness, true, Duration);
            }
            // Corner spheres so kinks read at a glance; goal sphere distinguished.
            const float R = 0.12f;
            for (int32 i = 0; i < Path.Corners.Num(); ++i)
            {
                const bool bGoal = (i + 1 == Path.Corners.Num());
                const FVector4 SphColor = bGoal ? FVector4(1.0f, 1.0f, 1.0f, 1.0f) : Color;
                World->GetDebugInterface()->DrawSphere(Path.Corners[i] + LiftV, bGoal ? R * 1.6f : R, SphColor, 10, 1.0f, true, Duration);
            }
            // Partial paths are easy to misread; flag them with a red marker on the last corner.
            if (Path.bPartial)
            {
                World->GetDebugInterface()->DrawSphere(Path.Corners.Last() + LiftV, 0.25f, FVector4(1.0f, 0.2f, 0.2f, 1.0f), 12, 1.0f, true, Duration);
            }
        }

        bool DrawDebugPath(CWorld* World, const FVector3& From, const FVector3& To, const FVector4& Color, float Duration, FName Agent)
        {
            FNavPath Path;
            if (!FindPath(World, From, To, Path, Agent) || !Path.bValid) return false;
            DrawPath(World, Path, Color, 3.0f, 0.15f, Duration);
            return true;
        }
    }
}
