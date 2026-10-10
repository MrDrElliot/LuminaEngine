#include "RuntimePCH.h"
#include "NavMeshSystem.h"

#include <algorithm>

#include "Platform/Time/PlatformTime.h"
#include "World/ECS/Registry.h"

#include "AI/Navigation/NavMesh.h"
#include "AI/Navigation/NavMeshBuilder.h"
#include "AI/Navigation/NavTileStreamer.h"
#include "Config/NavigationSettings.h"
#include "World/Entity/Systems/SignificanceSystem.h"
#include "Assets/AssetTypes/Mesh/StaticMesh/StaticMesh.h"
#include "Assets/AssetTypes/Physics/CollisionShape.h"
#include "Core/Console/ConsoleVariable.h"
#include "Physics/CollisionShapeGen.h"
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
        Writes<SNavMeshComponent>();
        Reads<SRigidBodyComponent, SBoxColliderComponent, SSphereColliderComponent, SMeshColliderComponent, SCapsuleColliderComponent, SCylinderColliderComponent, SCharacterPhysicsComponent, STerrainColliderComponent, STerrainComponent, STransformComponent, SStaticMeshComponent, SDynamicMeshColliderComponent, SDynamicMeshComponent, SCompoundColliderComponent, SNavModifierComponent, SNavLinkComponent, SFoliageComponent>();
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

    namespace
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

        void DrawNavDebug(const FSystemContext& Context, SNavMeshComponent& Comp)
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
                    RT.DebugSurface.clear();
                    RT.DebugSurface.reserve((size_t)Stats.Triangles * 3);
                    Mesh.ForEachTriangle([&](const FVector3& A, const FVector3& B, const FVector3& C, uint8 Area)
                    {
                        FVector4 Color = bColorByArea ? NavAreaColor(Area) : FVector4(0.15f, 0.85f, 0.35f, 1.0f);
                        Color.w = Alpha;
                        const uint32 Packed = PackColor(Color);
                        RT.DebugSurface.push_back({ A + Lift, Packed });
                        RT.DebugSurface.push_back({ B + Lift, Packed });
                        RT.DebugSurface.push_back({ C + Lift, Packed });
                    });
                    RT.DebugSurfaceEpoch   = Epoch;
                    RT.DebugSurfaceAlpha   = Alpha;
                    RT.bDebugSurfaceByArea = bColorByArea;
                    RT.bDebugSurfaceValid  = true;
                }

                if (!RT.DebugSurface.empty())
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
                auto ModifierView = Context.CreateView<SNavModifierComponent, STransformComponent>();
                for (ECS::FEntity E : ModifierView)
                {
                    const SNavModifierComponent& Modifier = ModifierView.Get<SNavModifierComponent>(E);
                    if (!Modifier.bEnabled) continue;

                    const FTransform& WT = ModifierView.Get<STransformComponent>(E).GetWorldTransform();
                    const FVector3 Scale = WT.GetScale();
                    const FVector3 Center = WT.GetLocation() + Math::Rotate(WT.GetRotation(), Modifier.Offset * Scale);
                    const FVector3 Half(Modifier.Extents.x * std::fabs(Scale.x), Modifier.Extents.y * std::fabs(Scale.y), Modifier.Extents.z * std::fabs(Scale.z));
                    Context.DrawDebugBox(Center, Half, WT.GetRotation(), NavAreaColor((uint8)Modifier.Area), 3.0f, -1.0f);
                }
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

    namespace
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

        // Compound children reuse the primitive types, so their sub-indices sit above a collision shape asset's.
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
            size_t Seed = (size_t)(uintptr_t)Asset;
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
        // Background work is never run by a waiting thread, so a gather the game thread blocks on has to outrank a bake in flight.
        ETaskPriority GatherPriority()
        {
            return Threading::IsMainThread() ? ETaskPriority::High : ETaskPriority::Background;
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
            Verts.resize(Rings * (Segments + 1));

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

        // Gift wrapping over the XZ projection, which an oriented box reduces to a hexagon at worst.
        void BuildXZHull(const FVector3* Pts, int32 N, TVector<FVector3>& Out)
        {
            Out.clear();
            if (N < 3)
            {
                return;
            }

            int32 Leftmost = 0;
            for (int32 i = 1; i < N; ++i)
            {
                if (Pts[i].x < Pts[Leftmost].x || (Pts[i].x == Pts[Leftmost].x && Pts[i].z < Pts[Leftmost].z))
                {
                    Leftmost = i;
                }
            }

            int32 Current = Leftmost;
            do
            {
                Out.push_back(FVector3(Pts[Current].x, 0.0f, Pts[Current].z));

                int32 Next = (Current + 1) % N;
                for (int32 i = 0; i < N; ++i)
                {
                    const float Cross = (Pts[Next].x - Pts[Current].x) * (Pts[i].z - Pts[Current].z)
                                      - (Pts[Next].z - Pts[Current].z) * (Pts[i].x - Pts[Current].x);
                    if (Cross < 0.0f)
                    {
                        Next = i;
                    }
                }
                Current = Next;
            }
            while (Current != Leftmost && (int32)Out.size() <= N);
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

        // Subsampled so a quad is about one cell, keeping only tris overlapping the bake bounds.
        void TessellateTerrain(const STerrainComponent& T, const FMatrix4& W, const FVector3& BakeMin, const FVector3& BakeMax, float CellSize, TVector<FVector3>& Out)
        {
            const int32 Res = T.Resolution;
            if (Res < 2 || (int64)T.Heightmap.size() < (int64)Res * (int64)Res)
            {
                return;
            }

            const float Stride = T.TileWorldSize / float(Res - 1);
            const float Half   = T.TileWorldSize * 0.5f;

            auto Height = [&](int32 Col, int32 Row) -> float
            {
                return T.Heightmap[(size_t)Row * (size_t)Res + (size_t)Col] * T.MaxHeight;
            };
            auto Pos = [&](int32 Col, int32 Row) -> FVector3
            {
                const FVector3 Local(-Half + (float)Col * Stride, Height(Col, Row), -Half + (float)Row * Stride);
                return FVector3(W * FVector4(Local, 1.0f));
            };

            // Clipped to the bake box first, or a volume covering a slice of a huge terrain still pays for
            // the whole thing and gets coarsened by it.
            int32 ColBegin = 0, ColEnd = Res - 1, RowBegin = 0, RowEnd = Res - 1;
            {
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
                ColBegin = Math::Clamp((int32)std::floor((LocalMin.x + Half) / Stride) - 1, 0, Res - 2);
                ColEnd   = Math::Clamp((int32)std::ceil ((LocalMax.x + Half) / Stride) + 1, ColBegin + 1, Res - 1);
                RowBegin = Math::Clamp((int32)std::floor((LocalMin.z + Half) / Stride) - 1, 0, Res - 2);
                RowEnd   = Math::Clamp((int32)std::ceil ((LocalMax.z + Half) / Stride) + 1, RowBegin + 1, Res - 1);
            }

            // One nav cell per quad is the finest worth emitting. The ceiling is on the quads this bake
            // actually emits, since coarsening past the cell size turns real relief into a steep facet and
            // Recast then marks the whole quad unwalkable.
            int32 Step = Math::Max(1, (int32)std::floor(Math::Max(CellSize, 0.01f) / Math::Max(Stride, 1e-4f)));
            const int32 SpanQuads = Math::Max(ColEnd - ColBegin, RowEnd - RowBegin);
            Step = Math::Max(Step, (int32)std::ceil((float)SpanQuads / 1024.0f));

            // Rows are independent, and on a world-sized bake this is the one source worth fanning out.
            const int32 RowStrips = (RowEnd - RowBegin + Step - 1) / Step;
            if (RowStrips <= 0)
            {
                return;
            }

            TVector<TVector<FVector3>> Strips((size_t)RowStrips);
            Task::ParallelFor((uint32)RowStrips, [&](uint32 StripIndex)
            {
                const int32 Row  = RowBegin + (int32)StripIndex * Step;
                const int32 RowN = Math::Min(Row + Step, RowEnd);
                TVector<FVector3>& Strip = Strips[StripIndex];

                for (int32 Col = ColBegin; Col < ColEnd; Col += Step)
                {
                    const int32 ColN = Math::Min(Col + Step, ColEnd);
                    const FVector3 A = Pos(Col,  Row);
                    const FVector3 B = Pos(ColN, Row);
                    const FVector3 C = Pos(ColN, RowN);
                    const FVector3 D = Pos(Col,  RowN);
                    // Wound so the surface normal points +Y (Recast's slope test marks it walkable).
                    if (TriIntersectsAABB(A, C, B, BakeMin, BakeMax))
                    {
                        Strip.push_back(A); Strip.push_back(C); Strip.push_back(B);
                    }
                    if (TriIntersectsAABB(A, D, C, BakeMin, BakeMax))
                    {
                        Strip.push_back(A); Strip.push_back(D); Strip.push_back(C);
                    }
                }
            }, 1, GatherPriority());

            size_t Total = 0;
            for (const TVector<FVector3>& Strip : Strips)
            {
                Total += Strip.size();
            }
            Out.reserve(Out.size() + Total);
            for (TVector<FVector3>& Strip : Strips)
            {
                Out.insert(Out.end(), Strip.begin(), Strip.end());
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
            TSharedPtr<TVector<FVector3>>   TriangleSoup;             // world-space tri soup (groups of 3); Terrain and TriangleSoup types
            TSharedPtr<FNavAreaVolume>      AreaVolume;               // AreaVolume type
            TSharedPtr<FNavOffMeshLink>     Link;                     // OffMeshLink type
            // Foliage type, shared by every instance of a species in its local space and placed by World.
            TSharedPtr<const TVector<FNavSourcePrim>> Children;
            // Reads the mesh's coarsest LOD, enough for a footprint at a fraction of the triangles.
            bool                            bCoarsestLOD = false;
        };

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
                        for (size_t i = 0; i + 2 < Tris.size(); i += 3)
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
                                for (size_t i = 0; i + 2 < Tris.size(); i += 3)
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

        // Emission is pure per prim, so it fans out into per-chunk buffers and concatenates with the
        // indices rebased. Serial emission was the whole front half of a world-sized bake.
        void EmitNavSourcePrims(const FNavSourcePrim* Prims, size_t Count, const FVector3& EmitMin,
            const FVector3& EmitMax, TVector<FVector3>& OutVertices, TVector<uint32>& OutIndices)
        {
            if (Count == 0)
            {
                return;
            }

            const uint32 Workers   = Math::Max(1u, Jobs::GetNumWorkers());
            const uint32 NumChunks = (uint32)Math::Min<size_t>(Count, (size_t)Workers * 2u);
            const size_t PerChunk  = (Count + NumChunks - 1) / NumChunks;

            TVector<FGatherAccumulator> Chunks((size_t)NumChunks);
            Task::ParallelFor(NumChunks, [&](uint32 ChunkIndex)
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
                TotalVerts += Acc.Vertices.size();
                TotalIndices += Acc.Indices.size();
            }
            OutVertices.reserve(OutVertices.size() + TotalVerts);
            OutIndices.reserve(OutIndices.size() + TotalIndices);

            for (FGatherAccumulator& Acc : Chunks)
            {
                const uint32 Base = (uint32)OutVertices.size();
                OutVertices.insert(OutVertices.end(), Acc.Vertices.begin(), Acc.Vertices.end());
                for (uint32 Index : Acc.Indices)
                {
                    OutIndices.push_back(Base + Index);
                }
            }
        }

        // Annotations travel with the geometry prims so every bake path sees the same authored set.
        void AppendAnnotation(const FNavSourcePrim& Prim, TVector<FNavAreaVolume>& OutVolumes, TVector<FNavOffMeshLink>& OutLinks)
        {
            if (Prim.AreaVolume)
            {
                OutVolumes.push_back(*Prim.AreaVolume);
            }
            else if (Prim.Link)
            {
                OutLinks.push_back(*Prim.Link);
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
        void ForEachCollisionShapePrim(const CCollisionShape& Asset, const FMatrix4& ColliderWorld, FnEmit&& Emit)
        {
            if (Asset.IsConcave())
            {
                FNavSourceEntry Entry;
                Entry.Prim.Type = ENavColliderType::TriangleSoup;
                Entry.Prim.World = ColliderWorld;
                Entry.Prim.TriangleSoup = MakeShared<TVector<FVector3>>();

                Entry.Prim.TriangleSoup->reserve(Asset.TriangleIndices.size());
                for (uint32 Index : Asset.TriangleIndices)
                {
                    const FVector3 P = FVector3(ColliderWorld * FVector4(Asset.TriangleVertices[Index], 1.0f));
                    Entry.Prim.TriangleSoup->push_back(P);
                    Entry.AABBMin = Math::Min(Entry.AABBMin, P);
                    Entry.AABBMax = Math::Max(Entry.AABBMax, P);
                }

                Emit(ENavColliderType::TriangleSoup, 0u, std::move(Entry));
                return;
            }

            for (uint32 i = 0; i < (uint32)Asset.Primitives.size(); ++i)
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

                        Entry.Prim.TriangleSoup->reserve(HullIndices.size());
                        for (uint32 Index : HullIndices)
                        {
                            const FVector3 P = FVector3(Entry.Prim.World * FVector4(HullVertices[Index], 1.0f));
                            Entry.Prim.TriangleSoup->push_back(P);
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
            if (const CCollisionShape* Shape = Type.GetDefaultCollisionShape())
            {
                ForEachCollisionShapePrim(*Shape, FMatrix4(1.0f), [&](ENavColliderType, uint32, FNavSourceEntry&& Entry)
                {
                    Template.LocalMin = Math::Min(Template.LocalMin, Entry.AABBMin);
                    Template.LocalMax = Math::Max(Template.LocalMax, Entry.AABBMax);
                    Children->push_back(std::move(Entry.Prim));
                });
                Template.ContentId = MakeContentId(Shape, Shape->TriangleIndices.size(), Shape->Primitives.size());
            }
            else if (CStaticMesh* Mesh = Type.Mesh.Get(); Mesh != nullptr && !Mesh->GetMeshResource().bSkinnedMesh)
            {
                FNavSourcePrim& Child = Children->emplace_back();
                Child.Type = ENavColliderType::Mesh;
                Child.Mesh = Mesh;
                Child.bCoarsestLOD = true;
                const FMeshletData& MeshletData = Mesh->GetMeshResource().MeshletData;
                Template.ContentId = MakeContentId(Mesh, MeshletData.VertexPositions.size(), MeshletData.MeshletTriangles.size());
                Template.LocalMin = Mesh->GetAABB().Min;
                Template.LocalMax = Mesh->GetAABB().Max;
            }

            if (!Children->empty())
            {
                Template.Children = Children;
            }
            return Template;
        }

        uint64 FoliageNavSignature(const SFoliageComponent& Foliage)
        {
            size_t Seed = Foliage.InstancesVersion;
            Hash::HashCombine(Seed, Foliage.Types.size());
            for (const SFoliageType& Type : Foliage.Types)
            {
                Hash::HashCombine(Seed, (size_t)Type.bEnableCollision);
                const CCollisionShape* Shape = Type.GetDefaultCollisionShape();
                Hash::HashCombine(Seed, (size_t)(uintptr_t)Shape);
                if (Shape != nullptr)
                {
                    Hash::HashCombine(Seed, Shape->TriangleIndices.size());
                    Hash::HashCombine(Seed, Shape->Primitives.size());
                }
                CStaticMesh* Mesh = Type.Mesh.Get();
                Hash::HashCombine(Seed, (size_t)(uintptr_t)Mesh);
                if (Mesh != nullptr)
                {
                    Hash::HashCombine(Seed, Mesh->GetMeshResource().MeshletData.MeshletTriangles.size());
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
        TVector<FFoliageNavTemplate> Templates;
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

    namespace
    {
        FNavFoliageCache& GetFoliageCache(SNavMeshComponent& Comp)
        {
            if (!Comp.Runtime.FoliageCache)
            {
                Comp.Runtime.FoliageCache = MakeShared<FNavFoliageCache>();
            }
            return *Comp.Runtime.FoliageCache;
        }

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

            Entry.Templates.clear();
            Entry.Templates.reserve(Foliage.Types.size());
            for (const SFoliageType& Type : Foliage.Types)
            {
                Entry.Templates.push_back(BuildFoliageNavTemplate(Type));
            }

            constexpr uint32 MaxKeyedInstances = 1u << 24;
            const uint32 NumInstances = (uint32)Math::Min<size_t>(Foliage.Instances.size(), MaxKeyedInstances);
            Entry.Min.assign(NumInstances, FVector3( FLT_MAX));
            Entry.Max.assign(NumInstances, FVector3(-FLT_MAX));
            for (uint32 Index = 0; Index < NumInstances; ++Index)
            {
                const SFoliageInstance& Instance = Foliage.Instances[Index];
                if (!Foliage.IsValidType(Instance.TypeIndex) || !Entry.Templates[(size_t)Instance.TypeIndex].Children)
                {
                    continue;
                }

                const FFoliageNavTemplate& Template = Entry.Templates[(size_t)Instance.TypeIndex];
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

        // A null cache leaves foliage out, for the change scan that diffs it per entity instead.
        void CollectNavSources(const FSystemContext& Context, const FVector3& BakeMin, const FVector3& BakeMax, bool bTessellateTerrain, float CellSize, TVector<FNavSourceEntry>& Out, FNavFoliageCache* FoliageCache)
        {

            // A simulated body is loose debris rather than level geometry, and baking it would rebuild tiles every time it settles.
            auto RigidBodies = Context.GetRegistry().GetStorage<SRigidBodyComponent>();
            auto IsSimulated = [&RigidBodies](ECS::FEntity Entity)
            {
                const SRigidBodyComponent* Body = RigidBodies.TryGet(Entity);
                return Body != nullptr && Body->BodyType == EBodyType::Dynamic;
            };

            auto BoxView = Context.CreateView<SBoxColliderComponent, STransformComponent>();
            for (ECS::FEntity E : BoxView)
            {
                SBoxColliderComponent& Box = BoxView.Get<SBoxColliderComponent>(E);
                if (!Box.bAffectsNavigation || IsSimulated(E)) continue;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Box);
                Entry.Prim.Type = ENavColliderType::Box;
                Entry.Prim.World = ColliderToWorld(BoxView.Get<STransformComponent>(E), Box.TranslationOffset, Box.RotationOffset);
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
            for (ECS::FEntity E : SphereView)
            {
                SSphereColliderComponent& Sphere = SphereView.Get<SSphereColliderComponent>(E);
                if (!Sphere.bAffectsNavigation || IsSimulated(E)) continue;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Sphere);
                Entry.Prim.Type = ENavColliderType::Sphere;
                Entry.Prim.World = ColliderToWorld(SphereView.Get<STransformComponent>(E), Sphere.TranslationOffset, FVector3(0.0f));
                Entry.Prim.Shape = FVector3(Sphere.Radius, 0.0f, 0.0f);
                const FVector3 Center = FVector3(Entry.Prim.World * FVector4(0.0f, 0.0f, 0.0f, 1.0f));
                const float R = ScaledRadius(Entry.Prim.World, Sphere.Radius);
                Entry.AABBMin = Center - FVector3(R);
                Entry.AABBMax = Center + FVector3(R);
                Out.push_back(std::move(Entry));
            }

            // One source per collision piece, so nav sees the same decomposed shape physics does.
            auto ShapeAssetView = Context.CreateView<SCollisionShapeComponent, STransformComponent>();
            for (ECS::FEntity E : ShapeAssetView)
            {
                SCollisionShapeComponent& CSC = ShapeAssetView.Get<SCollisionShapeComponent>(E);
                if (!CSC.bAffectsNavigation || IsSimulated(E))
                {
                    continue;
                }

                const CCollisionShape* Asset = CSC.CollisionShape.Get();
                if (Asset == nullptr || !Asset->HasCollision())
                {
                    continue;
                }

                const FMatrix4 ColliderWorld = ColliderToWorld(ShapeAssetView.Get<STransformComponent>(E),
                                                               CSC.TranslationOffset, CSC.RotationOffset);

                const uint64 AssetContentId = MakeContentId(Asset, Asset->TriangleIndices.size(), Asset->Primitives.size());
                ForEachCollisionShapePrim(*Asset, ColliderWorld, [&](ENavColliderType KeyType, uint32 SubIndex, FNavSourceEntry&& Entry)
                {
                    Entry.Key = PackSourceKey(E, KeyType, SubIndex);
                    Entry.ContentId = AssetContentId;
                    Out.push_back(std::move(Entry));
                });
            }

            // Painted instances are world space, and erasing one shifts or swaps indices, which the detector sees as a move.
            auto FoliageView = Context.CreateView<SFoliageComponent>();
            for (ECS::FEntity E : FoliageView)
            {
                if (FoliageCache == nullptr)
                {
                    break;
                }

                const SFoliageComponent& Foliage = FoliageView.Get<SFoliageComponent>(E);
                const FNavFoliageEntity& Cached = RefreshFoliageEntity(*FoliageCache, E, Foliage);
                for (uint32 Index = 0; Index < (uint32)Cached.Min.size(); ++Index)
                {
                    const FVector3& Mn = Cached.Min[Index];
                    const FVector3& Mx = Cached.Max[Index];
                    if (!HasFoliageBounds(Mn, Mx) ||
                        Mn.x > BakeMax.x || Mx.x < BakeMin.x || Mn.y > BakeMax.y || Mx.y < BakeMin.y || Mn.z > BakeMax.z || Mx.z < BakeMin.z)
                    {
                        continue;
                    }

                    const SFoliageInstance& Instance = Foliage.Instances[Index];
                    const FFoliageNavTemplate& Template = Cached.Templates[(size_t)Instance.TypeIndex];
                    FNavSourceEntry Entry;
                    Entry.Key = PackSourceKey(E, ENavColliderType::Foliage, Index);
                    Entry.ContentId = Template.ContentId;
                    Entry.Prim.Type = ENavColliderType::Foliage;
                    Entry.Prim.World = Instance.GetMatrix();
                    Entry.Prim.Children = Template.Children;
                    Entry.AABBMin = Mn;
                    Entry.AABBMax = Mx;
                    Out.push_back(std::move(Entry));
                }
            }

            auto MeshView = Context.CreateView<SMeshColliderComponent, STransformComponent>();
            for (ECS::FEntity E : MeshView)
            {
                SMeshColliderComponent& MC = MeshView.Get<SMeshColliderComponent>(E);
                if (!MC.bAffectsNavigation || IsSimulated(E)) continue;
                const SStaticMeshComponent* Fallback = Context.GetRegistry().TryGet<SStaticMeshComponent>(E);
                CStaticMesh* Mesh = ResolveMeshColliderAsset(MC, Fallback);
                if (!Mesh || Mesh->GetMeshResource().bSkinnedMesh) continue;

                // The mesh's simplified collision replaces its render triangles, as it does for the physics body.
                if (const CCollisionShape* Asset = Mesh->GetDefaultCollisionShape())
                {
                    const FMatrix4 ColliderWorld = ColliderToWorld(MeshView.Get<STransformComponent>(E), MC.TranslationOffset, MC.RotationOffset);
                    const uint64 AssetContentId = MakeContentId(Asset, Asset->TriangleIndices.size(), Asset->Primitives.size());
                    ForEachCollisionShapePrim(*Asset, ColliderWorld, [&](ENavColliderType KeyType, uint32 SubIndex, FNavSourceEntry&& Entry)
                    {
                        Entry.Key = PackSourceKey(E, KeyType, SubIndex);
                        Entry.ContentId = AssetContentId;
                        Out.push_back(std::move(Entry));
                    });
                    continue;
                }

                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Mesh);
                Entry.Prim.Type = ENavColliderType::Mesh;
                Entry.Prim.World = ColliderToWorld(MeshView.Get<STransformComponent>(E), MC.TranslationOffset, MC.RotationOffset);
                Entry.Prim.Mesh = Mesh;
                const FMeshletData& MeshletData = Mesh->GetMeshResource().MeshletData;
                Entry.ContentId = MakeContentId(Mesh, MeshletData.VertexPositions.size(), MeshletData.MeshletTriangles.size());
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

            // Fingerprinted on the render-data version so a re-commit with unchanged bounds still dirties the tile.
            auto DynamicMeshView = Context.CreateView<SDynamicMeshColliderComponent, SDynamicMeshComponent, STransformComponent>();
            for (ECS::FEntity E : DynamicMeshView)
            {
                if (!DynamicMeshView.Get<SDynamicMeshColliderComponent>(E).bAffectsNavigation || IsSimulated(E)) continue;
                const SDynamicMeshComponent& DM = DynamicMeshView.Get<SDynamicMeshComponent>(E);
                TSharedPtr<FDynamicMeshRenderData> MeshData = DM.LoadRenderData();
                if (!MeshData || MeshData->Resource.MeshletData.IsEmpty()) continue;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::DynamicMesh);
                Entry.ContentId = DM.LoadRenderDataVersion();
                Entry.Prim.Type = ENavColliderType::DynamicMesh;
                Entry.Prim.World = DynamicMeshView.Get<STransformComponent>(E).GetWorldMatrix();
                Entry.Prim.DynamicMesh = MeshData;
                const FVector3& Mn = MeshData->LocalMin;
                const FVector3& Mx = MeshData->LocalMax;
                const FVector3 Corners[8] = {
                    {Mn.x, Mn.y, Mn.z}, {Mx.x, Mn.y, Mn.z}, {Mn.x, Mx.y, Mn.z}, {Mx.x, Mx.y, Mn.z},
                    {Mn.x, Mn.y, Mx.z}, {Mx.x, Mn.y, Mx.z}, {Mn.x, Mx.y, Mx.z}, {Mx.x, Mx.y, Mx.z},
                };
                CornersAABB(Entry.Prim.World, Corners, 8, Entry.AABBMin, Entry.AABBMax);
                Out.push_back(std::move(Entry));
            }

            auto CapsuleView = Context.CreateView<SCapsuleColliderComponent, STransformComponent>();
            for (ECS::FEntity E : CapsuleView)
            {
                SCapsuleColliderComponent& Cap = CapsuleView.Get<SCapsuleColliderComponent>(E);
                if (!Cap.bAffectsNavigation) continue;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Capsule);
                Entry.Prim.Type = ENavColliderType::Capsule;
                Entry.Prim.World = ColliderToWorld(CapsuleView.Get<STransformComponent>(E), Cap.TranslationOffset, Cap.RotationOffset);
                Entry.Prim.Shape = FVector3(Cap.Radius, Cap.HalfHeight, 0.0f);
                const FVector3 Top = FVector3(Entry.Prim.World * FVector4(0.0f,  Cap.HalfHeight, 0.0f, 1.0f));
                const FVector3 Bot = FVector3(Entry.Prim.World * FVector4(0.0f, -Cap.HalfHeight, 0.0f, 1.0f));
                const float R = ScaledRadius(Entry.Prim.World, Cap.Radius);
                Entry.AABBMin = Math::Min(Top, Bot) - FVector3(R);
                Entry.AABBMax = Math::Max(Top, Bot) + FVector3(R);
                Out.push_back(std::move(Entry));
            }

            auto CylinderView = Context.CreateView<SCylinderColliderComponent, STransformComponent>();
            for (ECS::FEntity E : CylinderView)
            {
                SCylinderColliderComponent& Cyl = CylinderView.Get<SCylinderColliderComponent>(E);
                if (!Cyl.bAffectsNavigation || IsSimulated(E)) continue;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Cylinder);
                Entry.Prim.Type = ENavColliderType::Cylinder;
                Entry.Prim.World = ColliderToWorld(CylinderView.Get<STransformComponent>(E), Cyl.TranslationOffset, Cyl.RotationOffset);
                Entry.Prim.Shape = FVector3(Cyl.Radius, Cyl.HalfHeight, 0.0f);
                const FVector3 Top = FVector3(Entry.Prim.World * FVector4(0.0f,  Cyl.HalfHeight, 0.0f, 1.0f));
                const FVector3 Bot = FVector3(Entry.Prim.World * FVector4(0.0f, -Cyl.HalfHeight, 0.0f, 1.0f));
                const float R = ScaledRadius(Entry.Prim.World, Cyl.Radius);
                Entry.AABBMin = Math::Min(Top, Bot) - FVector3(R);
                Entry.AABBMax = Math::Max(Top, Bot) + FVector3(R);
                Out.push_back(std::move(Entry));
            }

            // Character capsules are opt-in, since agents should not normally carve the navmesh.
            auto CharView = Context.CreateView<SCharacterPhysicsComponent, STransformComponent>();
            for (ECS::FEntity E : CharView)
            {
                SCharacterPhysicsComponent& Cap = CharView.Get<SCharacterPhysicsComponent>(E);
                if (!Cap.bAffectsNavigation) continue;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::CharacterCapsule);
                Entry.Prim.Type = ENavColliderType::CharacterCapsule;
                FMatrix4 CapsuleWorld = CharView.Get<STransformComponent>(E).GetWorldMatrix();
                CapsuleWorld[3] = CapsuleWorld * FVector4(Cap.TranslationOffset.x, Cap.TranslationOffset.y, Cap.TranslationOffset.z, 1.0f);
                Entry.Prim.World = CapsuleWorld;
                Entry.Prim.Shape = FVector3(Cap.Radius, Cap.HalfHeight, 0.0f);
                const FVector3 Top = FVector3(Entry.Prim.World * FVector4(0.0f,  Cap.HalfHeight, 0.0f, 1.0f));
                const FVector3 Bot = FVector3(Entry.Prim.World * FVector4(0.0f, -Cap.HalfHeight, 0.0f, 1.0f));
                const float R = ScaledRadius(Entry.Prim.World, Cap.Radius);
                Entry.AABBMin = Math::Min(Top, Bot) - FVector3(R);
                Entry.AABBMax = Math::Max(Top, Bot) + FVector3(R);
                Out.push_back(std::move(Entry));
            }

            // A compound body is several primitives under one transform, so each child is its own source.
            auto CompoundView = Context.CreateView<SCompoundColliderComponent, STransformComponent>();
            for (ECS::FEntity E : CompoundView)
            {
                const SCompoundColliderComponent& Compound = CompoundView.Get<SCompoundColliderComponent>(E);
                if (!Compound.bAffectsNavigation || IsSimulated(E)) continue;

                const FMatrix4 BodyWorld = CompoundView.Get<STransformComponent>(E).GetWorldMatrix();
                for (uint32 i = 0; i < (uint32)Compound.Shapes.size(); ++i)
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

                    Out.push_back(std::move(Entry));
                }
            }

            // Area modifiers carry no triangles; the bake stamps their footprint onto the voxelized surface.
            auto ModifierView = Context.CreateView<SNavModifierComponent, STransformComponent>();
            for (ECS::FEntity E : ModifierView)
            {
                const SNavModifierComponent& Modifier = ModifierView.Get<SNavModifierComponent>(E);
                if (!Modifier.bEnabled) continue;
                if (Modifier.Extents.x <= 0.0f || Modifier.Extents.y <= 0.0f || Modifier.Extents.z <= 0.0f) continue;

                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::AreaVolume, (uint32)Modifier.Area);
                Entry.Prim.Type = ENavColliderType::AreaVolume;
                Entry.Prim.World = ModifierView.Get<STransformComponent>(E).GetWorldMatrix() * Math::Translate(FMatrix4(1.0f), Modifier.Offset);

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
                Out.push_back(std::move(Entry));
            }

            // Off-mesh links are stitched in by the bake; each endpoint has to land on walkable surface.
            auto LinkView = Context.CreateView<SNavLinkComponent, STransformComponent>();
            for (ECS::FEntity E : LinkView)
            {
                const SNavLinkComponent& LinkComp = LinkView.Get<SNavLinkComponent>(E);
                if (!LinkComp.bEnabled) continue;

                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::OffMeshLink, PackLinkSubIndex(LinkComp));
                Entry.Prim.Type = ENavColliderType::OffMeshLink;
                Entry.Prim.World = LinkView.Get<STransformComponent>(E).GetWorldMatrix();

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
                Out.push_back(std::move(Entry));
            }

            // Terrain heightfield needs both the collider and the source component.
            auto TerrainView = Context.CreateView<STerrainColliderComponent, STransformComponent>();
            for (ECS::FEntity E : TerrainView)
            {
                STerrainColliderComponent& TC = TerrainView.Get<STerrainColliderComponent>(E);
                if (!TC.bAffectsNavigation) continue;
                const STerrainComponent* Terrain = Context.GetRegistry().TryGet<STerrainComponent>(E);
                if (!Terrain || Terrain->Heightmap.empty()) continue;
                FNavSourceEntry Entry;
                Entry.Key = PackSourceKey(E, ENavColliderType::Terrain);
                Entry.ContentId = Terrain->CPUState.HeightmapVersion;
                Entry.Prim.Type = ENavColliderType::Terrain;
                Entry.Prim.World = TerrainView.Get<STransformComponent>(E).GetWorldMatrix();
                const float Half = Terrain->TileWorldSize * 0.5f;
                const FVector3 Corners[8] = {
                    {-Half, 0.0f, -Half}, { Half, 0.0f, -Half}, { Half, 0.0f,  Half}, {-Half, 0.0f,  Half},
                    {-Half, Terrain->MaxHeight, -Half}, { Half, Terrain->MaxHeight, -Half}, { Half, Terrain->MaxHeight,  Half}, {-Half, Terrain->MaxHeight,  Half},
                };
                CornersAABB(Entry.Prim.World, Corners, 8, Entry.AABBMin, Entry.AABBMax);
                if (bTessellateTerrain)
                {
                    Entry.Prim.TriangleSoup = MakeShared<TVector<FVector3>>();
                    TessellateTerrain(*Terrain, Entry.Prim.World, BakeMin, BakeMax, CellSize, *Entry.Prim.TriangleSoup);
                }
                Out.push_back(std::move(Entry));
            }

            // Without this the caller's box only culls per triangle, after emit has already transformed
            // geometry it is about to throw away.
            Out.erase(Algo::RemoveIf(Out, [&BakeMin, &BakeMax](const FNavSourceEntry& Entry)
            {
                return Entry.AABBMin.x > BakeMax.x || Entry.AABBMax.x < BakeMin.x
                    || Entry.AABBMin.y > BakeMax.y || Entry.AABBMax.y < BakeMin.y
                    || Entry.AABBMin.z > BakeMax.z || Entry.AABBMax.z < BakeMin.z;
            }), Out.end());
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

        // Snapshot at bake completion so the next change-detector tick reports zero diff.
        // A finished bake holds exactly what is in the world now, so the change scan starts from it.
        void SeedChangeTracking(const FSystemContext& Context, SNavMeshComponent& Comp)
        {
            THashMap<uint64, FNavSourceEntity>& OutCache = Comp.Runtime.EntityAABBs;
            OutCache.clear();
            TVector<FNavSourceEntry> Sources;
            CollectNavSources(Context, Comp.Center - Comp.GetWorldExtents(), Comp.Center + Comp.GetWorldExtents(), false, 0.0f, Sources, nullptr);
            for (const FNavSourceEntry& Entry : Sources)
            {
                OutCache[Entry.Key] = FNavSourceEntity{ Entry.AABBMin, Entry.AABBMax, Entry.ContentId };
            }

            FNavFoliageCache& Cache = GetFoliageCache(Comp);
            Cache.Entities.clear();
            auto FoliageView = Context.CreateView<SFoliageComponent>();
            for (ECS::FEntity E : FoliageView)
            {
                FNavFoliageEntity& Entry = RefreshFoliageEntity(Cache, E, FoliageView.Get<SFoliageComponent>(E));
                Entry.bDetected         = true;
                Entry.DetectedSignature = Entry.Signature;
                Entry.DetectedMin       = Entry.Min;
                Entry.DetectedMax       = Entry.Max;
            }
            Comp.Runtime.bSourcesSeeded = true;
        }

        // One walk feeds geometry and annotations, so a full bake and a hot rebake see the same authored set.
        void SnapshotBuildInput(const FSystemContext& Context, SNavMeshComponent& Comp, FNavBuildInput& Out, TVector<FNavSourcePrim>& OutPrims)
        {
            Out.Settings  = Comp.Settings;
            Out.BoundsMin = Comp.Center - Comp.GetWorldExtents();
            Out.BoundsMax = Comp.Center + Comp.GetWorldExtents();

            TVector<FNavSourceEntry> Sources;
            CollectNavSources(Context, Out.BoundsMin, Out.BoundsMax, true, Comp.Settings.CellSize, Sources, &GetFoliageCache(Comp));

            OutPrims.reserve(Sources.size());
            for (FNavSourceEntry& Entry : Sources)
            {
                AppendAnnotation(Entry.Prim, Out.AreaVolumes, Out.Links);
                OutPrims.push_back(std::move(Entry.Prim));
            }
        }

        void TickComponent(const FSystemContext& Context, ECS::FEntity Entity, SNavMeshComponent& Comp)
        {
            // Scale always mirrors, location only in the editor, since runtime keeps the serialized Center.
            if (const STransformComponent* X = Context.GetRegistry().TryGet<STransformComponent>(Entity))
            {
                const FTransform& WT = X->GetWorldTransform();
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
                Comp.TilesX          = Out.TilesX;
                Comp.TilesY          = Out.TilesY;
                Comp.Runtime.LiveLayout = Out;
                Comp.Runtime.LiveLayout.Tiles.clear();
                Comp.Runtime.ActiveBake.reset();
                Comp.Runtime.DirtyTiles.clear();
                Comp.Runtime.SettlingTiles.clear();

                if (NonEmptyTiles == 0)
                {
                    // Fail explicitly, since leaving State=Building here read as stuck on Baking.
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
                    SeedChangeTracking(Context, Comp);
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

            // The bRuntimeDirty branch is essential, or a re-bake leaves the old Mesh stuck Building.
            // A failed hydration nulls Mesh, so without the state test it re-kicks forever.
            const bool bHydrateWanted = Comp.Runtime.bRuntimeDirty
                                     || (!Comp.Runtime.Mesh && Comp.Runtime.State != ENavBakeState::Failed);
            if (Comp.HasBakedData() && !Comp.Runtime.PendingInit && bHydrateWanted)
            {
                // Tiles can persist with every blob empty, and Detour cannot be handed nothing.
                const bool bAnyWalkableTile = Algo::AnyOf(Comp.Tiles,
                    [](const FNavTileData& Tile) { return !Tile.Blob.empty(); });
                if (!bAnyWalkableTile)
                {
                    LOG_ERROR("NavMesh has {} tiles but none carry walkable geometry, so there is nothing to hydrate. "
                              "Re-bake the volume, and check it covers ground the agent radius and slope accept.",
                        (int32)Comp.Tiles.size());
                    Comp.Runtime.bRuntimeDirty = false;
                    Comp.Runtime.State = ENavBakeState::Failed;
                    return;
                }

                Comp.Runtime.LiveLayout.Origin          = Comp.Origin;
                Comp.Runtime.LiveLayout.TileWorldSize   = Comp.TileWorldSize;
                Comp.Runtime.LiveLayout.TilesX          = Comp.TilesX;
                Comp.Runtime.LiveLayout.TilesY          = Comp.TilesY;
                Comp.Runtime.LiveLayout.MaxTiles        = (int32)Comp.Tiles.size();
                Comp.Runtime.LiveLayout.MaxPolysPerTile = Comp.MaxPolysPerTile;

                auto Job = MakeShared<FNavInitJob>();
                Comp.Runtime.PendingInit = Job;
                Comp.Runtime.bRuntimeDirty = false;
                Comp.Runtime.State = ENavBakeState::Initializing;

                // With streaming on the mesh starts empty and the streamer pages tiles in; otherwise it is
                // seeded with everything, which is what the whole-navmesh-resident path always did.
                const bool bStreaming = NavSettings().StreamLoadRadius > 0.0f;
                const int32 TileCount = (int32)Comp.Tiles.size();
                const int32 InitMaxTiles = bStreaming
                    ? Math::Min(Math::Max(1, NavSettings().ResidentTileBudget), Math::Max(1, TileCount))
                    : Math::Max(1, TileCount);

                // Copy tiles so worker owns its data; Comp.Tiles stays serialized source of truth.
                TVector<FNavTileData> TilesCopy;
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
                Task::AsyncTask(1, 1, [Job, Tiles = std::move(TilesCopy), InitOrigin, InitTileSize, InitMaxTiles, InitMaxPolys, bLogTimings](uint32, uint32, uint32) mutable
                {
                    const int32 NumHydratedTiles = (int32)Tiles.size();
                    PlatformTime::FStopwatch Watch;
                    auto Mesh = MakeUnique<FNavMesh>();
                    Mesh->Initialize(InitOrigin, InitTileSize, InitMaxTiles, InitMaxPolys, std::move(Tiles));
                    if (bLogTimings)
                    {
                        LOG_INFO("NavTiming hydrate: {} tiles, maxTiles={}, {:.1f} ms.", NumHydratedTiles, InitMaxTiles, Watch.ElapsedMilliseconds());
                    }
                    Job->ResultMesh = std::move(Mesh);
                    Job->bDone.store(true, std::memory_order_release);
                }, ETaskPriority::Background);

                // Cache left empty, so a source loaded before a serialized bake reads as new and gets its tiles.
                Comp.Runtime.DirtyTiles.clear();
                Comp.Runtime.SettlingTiles.clear();
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
                    if (const FSignificanceState* Significance = Significance::GetState(Context))
                    {
                        if (Significance->bHasView)
                        {
                            Focus.push_back(Significance->ViewOrigin);
                        }
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

                // Swapping in a tile the streamer never paged in would leave it resident and unevictable.
                const bool bStreamerOwnsResidency = Comp.Runtime.bStreamedInit;
                if (!bStreamerOwnsResidency || Comp.Runtime.Streamer.IsResident(Job->TileX, Job->TileY))
                {
                    Comp.Runtime.Mesh->RebuildTile(Job->TileX, Job->TileY, std::move(Job->ResultBlob));
                }
                Job->bConsumed.store(true, std::memory_order_release);
            }
            
            Comp.Runtime.PendingRebakes.erase(
                Algo::RemoveIf(Comp.Runtime.PendingRebakes,
                    [](const TSharedPtr<FNavTileRebake>& J) { return !J || J->bConsumed.load(std::memory_order_acquire); }),
                Comp.Runtime.PendingRebakes.end());

            // Held between scans so a long interval still measures real elapsed time.
            Comp.Runtime.DynamicScanTimer = Comp.bDynamicRebuild
                ? Comp.Runtime.DynamicScanTimer + (float)Context.GetDeltaTime()
                : 0.0f;

            // Tiles dirtied before the setting was turned off still rebake below.
            if (Comp.bDynamicRebuild && Comp.Runtime.DynamicScanTimer >= Comp.DynamicRebuildInterval)
            {
                Comp.Runtime.DynamicScanTimer = 0.0f;

                // What the previous scan dirtied has now been seen twice, so it is ready to rebake.
                for (uint64 TileKey : Comp.Runtime.SettlingTiles)
                {
                    Comp.Runtime.DirtyTiles.insert(TileKey);
                }
                Comp.Runtime.SettlingTiles.clear();

                // Detect moved, reshaped, added and removed source geometry and dirty its tiles.
                THashMap<uint64, FNavSourceEntity> CurrentAABBs;
                CurrentAABBs.reserve(Comp.Runtime.EntityAABBs.size());

                auto MarkDirtyForAABB = [&](const FVector3& Mn, const FVector3& Mx)
                {
                    int32 TX0, TY0, TX1, TY1;
                    TilesForAABB(Mn, Mx, Comp.Origin, Comp.TileWorldSize, Comp.TilesX, Comp.TilesY, TX0, TY0, TX1, TY1);
                    for (int32 ty = TY0; ty <= TY1; ++ty)
                    {
                        for (int32 tx = TX0; tx <= TX1; ++tx)
                        {
                            // A tile already waiting rebakes from the geometry as it stands when it is kicked, so it needs no second pass.
                            const uint64 TileKey = PackTileKey(tx, ty);
                            if (!Comp.Runtime.DirtyTiles.contains(TileKey))
                            {
                                Comp.Runtime.SettlingTiles.insert(TileKey);
                            }
                        }
                    }
                };

                // Counted per cause and collider type, so a timing log can say what keeps dirtying tiles.
                constexpr int32 kTypeSlots = 16;
                int32 CauseCounts[4][kTypeSlots] = {};
                auto CountCause = [&](int32 Cause, uint64 Key)
                {
                    ++CauseCounts[Cause][Math::Min<int32>((int32)(Key & 0xFF), kTypeSlots - 1)];
                };

                const bool bAdopt = !Comp.Runtime.bSourcesSeeded;

                auto VisitSource = [&](uint64 Key, const FVector3& Mn, const FVector3& Mx, uint64 ContentId)
                {
                    CurrentAABBs[Key] = FNavSourceEntity{ Mn, Mx, ContentId };
                    auto It = Comp.Runtime.EntityAABBs.find(Key);
                    const bool bNew   = It == Comp.Runtime.EntityAABBs.end();

                    if (bAdopt)
                    {
                        return;
                    }
                    const bool bMoved = !bNew && (!Math::IsNearlyEqual(It->second.AABBMin, Mn) || !Math::IsNearlyEqual(It->second.AABBMax, Mx));

                    // A re-imported, swapped or sculpted mesh keeps its bounds, so the AABB test alone misses it.
                    const bool bReshaped = !bNew && It->second.ContentId != ContentId;

                    if (bNew || bMoved || bReshaped)
                    {
                        CountCause(bNew ? 0 : bMoved ? 1 : 2, Key);
                        if (bMoved || bReshaped)
                        {
                            // Old footprint also dirtied so vacated tris get re-evaluated.
                            MarkDirtyForAABB(It->second.AABBMin, It->second.AABBMax);
                        }
                        MarkDirtyForAABB(Mn, Mx);
                    }
                };

                TVector<FNavSourceEntry> CurrentSources;
                PlatformTime::FStopwatch DetectWatch;
                CollectNavSources(Context, Comp.Center - Comp.GetWorldExtents(), Comp.Center + Comp.GetWorldExtents(), false, 0.0f, CurrentSources, nullptr);
                const int32 DirtyBefore = (int32)(Comp.Runtime.DirtyTiles.size() + Comp.Runtime.SettlingTiles.size());
                for (const FNavSourceEntry& Src : CurrentSources)
                {
                    VisitSource(Src.Key, Src.AABBMin, Src.AABBMax, Src.ContentId);
                }

                // Removed colliders dirty their last-known tiles.
                for (const auto& [Id, Snap] : Comp.Runtime.EntityAABBs)
                {
                    if (!bAdopt && CurrentAABBs.find(Id) == CurrentAABBs.end())
                    {
                        CountCause(3, Id);
                        MarkDirtyForAABB(Snap.AABBMin, Snap.AABBMax);
                    }
                }

                // Foliage is diffed per entity, so a scan where nothing changed never walks its instances.
                FNavFoliageCache& FoliageCache = GetFoliageCache(Comp);
                for (auto& [Id, Cached] : FoliageCache.Entities)
                {
                    Cached.bSeen = false;
                }
                const uint64 FoliageKey = (uint64)ENavColliderType::Foliage;
                auto FoliageView = Context.CreateView<SFoliageComponent>();
                for (ECS::FEntity E : FoliageView)
                {
                    FNavFoliageEntity& Cached = RefreshFoliageEntity(FoliageCache, E, FoliageView.Get<SFoliageComponent>(E));
                    if (Cached.bDetected && Cached.DetectedSignature == Cached.Signature)
                    {
                        continue;
                    }

                    if (!bAdopt)
                    {
                        const size_t Count = Math::Max(Cached.Min.size(), Cached.DetectedMin.size());
                        for (size_t i = 0; i < Count; ++i)
                        {
                            const bool bWas = i < Cached.DetectedMin.size() && HasFoliageBounds(Cached.DetectedMin[i], Cached.DetectedMax[i]);
                            const bool bIs  = i < Cached.Min.size() && HasFoliageBounds(Cached.Min[i], Cached.Max[i]);
                            if (!bWas && !bIs)
                            {
                                continue;
                            }
                            if (bWas && bIs && Math::IsNearlyEqual(Cached.DetectedMin[i], Cached.Min[i]) && Math::IsNearlyEqual(Cached.DetectedMax[i], Cached.Max[i]))
                            {
                                continue;
                            }

                            CountCause(bWas ? (bIs ? 1 : 3) : 0, FoliageKey);
                            if (bWas)
                            {
                                MarkDirtyForAABB(Cached.DetectedMin[i], Cached.DetectedMax[i]);
                            }
                            if (bIs)
                            {
                                MarkDirtyForAABB(Cached.Min[i], Cached.Max[i]);
                            }
                        }
                    }

                    Cached.bDetected         = true;
                    Cached.DetectedSignature = Cached.Signature;
                    Cached.DetectedMin       = Cached.Min;
                    Cached.DetectedMax       = Cached.Max;
                }

                for (auto It = FoliageCache.Entities.begin(); It != FoliageCache.Entities.end();)
                {
                    FNavFoliageEntity& Cached = It->second;
                    if (Cached.bSeen)
                    {
                        ++It;
                        continue;
                    }
                    if (!bAdopt)
                    {
                        for (size_t i = 0; i < Cached.DetectedMin.size(); ++i)
                        {
                            if (HasFoliageBounds(Cached.DetectedMin[i], Cached.DetectedMax[i]))
                            {
                                CountCause(3, FoliageKey);
                                MarkDirtyForAABB(Cached.DetectedMin[i], Cached.DetectedMax[i]);
                            }
                        }
                    }
                    It = FoliageCache.Entities.erase(It);
                }
                Comp.Runtime.bSourcesSeeded = true;
                const double DetectGatherMs = DetectWatch.ElapsedMilliseconds();

                if (CVarNavTimings.GetValue())
                {
                    const int32 DirtyAfter = (int32)(Comp.Runtime.DirtyTiles.size() + Comp.Runtime.SettlingTiles.size());

                    // Logged on a steady state too, since "nothing dirtied" is the answer when a scan finds
                    // no sources or the volume sits somewhere the geometry is not.
                    static double LastIdleLogSeconds = 0.0;
                    const double NowSeconds = Context.GetTime();
                    const bool bQuiet = DirtyAfter == DirtyBefore && DetectGatherMs <= 1.0;
                    if (!bQuiet || NowSeconds - LastIdleLogSeconds >= 2.0)
                    {
                        if (bQuiet)
                        {
                            LastIdleLogSeconds = NowSeconds;
                        }
                        const FVector3 WExt = Comp.GetWorldExtents();
                        LOG_INFO("NavTiming detect [{}]: {} sources in {:.2f} ms, dirty {} -> {}, pending {}, "
                                 "box=({:.1f},{:.1f},{:.1f})+/-({:.1f},{:.1f},{:.1f}), origin=({:.1f},{:.1f},{:.1f}), tracked {}.",
                            Context.GetWorldType() == EWorldType::Editor ? "editor" : "play",
                            (int32)CurrentSources.size(), DetectGatherMs, DirtyBefore, DirtyAfter,
                            (int32)Comp.Runtime.PendingRebakes.size(),
                            Comp.Center.x, Comp.Center.y, Comp.Center.z, WExt.x, WExt.y, WExt.z,
                            Comp.Origin.x, Comp.Origin.y, Comp.Origin.z,
                            (int32)Comp.Runtime.EntityAABBs.size());

                        if (DirtyAfter != DirtyBefore)
                        {
                            const char* CauseNames[4] = { "new", "moved", "reshaped", "removed" };
                            const char* TypeNames[kTypeSlots] = { "Box", "Sphere", "Mesh", "CharacterCapsule", "Capsule", "Cylinder", "Terrain",
                                                                  "TriangleSoup", "DynamicMesh", "AreaVolume", "OffMeshLink", "Foliage", "?", "?", "?", "?" };
                            FString Causes;
                            for (int32 Cause = 0; Cause < 4; ++Cause)
                            {
                                int32 Total = 0;
                                int32 TopType = 0;
                                for (int32 Type = 0; Type < kTypeSlots; ++Type)
                                {
                                    Total += CauseCounts[Cause][Type];
                                    TopType = CauseCounts[Cause][Type] > CauseCounts[Cause][TopType] ? Type : TopType;
                                }
                                if (Total > 0)
                                {
                                    Causes += Format(" {} {} (mostly {})", CauseNames[Cause], Total, TypeNames[TopType]);
                                }
                            }
                            LOG_INFO("NavTiming detect causes:{}", Causes.c_str());
                        }
                    }
                }

                Comp.Runtime.EntityAABBs = std::move(CurrentAABBs);
            }

            // With scanning off nothing would promote them, so the last scan's tiles go straight through.
            if (!Comp.bDynamicRebuild && !Comp.Runtime.SettlingTiles.empty())
            {
                for (uint64 TileKey : Comp.Runtime.SettlingTiles)
                {
                    Comp.Runtime.DirtyTiles.insert(TileKey);
                }
                Comp.Runtime.SettlingTiles.clear();
            }

            // Cap concurrent rebake jobs; remaining dirty tiles wait for next tick.
            const uint32 MaxConcurrent = (uint32)Math::Max(1, NavSettings().MaxConcurrentTileRebakes);
            if (Comp.Runtime.DirtyTiles.empty() || Comp.Runtime.PendingRebakes.size() >= MaxConcurrent)
            {
                return;
            }

            // Main thread only snapshots params + world matrices; tessellation runs on a worker.
            const uint32 Capacity = MaxConcurrent - (uint32)Comp.Runtime.PendingRebakes.size();

            // A batch gathers the union of its tiles, so scattered tiles re-tessellate everything between.
            TVector<uint64> Candidates(Comp.Runtime.DirtyTiles.begin(), Comp.Runtime.DirtyTiles.end());
            int32 SeedX, SeedY;
            NavTile::UnpackKey(Candidates[0], SeedX, SeedY);
            auto SeedDistance = [SeedX, SeedY](uint64 Key)
            {
                int32 X, Y;
                NavTile::UnpackKey(Key, X, Y);
                return Math::Max(Math::Abs(X - SeedX), Math::Abs(Y - SeedY));
            };
            size_t Take = Math::Min((size_t)Capacity, Candidates.size());
            std::partial_sort(Candidates.begin(), Candidates.begin() + Take, Candidates.end(),
                [&SeedDistance](uint64 A, uint64 B) { return SeedDistance(A) < SeedDistance(B); });

            // Tiles past this radius wait for a batch seeded near them, since one far tile widens the gather to everything in between.
            constexpr int32 kMaxBatchRadiusTiles = 4;
            while (Take > 1 && SeedDistance(Candidates[Take - 1]) > kMaxBatchRadiusTiles)
            {
                --Take;
            }

            TVector<TSharedPtr<FNavTileRebake>> BatchJobs;
            BatchJobs.reserve(Take);
            for (size_t i = 0; i < Take; ++i)
            {
                Comp.Runtime.DirtyTiles.erase(Candidates[i]);

                auto Job = MakeShared<FNavTileRebake>();
                NavTile::UnpackKey(Candidates[i], Job->TileX, Job->TileY);
                BatchJobs.push_back(Job);
                Comp.Runtime.PendingRebakes.push_back(std::move(Job));
            }

            // Terrain is pre-tessellated on this thread; the worker re-emits and bakes the dirty tiles.
            struct FInputSnapshot
            {
                TVector<FNavSourcePrim> Prims;
                FVector3                BakeMin;
                FVector3                BakeMax;
                FVector3                GatherMin;
                FVector3                GatherMax;
                FNavBuildSettings       Settings;
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

            PlatformTime::FStopwatch GatherWatch;
            {
                TVector<FNavSourceEntry> Sources;
                CollectNavSources(Context, Snap->GatherMin, Snap->GatherMax, true, Comp.Settings.CellSize, Sources, &GetFoliageCache(Comp));
                Snap->Prims.reserve(Sources.size());
                for (FNavSourceEntry& Entry : Sources)
                {
                    Snap->Prims.push_back(std::move(Entry.Prim));
                }
            }
            const bool bLogRebakeTimings = CVarNavTimings.GetValue();
            if (bLogRebakeTimings)
            {
                const FVector3 GatherSpan = Snap->GatherMax - Snap->GatherMin;
                LOG_INFO("NavTiming rebake gather: {} tiles, box {:.0f}x{:.0f}, {} prims in {:.2f} ms, {} still dirty.",
                    (int32)BatchJobs.size(), GatherSpan.x, GatherSpan.z, (int32)Snap->Prims.size(),
                    GatherWatch.ElapsedMilliseconds(), (int32)Comp.Runtime.DirtyTiles.size());
            }

            // Coordinator emits geometry once on a worker, then ParallelFors the per-tile bakes.
            Task::AsyncTask(1, 1, [Snap, Jobs = std::move(BatchJobs), bLogRebakeTimings](uint32, uint32, uint32) mutable
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
                EmitNavSourcePrims(Snap->Prims.data(), Snap->Prims.size(), Snap->GatherMin, Snap->GatherMax,
                    Input.Vertices, Input.Indices);

                // One binning pass feeds every dirty tile, rather than each tile rescanning the soup.
                TVector<FNavTileCoord> Coords;
                Coords.reserve(Jobs.size());
                for (const TSharedPtr<FNavTileRebake>& Job : Jobs)
                {
                    Coords.push_back(FNavTileCoord{ Job->TileX, Job->TileY });
                }

                const double EmitMs = EmitWatch.ElapsedMilliseconds();
                const int32 EmittedTris = (int32)(Input.Indices.size() / 3);

                PlatformTime::FStopwatch BakeWatch;
                TVector<FNavTileData> Baked;
                NavMeshBuilder::BakeTiles(Input, Snap->Layout, Coords, Baked);
                if (bLogRebakeTimings)
                {
                    LOG_INFO("NavTiming rebake work: {} tiles, emit {} tris in {:.2f} ms, bake {:.2f} ms.",
                        (int32)Coords.size(), EmittedTris, EmitMs, BakeWatch.ElapsedMilliseconds());
                }

                for (size_t i = 0; i < Jobs.size(); ++i)
                {
                    if (i < Baked.size())
                    {
                        Jobs[i]->ResultBlob = std::move(Baked[i].Blob);
                    }
                    Jobs[i]->bDone.store(true, std::memory_order_release);
                }
            }, ETaskPriority::Background);

            (void)Entity;
        }

        template<typename TView>
        FNavMesh* FindReadyNavMesh(TView& View, FName Agent)
        {
            FNavMesh* Fallback = nullptr;
            for (ECS::FEntity Entity : View)
            {
                SNavMeshComponent& Comp = View.template Get<SNavMeshComponent>(Entity);
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

        FNavMesh* FirstReadyNavMesh(const FSystemContext& Context, FName Agent = FName())
        {
            auto View = Context.CreateView<SNavMeshComponent>();
            return FindReadyNavMesh(View, Agent);
        }
    }

    namespace
    {
        // Initializes Center from entity transform so bake volume defaults at entity location.
        void OnNavMeshConstructed(ECS::FRegistry& Reg, ECS::FEntity Entity)
        {
            SNavMeshComponent& Nav = Reg.Get<SNavMeshComponent>(Entity);
            if (auto* Xform = Reg.TryGet<STransformComponent>(Entity))
            {
                Nav.Center = Xform->GetWorldTransform().GetLocation();
            }
        }
    }

    void SNavMeshSystem::OnStartup()
    {
        const FSystemContext& Context = GetContext();

        LUMINA_PROFILE_SCOPE();

        // A sink binds by handler identity, so reconnecting the same one is a no-op.
        Context.GetRegistry().GetSignals<SNavMeshComponent>().OnConstruct.Connect<&OnNavMeshConstructed>();

        auto View = Context.CreateView<SNavMeshComponent>();
        for (ECS::FEntity Entity : View)
        {
            TickComponent(Context, Entity, View.Get<SNavMeshComponent>(Entity));
        }
    }

    void SNavMeshSystem::OnUpdate()
    {
        const FSystemContext& Context = GetContext();

        LUMINA_PROFILE_SCOPE();
        auto View = Context.CreateView<SNavMeshComponent>();
        for (ECS::FEntity Entity : View)
        {
            TickComponent(Context, Entity, View.Get<SNavMeshComponent>(Entity));
        }
    }

    void SNavMeshSystem::OnTeardown()
    {
        const FSystemContext& Context = GetContext();

        LUMINA_PROFILE_SCOPE();
        auto View = Context.CreateView<SNavMeshComponent>();
        for (ECS::FEntity Entity : View)
        {
            SNavMeshComponent& Comp = View.Get<SNavMeshComponent>(Entity);
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
            Comp.Runtime.SettlingTiles.clear();
            Comp.Runtime.EntityAABBs.clear();
            Comp.Runtime.FoliageCache.reset();
            Comp.Runtime.FoliageCache.reset();
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

        // The snapshot reads the registry so it stays here, while emitting its triangles moves to the bake worker.
        FNavBuildInput Input;
        TVector<FNavSourcePrim> Prims;
        SnapshotBuildInput(Context, Comp, Input, Prims);

        // EntityAABB cache populated at bake-completion drain (avoids tight-vs-conservative AABB mismatch storm).
        Comp.Runtime.EntityAABBs.clear();
        Comp.Runtime.FoliageCache.reset();
        Comp.Runtime.FoliageCache.reset();
        Comp.Runtime.DirtyTiles.clear();
        Comp.Runtime.SettlingTiles.clear();
        Comp.Runtime.PendingRebakes.clear();
        Comp.Runtime.ActiveBake = NavMeshBuilder::Bake(std::move(Input), [Prims = std::move(Prims)](FNavBuildInput& In)
        {
            EmitNavSourcePrims(Prims.data(), Prims.size(), In.BoundsMin, In.BoundsMax, In.Vertices, In.Indices);
            if (In.Vertices.empty() || In.Indices.empty())
            {
                LOG_WARN("NavMesh bake starting with no source geometry inside bounds. The result will be an empty navmesh; check that static meshes (non-character-controller) overlap the bounds volume.");
            }
            else
            {
                LOG_INFO("NavMesh bake starting: {} verts, {} tris, bounds=({:.1f},{:.1f},{:.1f})..({:.1f},{:.1f},{:.1f}).",
                    (int32)In.Vertices.size(), (int32)(In.Indices.size() / 3),
                    In.BoundsMin.x, In.BoundsMin.y, In.BoundsMin.z,
                    In.BoundsMax.x, In.BoundsMax.y, In.BoundsMax.z);
            }
        });
        Comp.Runtime.State = ENavBakeState::Building;
    }

    namespace Nav
    {
        FNavMesh* GetReadyNavMesh(const FSystemContext& Context, FName Agent)
        {
            return FirstReadyNavMesh(Context, Agent);
        }

        bool FindPath(const FSystemContext& Context, const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, FNavPath& Out, FName Agent)
        {
            FNavMesh* Mesh = FirstReadyNavMesh(Context, Agent);
            if (!Mesh)
            {
                // Reset rather than leave a caller's reused path reading as its previous outcome.
                Out = {};
                Out.Result = ENavPathResult::NoNavMesh;
                return false;
            }
            return Mesh->FindPath(Start, End, Filter, Out);
        }

        bool ProjectPoint(const FSystemContext& Context, const FVector3& World, const FVector3& Extents, const FNavQueryFilter& Filter, FVector3& Out, FName Agent)
        {
            FNavMesh* Mesh = FirstReadyNavMesh(Context, Agent);
            return Mesh && Mesh->ProjectPoint(World, Extents, Filter, Out);
        }

        bool Raycast(const FSystemContext& Context, const FVector3& Start, const FVector3& End, const FNavQueryFilter& Filter, FNavRaycastResult& Out, FName Agent)
        {
            FNavMesh* Mesh = FirstReadyNavMesh(Context, Agent);
            return Mesh && Mesh->Raycast(Start, End, Filter, Out);
        }

        namespace
        {
            FNavMesh* FirstReadyNavMeshFromWorld(CWorld* World, FName Agent = FName())
            {
                if (!World) return nullptr;
                auto View = World->View<SNavMeshComponent>();
                return FindReadyNavMesh(View, Agent);
            }
        }

        bool IsReady(CWorld* World, FName Agent)
        {
            return FirstReadyNavMeshFromWorld(World, Agent) != nullptr;
        }

        int32 RequestRebuild(CWorld* World)
        {
            if (!World)
            {
                return 0;
            }
            int32 Count = 0;
            auto View = World->View<SNavMeshComponent>();
            for (ECS::FEntity E : View)
            {
                View.Get<SNavMeshComponent>(E).bBakeRequested = true;
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
            FNavMesh* Mesh = FirstReadyNavMeshFromWorld(World, Agent);
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
            FNavMesh* Mesh = FirstReadyNavMeshFromWorld(World, Agent);
            FNavQueryFilter Filter;
            return Mesh && Mesh->ProjectPoint(Point, Extents, Filter, Out);
        }

        bool Raycast(CWorld* World, const FVector3& Start, const FVector3& End, FNavRaycastResult& Out, FName Agent)
        {
            FNavMesh* Mesh = FirstReadyNavMeshFromWorld(World, Agent);
            FNavQueryFilter Filter;
            return Mesh && Mesh->Raycast(Start, End, Filter, Out);
        }

        bool IsWalkableLine(CWorld* World, const FVector3& From, const FVector3& To, FName Agent)
        {
            FNavRaycastResult Result;
            return Raycast(World, From, To, Result, Agent) && !Result.bHit;
        }

        bool FindRandomReachablePoint(CWorld* World, const FVector3& Origin, float Radius, FVector3& Out, FName Agent)
        {
            FNavMesh* Mesh = FirstReadyNavMeshFromWorld(World, Agent);
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

        bool DrawDebugPath(CWorld* World, const FVector3& From, const FVector3& To, const FVector4& Color, float Duration, FName Agent)
        {
            FNavPath Path;
            if (!FindPath(World, From, To, Path, Agent) || !Path.bValid) return false;
            DrawPath(World, Path, Color, 3.0f, 0.15f, Duration);
            return true;
        }
    }
}
