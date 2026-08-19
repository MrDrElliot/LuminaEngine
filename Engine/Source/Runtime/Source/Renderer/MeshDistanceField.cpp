#include "RuntimePCH.h"
#include "MeshDistanceField.h"

#include <cfloat>
#include "MeshData.h"
#include "Core/Math/Math.h"
#include "Core/Progress/SlowTask.h"
#include "Log/Log.h"
#include "Memory/MemoryTracking.h"
#include "TaskSystem/TaskSystem.h"
#include "Renderer/MeshQuantization.h"

namespace Lumina::DistanceField
{
    namespace
    {
        constexpr uint32 kLeafTriangles = 8;

        constexpr uint32 kSignRayCount = 16;

        struct FTriangle
        {
            FVector3 V0;
            FVector3 V1;
            FVector3 V2;
        };

        struct FBVHNode
        {
            FVector3 Min;
            FVector3 Max;
            // Interior: index of the first child (second is First + 1). Leaf: index of the first triangle.
            uint32   First = 0;
            // 0 marks an interior node.
            uint32   Count = 0;
        };

        struct FBVH
        {
            TVector<FBVHNode>  Nodes;
            TVector<FTriangle> Triangles;
        };

        FORCEINLINE void GrowBounds(FVector3& Min, FVector3& Max, const FVector3& P)
        {
            Min = Math::Min(Min, P);
            Max = Math::Max(Max, P);
        }

        void BuildNode(FBVH& BVH, uint32 NodeIndex, uint32 Begin, uint32 End)
        {
            FBVHNode& Node = BVH.Nodes[NodeIndex];

            Node.Min = FVector3(FLT_MAX);
            Node.Max = FVector3(-FLT_MAX);
            for (uint32 i = Begin; i < End; ++i)
            {
                const FTriangle& T = BVH.Triangles[i];
                GrowBounds(Node.Min, Node.Max, T.V0);
                GrowBounds(Node.Min, Node.Max, T.V1);
                GrowBounds(Node.Min, Node.Max, T.V2);
            }

            const uint32 Count = End - Begin;
            if (Count <= kLeafTriangles)
            {
                Node.First = Begin;
                Node.Count = Count;
                return;
            }

            FVector3 CentroidMin(FLT_MAX);
            FVector3 CentroidMax(-FLT_MAX);
            for (uint32 i = Begin; i < End; ++i)
            {
                const FTriangle& T = BVH.Triangles[i];
                GrowBounds(CentroidMin, CentroidMax, (T.V0 + T.V1 + T.V2) * (1.0f / 3.0f));
            }

            const FVector3 Spread = CentroidMax - CentroidMin;
            int32 Axis = 0;
            if (Spread.y > Spread[Axis]) { Axis = 1; }
            if (Spread.z > Spread[Axis]) { Axis = 2; }

            if (Spread[Axis] <= 0.0f)
            {
                Node.First = Begin;
                Node.Count = Count;
                return;
            }

            const uint32 Mid = Begin + Count / 2u;
            std::nth_element(
                BVH.Triangles.begin() + Begin,
                BVH.Triangles.begin() + Mid,
                BVH.Triangles.begin() + End,
                [Axis](const FTriangle& A, const FTriangle& B)
                {
                    return (A.V0[Axis] + A.V1[Axis] + A.V2[Axis]) < (B.V0[Axis] + B.V1[Axis] + B.V2[Axis]);
                });

            const uint32 LeftIndex = (uint32)BVH.Nodes.size();
            BVH.Nodes.push_back(FBVHNode{});
            BVH.Nodes.push_back(FBVHNode{});

            // Re-fetched by index: the two push_backs above can reallocate, which invalidates Node.
            BVH.Nodes[NodeIndex].First = LeftIndex;
            BVH.Nodes[NodeIndex].Count = 0;

            BuildNode(BVH, LeftIndex,      Begin, Mid);
            BuildNode(BVH, LeftIndex + 1u, Mid,   End);
        }

        void BuildBVH(FBVH& BVH)
        {
            LUMINA_PROFILE_SCOPE();

            // Worst case for a median-split binary tree over N leaves of kLeafTriangles.
            BVH.Nodes.reserve((BVH.Triangles.size() / kLeafTriangles + 1u) * 4u);
            BVH.Nodes.push_back(FBVHNode{});
            BuildNode(BVH, 0u, 0u, (uint32)BVH.Triangles.size());
        }

        // Squared distance from P to an AABB; 0 when inside. The traversal prune.
        FORCEINLINE float DistanceSquaredToBox(const FVector3& P, const FVector3& Min, const FVector3& Max)
        {
            const FVector3 D = Math::Max(Math::Max(Min - P, P - Max), FVector3(0.0f));
            return Math::Dot(D, D);
        }

        FVector3 ClosestPointOnTriangle(const FVector3& P, const FTriangle& T)
        {
            const FVector3 AB = T.V1 - T.V0;
            const FVector3 AC = T.V2 - T.V0;
            const FVector3 AP = P - T.V0;

            const float D1 = Math::Dot(AB, AP);
            const float D2 = Math::Dot(AC, AP);
            if (D1 <= 0.0f && D2 <= 0.0f)
            {
                return T.V0;
            }

            const FVector3 BP = P - T.V1;
            const float D3 = Math::Dot(AB, BP);
            const float D4 = Math::Dot(AC, BP);
            if (D3 >= 0.0f && D4 <= D3)
            {
                return T.V1;
            }

            const float VC = D1 * D4 - D3 * D2;
            if (VC <= 0.0f && D1 >= 0.0f && D3 <= 0.0f)
            {
                const float V = D1 / (D1 - D3);
                return T.V0 + AB * V;
            }

            const FVector3 CP = P - T.V2;
            const float D5 = Math::Dot(AB, CP);
            const float D6 = Math::Dot(AC, CP);
            if (D6 >= 0.0f && D5 <= D6)
            {
                return T.V2;
            }

            const float VB = D5 * D2 - D1 * D6;
            if (VB <= 0.0f && D2 >= 0.0f && D6 <= 0.0f)
            {
                const float W = D2 / (D2 - D6);
                return T.V0 + AC * W;
            }

            const float VA = D3 * D6 - D5 * D4;
            if (VA <= 0.0f && (D4 - D3) >= 0.0f && (D5 - D6) >= 0.0f)
            {
                const float W = (D4 - D3) / ((D4 - D3) + (D5 - D6));
                return T.V1 + (T.V2 - T.V1) * W;
            }

            const float Denom = 1.0f / (VA + VB + VC);
            return T.V0 + AB * (VB * Denom) + AC * (VC * Denom);
        }

        float ClosestDistanceSquared(const FBVH& BVH, const FVector3& P)
        {
            float Best = FLT_MAX;

            uint32 Stack[64];
            uint32 StackSize = 0;
            Stack[StackSize++] = 0u;

            while (StackSize > 0)
            {
                const FBVHNode& Node = BVH.Nodes[Stack[--StackSize]];

                if (DistanceSquaredToBox(P, Node.Min, Node.Max) >= Best)
                {
                    continue;
                }

                if (Node.Count > 0)
                {
                    for (uint32 i = 0; i < Node.Count; ++i)
                    {
                        const FTriangle& T = BVH.Triangles[Node.First + i];
                        const FVector3 Q = ClosestPointOnTriangle(P, T);
                        Best = Math::Min(Best, Math::DistanceSquared(P, Q));
                    }
                    continue;
                }

                const uint32 L = Node.First;
                const uint32 R = Node.First + 1u;
                const float DL = DistanceSquaredToBox(P, BVH.Nodes[L].Min, BVH.Nodes[L].Max);
                const float DR = DistanceSquaredToBox(P, BVH.Nodes[R].Min, BVH.Nodes[R].Max);

                // Farther child pushed first so the nearer one pops next.
                if (DL < DR)
                {
                    if (DR < Best && StackSize < 64u) { Stack[StackSize++] = R; }
                    if (DL < Best && StackSize < 64u) { Stack[StackSize++] = L; }
                }
                else
                {
                    if (DL < Best && StackSize < 64u) { Stack[StackSize++] = L; }
                    if (DR < Best && StackSize < 64u) { Stack[StackSize++] = R; }
                }
            }

            return Best;
        }

        FORCEINLINE bool RayTriangle(const FVector3& Origin, const FVector3& Dir, const FTriangle& T,
                                     float& OutT, float& OutFacing)
        {
            const FVector3 E1 = T.V1 - T.V0;
            const FVector3 E2 = T.V2 - T.V0;
            const FVector3 Pv = Math::Cross(Dir, E2);
            const float Det = Math::Dot(E1, Pv);

            if (Math::Abs(Det) < 1e-12f)
            {
                return false;
            }

            const float InvDet = 1.0f / Det;
            const FVector3 Tv = Origin - T.V0;
            const float U = Math::Dot(Tv, Pv) * InvDet;
            if (U < 0.0f || U > 1.0f)
            {
                return false;
            }

            const FVector3 Qv = Math::Cross(Tv, E1);
            const float V = Math::Dot(Dir, Qv) * InvDet;
            if (V < 0.0f || U + V > 1.0f)
            {
                return false;
            }

            const float Hit = Math::Dot(E2, Qv) * InvDet;
            if (Hit <= 1e-6f)
            {
                return false;
            }

            OutT = Hit;
            OutFacing = Math::Dot(Dir, Math::Cross(E1, E2));
            return true;
        }

        FORCEINLINE bool RayBox(const FVector3& Origin, const FVector3& InvDir, const FVector3& Min,
                                const FVector3& Max, float Limit)
        {
            const FVector3 T0 = (Min - Origin) * InvDir;
            const FVector3 T1 = (Max - Origin) * InvDir;
            const FVector3 Lo = Math::Min(T0, T1);
            const FVector3 Hi = Math::Max(T0, T1);

            const float Near = Math::Max(Math::Max(Lo.x, Lo.y), Math::Max(Lo.z, 0.0f));
            const float Far  = Math::Min(Math::Min(Hi.x, Hi.y), Math::Min(Hi.z, Limit));
            return Near <= Far;
        }

        /** True when the first surface the ray meets is a backface, i.e. this ray votes "inside". */
        bool RayHitsBackface(const FBVH& BVH, const FVector3& Origin, const FVector3& Dir)
        {
            const FVector3 InvDir(1.0f / Dir.x, 1.0f / Dir.y, 1.0f / Dir.z);

            float BestT = FLT_MAX;
            float BestFacing = 0.0f;

            uint32 Stack[64];
            uint32 StackSize = 0;
            Stack[StackSize++] = 0u;

            while (StackSize > 0)
            {
                const FBVHNode& Node = BVH.Nodes[Stack[--StackSize]];

                if (!RayBox(Origin, InvDir, Node.Min, Node.Max, BestT))
                {
                    continue;
                }

                if (Node.Count > 0)
                {
                    for (uint32 i = 0; i < Node.Count; ++i)
                    {
                        float T, Facing;
                        if (RayTriangle(Origin, Dir, BVH.Triangles[Node.First + i], T, Facing) && T < BestT)
                        {
                            BestT = T;
                            BestFacing = Facing;
                        }
                    }
                    continue;
                }

                if (StackSize + 2u <= 64u)
                {
                    Stack[StackSize++] = Node.First;
                    Stack[StackSize++] = Node.First + 1u;
                }
            }

            return BestT < FLT_MAX && BestFacing > 0.0f;
        }

        void BuildSignRays(FVector3 (&OutRays)[kSignRayCount])
        {
            constexpr float GoldenAngle = 2.39996322972865332f;   // pi * (3 - sqrt(5))

            for (uint32 i = 0; i < kSignRayCount; ++i)
            {
                const float Z     = 1.0f - (2.0f * (float)i + 1.0f) / (float)kSignRayCount;
                const float Rad   = Math::Sqrt(Math::Max(0.0f, 1.0f - Z * Z));
                const float Theta = GoldenAngle * (float)i;
                OutRays[i] = FVector3(Math::Cos(Theta) * Rad, Math::Sin(Theta) * Rad, Z);
            }
        }

        void GatherTriangles(const FMeshResource& Resource, uint32 RequestedLOD, TVector<FTriangle>& Out)
        {
            const FMeshletData& MD = Resource.MeshletData;
            const bool bSkinned    = Resource.bSkinnedMesh;

            const size_t VertexCount = bSkinned ? MD.MeshletSkinnedVertices.size() : MD.MeshletVertices.size();
            if (MD.Meshlets.empty() || VertexCount == 0)
            {
                return;
            }

            auto ReadPosition = [&](const FMeshlet& M, uint32 Index) -> FVector3
            {
                return bSkinned ? DecodeMeshletPosition(M, MD.MeshletSkinnedVertices[Index])
                                : DecodeMeshletPosition(M, MD.MeshletVertices[Index]);
            };

            for (const FGeometrySurface& Surface : Resource.GeometrySurfaces)
            {
                if (Surface.NumLODs == 0)
                {
                    continue;
                }

                const uint32 Slot   = Math::Min(RequestedLOD, Surface.NumLODs - 1u);
                const uint32 Offset = Surface.LODMeshletOffset[Slot];
                const uint32 Count  = Surface.LODMeshletCount[Slot];

                for (uint32 m = 0; m < Count; ++m)
                {
                    if (Offset + m >= MD.Meshlets.size())
                    {
                        break;
                    }

                    const FMeshlet& Meshlet = MD.Meshlets[Offset + m];
                    for (uint32 t = 0; t < Meshlet.TriangleCount; ++t)
                    {
                        const size_t DwordIndex = (size_t)Meshlet.TriangleOffset + t;
                        if (DwordIndex >= MD.MeshletTriangles.size())
                        {
                            break;
                        }

                        const uint32 Packed = MD.MeshletTriangles[DwordIndex];
                        const uint32 I0 = Meshlet.VertexOffset + (Packed & 0xFFu);
                        const uint32 I1 = Meshlet.VertexOffset + ((Packed >> 8) & 0xFFu);
                        const uint32 I2 = Meshlet.VertexOffset + ((Packed >> 16) & 0xFFu);

                        if (I0 >= VertexCount || I1 >= VertexCount || I2 >= VertexCount)
                        {
                            continue;
                        }

                        Out.push_back(FTriangle{ ReadPosition(Meshlet, I0),
                                                 ReadPosition(Meshlet, I1),
                                                 ReadPosition(Meshlet, I2) });
                    }
                }
            }
        }
    }

    bool Build(const FMeshResource& Resource, const SDistanceFieldBuildSettings& Settings,
               FDistanceFieldVolume& OutVolume, FScopedSlowTask* Progress)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Meshes");

        OutVolume = FDistanceFieldVolume{};

        if (!Settings.bEnabled)
        {
            return false;
        }

        if (Resource.bSkinnedMesh)
        {
            LOG_WARN("Distance field skipped for skeletal mesh '{}': a baked field cannot follow skinning, "
                     "so it would describe the bind pose regardless of the current animation.",
                     Resource.Name.c_str());
            return false;
        }

        if (Progress)
        {
            Progress->UpdateMessage("Building distance field...");
        }

        FBVH BVH;
        GatherTriangles(Resource, Math::Min(Settings.SourceLOD, MAX_SHADOW_LOD), BVH.Triangles);
        if (BVH.Triangles.empty())
        {
            LOG_WARN("Distance field skipped for mesh '{}': no triangles at LOD {}.",
                     Resource.Name.c_str(), Settings.SourceLOD);
            return false;
        }

        FVector3 BoundsMin(FLT_MAX);
        FVector3 BoundsMax(-FLT_MAX);
        for (const FTriangle& T : BVH.Triangles)
        {
            GrowBounds(BoundsMin, BoundsMax, T.V0);
            GrowBounds(BoundsMin, BoundsMax, T.V1);
            GrowBounds(BoundsMin, BoundsMax, T.V2);
        }

        const FVector3 MeshSize  = BoundsMax - BoundsMin;
        const float    MaxExtent = Math::Max(MeshSize.x, Math::Max(MeshSize.y, MeshSize.z));
        if (!(MaxExtent > 0.0f))
        {
            LOG_WARN("Distance field skipped for mesh '{}': degenerate bounds.", Resource.Name.c_str());
            return false;
        }

        const uint32 Resolution  = Math::Clamp(Settings.Resolution, 4u, 256u);
        const float  BandScale   = Math::Clamp(Settings.NarrowBandScale, 0.01f, 1.0f);
        const float  MaxDistance = MaxExtent * BandScale;

        const FVector3 PaddedMin  = BoundsMin - FVector3(MaxDistance);
        const FVector3 PaddedSize = MeshSize + FVector3(MaxDistance * 2.0f);

        const float VoxelSize = Math::Max(PaddedSize.x, Math::Max(PaddedSize.y, PaddedSize.z)) / (float)Resolution;

        FUIntVector3 Dimensions;
        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            const uint32 Steps = (uint32)Math::Ceil(PaddedSize[Axis] / VoxelSize);
            Dimensions[Axis] = Math::Clamp(Steps, 2u, Resolution);
        }

        const FVector3 VolumeSize(Dimensions.x * VoxelSize, Dimensions.y * VoxelSize, Dimensions.z * VoxelSize);
        const FVector3 VolumeMin = PaddedMin + (PaddedSize - VolumeSize) * 0.5f;

        BuildBVH(BVH);

        FVector3 SignRays[kSignRayCount];
        BuildSignRays(SignRays);

        const size_t VoxelCount = (size_t)Dimensions.x * Dimensions.y * Dimensions.z;

        OutVolume.Dimensions  = Dimensions;
        OutVolume.VolumeMin   = VolumeMin;
        OutVolume.VolumeSize  = VolumeSize;
        OutVolume.MaxDistance = MaxDistance;
        OutVolume.bTwoSided   = Settings.bTwoSided;
        OutVolume.Distances.resize(VoxelCount);

        const bool  bTwoSided  = Settings.bTwoSided;
        const float EncodeRcp  = 1.0f / MaxDistance;
        uint8* const Distances = OutVolume.Distances.data();

        Task::ParallelFor(Dimensions.z, [&](uint32 Z)
        {
            LUMINA_PROFILE_SECTION("Distance Field Slice");

            for (uint32 Y = 0; Y < Dimensions.y; ++Y)
            {
                for (uint32 X = 0; X < Dimensions.x; ++X)
                {
                    const FVector3 P = VolumeMin + FVector3(
                        ((float)X + 0.5f) * VoxelSize,
                        ((float)Y + 0.5f) * VoxelSize,
                        ((float)Z + 0.5f) * VoxelSize);

                    float Distance = Math::Sqrt(ClosestDistanceSquared(BVH, P));

                    if (!bTwoSided)
                    {
                        const bool bMayBeInside =
                               P.x >= BoundsMin.x && P.x <= BoundsMax.x
                            && P.y >= BoundsMin.y && P.y <= BoundsMax.y
                            && P.z >= BoundsMin.z && P.z <= BoundsMax.z;

                        if (bMayBeInside)
                        {
                            uint32 InsideVotes = 0;
                            for (uint32 R = 0; R < kSignRayCount; ++R)
                            {
                                InsideVotes += RayHitsBackface(BVH, P, SignRays[R]) ? 1u : 0u;
                            }

                            if (InsideVotes * 2u > kSignRayCount)
                            {
                                Distance = -Distance;
                            }
                        }
                    }

                    const float Normalized = bTwoSided
                        ? Math::Clamp(Distance * EncodeRcp, 0.0f, 1.0f)
                        : Math::Clamp(Distance * EncodeRcp * 0.5f + 0.5f, 0.0f, 1.0f);

                    const size_t Index = ((size_t)Z * Dimensions.y + Y) * Dimensions.x + X;
                    Distances[Index] = (uint8)(Normalized * 255.0f + 0.5f);
                }
            }
        });

        LOG_INFO("Built distance field for mesh '{}': {}x{}x{} ({} KB) from {} triangles at LOD {}.",
                 Resource.Name.c_str(), Dimensions.x, Dimensions.y, Dimensions.z,
                 VoxelCount / 1024, BVH.Triangles.size(), Settings.SourceLOD);

        return true;
    }
}
