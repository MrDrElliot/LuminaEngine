#include "RuntimePCH.h"
#include "VoxelFarBuilder.h"

#include <cfloat>
#include "Containers/Algorithm.h"
#include "Containers/HashTable.h"
#include "Memory/MemoryTracking.h"

namespace Lumina::Import::VoxelFar
{
    namespace
    {
        // Coarser than this holds a handful of voxels, which the far proxies already stand in for.
        constexpr uint32 kMinResolution = 2;

        // A splat's half width in voxels, wide enough that neighboring splats overlap rather than leave gaps.
        constexpr float kSplatHalfWidth = 0.6f;

        // Circumradius over half width of the equilateral triangle whose area matches a square of that half width.
        constexpr float kSplatTriangleRadius = 1.7548f;

        // Below this length of the area weighted mean normal, the voxel's surfaces face both ways, like a leaf's two sides.
        constexpr float kMinNormalCoherence = 0.1f;

        // A fixed hash per voxel, so a rebuild keeps the same sparse voxels and the splats do not swim between builds.
        FORCEINLINE float VoxelHash(uint32 Key)
        {
            uint32 H = Key * 0x9E3779B1u;
            H ^= H >> 15u;
            H *= 0x85EBCA77u;
            H ^= H >> 13u;
            return (float)(H >> 8u) * (1.0f / 16777216.0f);
        }

        struct FVoxelAccumulator
        {
            FVector3 NormalSum = FVector3(0.0f);
            // Surface area inside the voxel, which is what the splat's size stands for.
            float    Area      = 0.0f;
            // The largest triangle's UV box, which the material pass averages over rather than one texel of the atlas.
            FVector2 UVMin     = FVector2(0.0f);
            FVector2 UVMax     = FVector2(0.0f);
            FVector3 BestNormal = FVector3(0.0f);
            float    BestArea  = 0.0f;
            uint32   Source    = 0;
        };

        FORCEINLINE uint32 VoxelKey(uint32 X, uint32 Y, uint32 Z)
        {
            return X | (Y << 10u) | (Z << 20u);
        }

        FORCEINLINE void KeyToVoxel(uint32 Key, uint32& X, uint32& Y, uint32& Z)
        {
            X = Key & 1023u;
            Y = (Key >> 10u) & 1023u;
            Z = (Key >> 20u) & 1023u;
        }

        // Akenine-Moller separating axis test of a triangle against a box at the origin with the given half size.
        bool TriangleOverlapsBox(const FVector3& BoxCenter, float HalfSize, const FVector3& A, const FVector3& B, const FVector3& C)
        {
            const FVector3 V0 = A - BoxCenter;
            const FVector3 V1 = B - BoxCenter;
            const FVector3 V2 = C - BoxCenter;

            const FVector3 E0 = V1 - V0;
            const FVector3 E1 = V2 - V1;
            const FVector3 E2 = V0 - V2;

            auto AxisTest = [&](const FVector3& Axis) -> bool
            {
                const float P0 = Math::Dot(V0, Axis);
                const float P1 = Math::Dot(V1, Axis);
                const float P2 = Math::Dot(V2, Axis);
                const float R  = HalfSize * (Math::Abs(Axis.x) + Math::Abs(Axis.y) + Math::Abs(Axis.z));
                return Math::Max(-Math::Max(P0, Math::Max(P1, P2)), Math::Min(P0, Math::Min(P1, P2))) <= R;
            };

            const FVector3 BoxAxes[3] = { FVector3(1.0f, 0.0f, 0.0f), FVector3(0.0f, 1.0f, 0.0f), FVector3(0.0f, 0.0f, 1.0f) };
            const FVector3 Edges[3]   = { E0, E1, E2 };
            for (const FVector3& Edge : Edges)
            {
                for (const FVector3& BoxAxis : BoxAxes)
                {
                    const FVector3 Axis = Math::Cross(Edge, BoxAxis);
                    if (Math::Dot(Axis, Axis) > 1e-12f && !AxisTest(Axis))
                    {
                        return false;
                    }
                }
            }

            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                const float Lo = Math::Min(V0[Axis], Math::Min(V1[Axis], V2[Axis]));
                const float Hi = Math::Max(V0[Axis], Math::Max(V1[Axis], V2[Axis]));
                if (Lo > HalfSize || Hi < -HalfSize)
                {
                    return false;
                }
            }

            const FVector3 Normal = Math::Cross(E0, E1);
            return AxisTest(Normal);
        }

        void VoxelizeLevel(const FBuildInput& In, uint32 Resolution, float VoxelSize, THashMap<uint32, FVoxelAccumulator>& Out)
        {
            const uint32 NumTriangles = (uint32)In.Indices.Num() / 3u;
            const float  HalfSize     = VoxelSize * 0.5f;
            const uint32 MaxIndex     = Resolution - 1u;

            for (uint32 t = 0; t < NumTriangles; ++t)
            {
                const uint32 I0 = In.Indices[t * 3u];
                const uint32 I1 = In.Indices[t * 3u + 1u];
                const uint32 I2 = In.Indices[t * 3u + 2u];
                const FVector3& A = In.Positions[I0];
                const FVector3& B = In.Positions[I1];
                const FVector3& C = In.Positions[I2];

                const FVector3 FaceNormal = Math::Cross(B - A, C - A);
                const float    Area       = 0.5f * Math::Length(FaceNormal);
                if (!(Area > 0.0f))
                {
                    continue;
                }

                FVector3 NormalSum(0.0f);
                FVector2 UVMin(FLT_MAX);
                FVector2 UVMax(-FLT_MAX);
                const uint32 Corners[3] = { I0, I1, I2 };
                for (uint32 Corner : Corners)
                {
                    const float* Attr = &In.Attributes[Corner * In.AttributeCount];
                    NormalSum += FVector3(Attr[0], Attr[1], Attr[2]);
                    UVMin      = Math::Min(UVMin, FVector2(Attr[3], Attr[4]));
                    UVMax      = Math::Max(UVMax, FVector2(Attr[3], Attr[4]));
                }
                // A degenerate vertex normal falls back to the face, which is at least on the right side.
                FVector3 Normal = Math::Dot(NormalSum, NormalSum) > 1e-12f ? Math::Normalize(NormalSum) : Math::Normalize(FaceNormal);

                const FVector3 TriMin = Math::Min(A, Math::Min(B, C));
                const FVector3 TriMax = Math::Max(A, Math::Max(B, C));

                uint32 Lo[3];
                uint32 Hi[3];
                for (int32 Axis = 0; Axis < 3; ++Axis)
                {
                    const float FLo = Math::Floor((TriMin[Axis] - In.GridMin[Axis]) / VoxelSize);
                    const float FHi = Math::Floor((TriMax[Axis] - In.GridMin[Axis]) / VoxelSize);
                    Lo[Axis] = (uint32)Math::Clamp(FLo, 0.0f, (float)MaxIndex);
                    Hi[Axis] = (uint32)Math::Clamp(FHi, 0.0f, (float)MaxIndex);
                }

                for (uint32 Z = Lo[2]; Z <= Hi[2]; ++Z)
                {
                    for (uint32 Y = Lo[1]; Y <= Hi[1]; ++Y)
                    {
                        for (uint32 X = Lo[0]; X <= Hi[0]; ++X)
                        {
                            const FVector3 Center = In.GridMin + FVector3((float)X + 0.5f, (float)Y + 0.5f, (float)Z + 0.5f) * VoxelSize;
                            if (!TriangleOverlapsBox(Center, HalfSize, A, B, C))
                            {
                                continue;
                            }

                            FVoxelAccumulator& Acc = Out.FindOrAdd(VoxelKey(X, Y, Z));
                            // A triangle spanning many voxels puts at most a voxel face's worth of itself in each.
                            const float AreaInVoxel = Math::Min(Area, VoxelSize * VoxelSize);
                            Acc.NormalSum += Normal * AreaInVoxel;
                            Acc.Area      += AreaInVoxel;
                            if (Area > Acc.BestArea)
                            {
                                Acc.BestArea = Area;
                                Acc.UVMin    = UVMin;
                                Acc.UVMax    = UVMax;
                                Acc.BestNormal = Normal;
                                Acc.Source   = I0;
                            }
                        }
                    }
                }
            }
        }

        void EmitLevel(const FBuildInput& In, const THashMap<uint32, FVoxelAccumulator>& Voxels, float VoxelSize, FLevel& Out)
        {
            // Sorted so neighboring voxels land in the same meshlet, which the meshlet builder cannot do from a hash walk.
            TVector<uint32> Keys;
            Keys.Reserve(Voxels.Num());
            for (const auto& [Key, Acc] : Voxels)
            {
                Keys.Add(Key);
            }
            Algo::Sort(Keys);

            const float CornerReach = VoxelSize * kSplatHalfWidth * kSplatTriangleRadius;

            Out.VoxelSize    = VoxelSize;
            // A splat strays from its voxel center no further than its corners reach.
            Out.Error        = CornerReach;
            Out.Positions.Reserve(Keys.Num() * 3);
            Out.Attributes.Reserve(Keys.Num() * 3 * In.AttributeCount);
            Out.SourceVertex.Reserve(Keys.Num() * 3);
            Out.Indices.Reserve(Keys.Num() * 3);

            // A sparse voxel keeps a full splat only some of the time, since a shrunken one would vanish below a pixel.
            THashSet<uint32> Kept;
            for (uint32 Key : Keys)
            {
                const FVoxelAccumulator& Acc = *Voxels.Find(Key);
                const float Coverage = Math::Clamp(Acc.Area / (VoxelSize * VoxelSize), 0.0f, 1.0f);
                if (VoxelHash(Key) <= Math::Sqrt(Coverage))
                {
                    Kept.Add(Key);
                }
            }

            // Splats are opaque and overlap their neighbors, so a voxel kept on all six sides is never seen and draws nothing.
            auto IsEnclosed = [&Kept](uint32 X, uint32 Y, uint32 Z)
            {
                return X > 0u && Y > 0u && Z > 0u && X < 1023u && Y < 1023u && Z < 1023u
                    && Kept.Contains(VoxelKey(X - 1u, Y, Z)) && Kept.Contains(VoxelKey(X + 1u, Y, Z))
                    && Kept.Contains(VoxelKey(X, Y - 1u, Z)) && Kept.Contains(VoxelKey(X, Y + 1u, Z))
                    && Kept.Contains(VoxelKey(X, Y, Z - 1u)) && Kept.Contains(VoxelKey(X, Y, Z + 1u));
            };

            for (uint32 Key : Keys)
            {
                uint32 X, Y, Z;
                KeyToVoxel(Key, X, Y, Z);
                if (!Kept.Contains(Key) || IsEnclosed(X, Y, Z))
                {
                    continue;
                }

                const FVoxelAccumulator& Acc = *Voxels.Find(Key);
                const FVector3 Center = In.GridMin + FVector3((float)X + 0.5f, (float)Y + 0.5f, (float)Z + 0.5f) * VoxelSize;
                // A plane facing one way only would be culled from behind, so canceling sides keep the largest triangle's plane and face both ways.
                const bool bTwoSided = Math::Length(Acc.NormalSum) < kMinNormalCoherence * Acc.Area;
                const FVector3 FrontNormal = bTwoSided ? Acc.BestNormal : Math::Normalize(Acc.NormalSum);

                auto EmitSplat = [&](const FVector3& Normal)
                {
                    const FVector3 Helper    = Math::Abs(Normal.y) < 0.9f ? FVector3(0.0f, 1.0f, 0.0f) : FVector3(1.0f, 0.0f, 0.0f);
                    const FVector3 Tangent   = Math::Normalize(Math::Cross(Helper, Normal));
                    const FVector3 Bitangent = Math::Cross(Normal, Tangent);

                    // One equilateral triangle, since a splat is a blob and the raster pays per triangle.
                    const uint32 Base = (uint32)Out.Positions.Num();
                    const FVector3 Corners[3] =
                    {
                        Center + Bitangent * CornerReach,
                        Center + (Tangent * -0.8660254f - Bitangent * 0.5f) * CornerReach,
                        Center + (Tangent *  0.8660254f - Bitangent * 0.5f) * CornerReach,
                    };
                    // Corner UVs span the box, so the splat's screen derivatives pick a mip that averages the triangle's texels.
                    const FVector2 CornerUVs[3] = { Acc.UVMin, FVector2(Acc.UVMax.x, Acc.UVMin.y), FVector2(Acc.UVMin.x, Acc.UVMax.y) };
                    for (uint32 c = 0; c < 3u; ++c)
                    {
                        const FVector3& Corner = Corners[c];
                        const FVector2& UV     = CornerUVs[c];
                        Out.Positions.Add(Corner);
                        Out.SourceVertex.Add(Acc.Source);
                        const int32 First = Out.Attributes.Num();
                        Out.Attributes.SetNum(First + (int32)In.AttributeCount);
                        float* Attr = &Out.Attributes[First];
                        Attr[0] = Normal.x;
                        Attr[1] = Normal.y;
                        Attr[2] = Normal.z;
                        Attr[3] = UV.x;
                        Attr[4] = UV.y;
                    }

                    Out.Indices.Append({ Base, Base + 1u, Base + 2u });
                };

                EmitSplat(FrontNormal);
                if (bTwoSided)
                {
                    EmitSplat(-FrontNormal);
                }
                ++Out.VoxelCount;
            }
        }
    }

    void Build(const FBuildInput& In, TVector<FLevel>& OutLevels)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Meshes");

        OutLevels.Reset();
        if (In.Indices.Num() < 3 || In.Positions.IsEmpty() || In.AttributeCount < 5u || !(In.VoxelSize > 0.0f))
        {
            return;
        }

        THashMap<uint32, FVoxelAccumulator> Voxels;
        float  VoxelSize  = In.VoxelSize;
        for (uint32 Resolution = In.Resolution; Resolution >= kMinResolution; Resolution /= 2u, VoxelSize *= 2.0f)
        {
            Voxels.Reset();
            VoxelizeLevel(In, Resolution, VoxelSize, Voxels);
            if (Voxels.IsEmpty())
            {
                continue;
            }

            FLevel& Level = OutLevels.EmplaceGetRef();
            EmitLevel(In, Voxels, VoxelSize, Level);
        }
    }
}
