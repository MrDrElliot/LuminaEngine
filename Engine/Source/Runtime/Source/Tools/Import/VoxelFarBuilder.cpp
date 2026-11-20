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

        // Voxels' worth of surface between a voxel and the outside, on every axis, that hides it from any view.
        constexpr float kOccludingCoverage = 2.0f;

        // Share of a canopy splat's normal that comes from the open side of its neighborhood rather than its own surfaces.
        constexpr float kCanopyNormalWeight = 0.75f;

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

        void VoxelizeLevel(const FBuildInput& In, uint32 Resolution, float VoxelSize, THashMap<uint32, FVoxelCell>& Out)
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

                            // A triangle spanning many voxels puts at most a voxel face's worth of itself in each.
                            const float AreaInVoxel = Math::Min(Area, VoxelSize * VoxelSize);
                            Out.FindOrAdd(VoxelKey(X, Y, Z)).Merge({ Normal * AreaInVoxel, AreaInVoxel, UVMin, UVMax, Normal, Area, I0 });
                        }
                    }
                }
            }
        }

        // Each coarse voxel is exactly its eight children, so merging them stands in for testing every triangle again.
        void CoarsenLevel(const THashMap<uint32, FVoxelCell>& Fine, uint32 Resolution, THashMap<uint32, FVoxelCell>& Out)
        {
            const uint32 MaxIndex = Resolution - 1u;
            for (const auto& [Key, Acc] : Fine)
            {
                uint32 X, Y, Z;
                KeyToVoxel(Key, X, Y, Z);
                Out.FindOrAdd(VoxelKey(Math::Min(X / 2u, MaxIndex), Math::Min(Y / 2u, MaxIndex), Math::Min(Z / 2u, MaxIndex))).Merge(Acc);
            }
        }

        // Per voxel, the least surface coverage lying between it and the grid's edge along any of the six axis directions.
        template<typename TCoverage>
        void ComputeBurial(const TVector<uint32>& Keys, TCoverage&& CoverageAt, THashMap<uint32, float>& OutBurial)
        {
            const int32 Num = Keys.Num();
            TVector<float> Coverage(Num);
            TVector<float> Burial(Num, FLT_MAX);
            for (int32 i = 0; i < Num; ++i)
            {
                Coverage[i] = CoverageAt(Keys[i]);
            }

            TVector<int32> Order(Num);
            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                // Rows run along Axis, so sorting by the other two coordinates first puts each row together in order.
                auto RowKey = [&](uint32 Key)
                {
                    uint32 C[3];
                    KeyToVoxel(Key, C[0], C[1], C[2]);
                    return ((uint64)C[(Axis + 1) % 3] << 40) | ((uint64)C[(Axis + 2) % 3] << 20) | C[Axis];
                };
                for (int32 i = 0; i < Num; ++i)
                {
                    Order[i] = i;
                }
                Algo::Sort(Order, [&](int32 A, int32 B) { return RowKey(Keys[A]) < RowKey(Keys[B]); });

                for (int32 Begin = 0; Begin < Num;)
                {
                    const uint64 Row = RowKey(Keys[Order[Begin]]) >> 20;
                    int32 End = Begin + 1;
                    while (End < Num && (RowKey(Keys[Order[End]]) >> 20) == Row)
                    {
                        ++End;
                    }
                    float Before = 0.0f;
                    for (int32 i = Begin; i < End; ++i)
                    {
                        Burial[Order[i]] = Math::Min(Burial[Order[i]], Before);
                        Before += Coverage[Order[i]];
                    }
                    float After = 0.0f;
                    for (int32 i = End - 1; i >= Begin; --i)
                    {
                        Burial[Order[i]] = Math::Min(Burial[Order[i]], After);
                        After += Coverage[Order[i]];
                    }
                    Begin = End;
                }
            }

            for (int32 i = 0; i < Num; ++i)
            {
                OutBurial.Add(Keys[i], Burial[i]);
            }
        }

        void EmitLevel(const FBuildInput& In, const THashMap<uint32, FVoxelCell>& Voxels, const THashMap<uint32, FVoxelOwner>* Shared, uint32 SurfaceIndex,
                       float VoxelSize, FLevel& Out)
        {
            // Sorted so neighboring voxels land in the same meshlet, which the meshlet builder cannot do from a hash walk.
            TVector<uint32> Keys;
            if (Shared != nullptr)
            {
                Keys.Reserve(Shared->Num());
                for (const auto& [Key, Owner] : *Shared)
                {
                    Keys.Add(Key);
                }
            }
            else
            {
                Keys.Reserve(Voxels.Num());
                for (const auto& [Key, Cell] : Voxels)
                {
                    Keys.Add(Key);
                }
            }
            Algo::Sort(Keys);

            // Shared voxels are kept, hidden and shaded by what every surface puts in them, so a trunk inside a crown stays inside it.
            auto CoverageAt = [&](uint32 Key)
            {
                float Area = 0.0f;
                if (Shared != nullptr)
                {
                    const FVoxelOwner* Owner = Shared->Find(Key);
                    Area = Owner != nullptr ? Owner->Area : 0.0f;
                }
                else
                {
                    const FVoxelCell* Cell = Voxels.Find(Key);
                    Area = Cell != nullptr ? Cell->Area : 0.0f;
                }
                return Math::Clamp(Area / (VoxelSize * VoxelSize), 0.0f, 1.0f);
            };

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
                if (VoxelHash(Key) <= Math::Sqrt(CoverageAt(Key)))
                {
                    Kept.Add(Key);
                }
            }

            THashMap<uint32, float> Burial;
            if (In.bCullOccluded)
            {
                ComputeBurial(Keys, CoverageAt, Burial);
            }

            // Splats are opaque and overlap their neighbors, so a voxel kept on all six sides is never seen and draws nothing.
            auto IsEnclosed = [&Kept](uint32 X, uint32 Y, uint32 Z)
            {
                return X > 0u && Y > 0u && Z > 0u && X < 1023u && Y < 1023u && Z < 1023u
                    && Kept.Contains(VoxelKey(X - 1u, Y, Z)) && Kept.Contains(VoxelKey(X + 1u, Y, Z))
                    && Kept.Contains(VoxelKey(X, Y - 1u, Z)) && Kept.Contains(VoxelKey(X, Y + 1u, Z))
                    && Kept.Contains(VoxelKey(X, Y, Z - 1u)) && Kept.Contains(VoxelKey(X, Y, Z + 1u));
            };

            // Points away from the coverage around the voxel, which for a crown is out through its skin and for the ground is up.
            auto OpenSide = [&](uint32 X, uint32 Y, uint32 Z)
            {
                FVector3 Outward(0.0f);
                for (int32 DZ = -1; DZ <= 1; ++DZ)
                {
                    for (int32 DY = -1; DY <= 1; ++DY)
                    {
                        for (int32 DX = -1; DX <= 1; ++DX)
                        {
                            const int32 NX = (int32)X + DX;
                            const int32 NY = (int32)Y + DY;
                            const int32 NZ = (int32)Z + DZ;
                            if ((DX | DY | DZ) == 0 || NX < 0 || NY < 0 || NZ < 0 || NX > 1023 || NY > 1023 || NZ > 1023)
                            {
                                continue;
                            }
                            Outward -= FVector3((float)DX, (float)DY, (float)DZ) * CoverageAt(VoxelKey((uint32)NX, (uint32)NY, (uint32)NZ));
                        }
                    }
                }
                return Math::Dot(Outward, Outward) > 1e-6f ? Math::Normalize(Outward) : FVector3(0.0f, 1.0f, 0.0f);
            };

            for (uint32 Key : Keys)
            {
                uint32 X, Y, Z;
                KeyToVoxel(Key, X, Y, Z);
                if (!Kept.Contains(Key) || IsEnclosed(X, Y, Z))
                {
                    continue;
                }
                if (In.bCullOccluded && *Burial.Find(Key) >= kOccludingCoverage)
                {
                    continue;
                }

                // One splat per shared voxel, since two at one center with different spins speckle the loser through the winner.
                const FVoxelCell* Found = Voxels.Find(Key);
                if (Found == nullptr || (Shared != nullptr && Shared->Find(Key)->Owner != SurfaceIndex))
                {
                    continue;
                }
                const FVoxelCell& Acc = *Found;
                const FVector3 Center = In.GridMin + FVector3((float)X + 0.5f, (float)Y + 0.5f, (float)Z + 0.5f) * VoxelSize;
                // A plane facing one way only would be culled from behind, so canceling sides keep the largest triangle's plane and face both ways.
                const bool bCanceling = Math::Length(Acc.NormalSum) < kMinNormalCoherence * Acc.Area;
                const bool bOpenNeeded = In.bCanopyNormals || (bCanceling && In.bFaceOutward);
                const FVector3 Open = bOpenNeeded ? OpenSide(X, Y, Z) : FVector3(0.0f, 1.0f, 0.0f);
                const FVector3 Coherent = bCanceling ? Acc.BestNormal : Math::Normalize(Acc.NormalSum);
                const bool bTwoSided = bCanceling && !In.bFaceOutward;
                const FVector3 FrontNormal = bCanceling && In.bFaceOutward && Math::Dot(Coherent, Open) < 0.0f ? -Coherent : Coherent;

                FVector3 CanopyNormal(0.0f);
                if (In.bCanopyNormals)
                {
                    const FVector3 Surface = Math::Dot(FrontNormal, Open) < 0.0f ? -FrontNormal : FrontNormal;
                    CanopyNormal = Math::Normalize(Math::Mix(Surface, Open, kCanopyNormalWeight));
                }

                // Turned by a hash of the voxel, since splats that all point one way line up into stripes across a canopy.
                const float Spin = VoxelHash(Key ^ 0x5BD1E995u) * Math::TwoPi<float>();
                auto EmitSplat = [&](const FVector3& Normal)
                {
                    const FVector3 Shading = In.bCanopyNormals ? CanopyNormal : Normal;
                    const FVector3 Helper    = Math::Abs(Normal.y) < 0.9f ? FVector3(0.0f, 1.0f, 0.0f) : FVector3(1.0f, 0.0f, 0.0f);
                    const FVector3 Axis0     = Math::Normalize(Math::Cross(Helper, Normal));
                    const FVector3 Axis1     = Math::Cross(Normal, Axis0);
                    const FVector3 Tangent   = Axis0 * Math::Cos(Spin) + Axis1 * Math::Sin(Spin);
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
                        Attr[0] = Shading.x;
                        Attr[1] = Shading.y;
                        Attr[2] = Shading.z;
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

    void FVoxelCell::Merge(const FVoxelCell& Other)
    {
        NormalSum += Other.NormalSum;
        Area      += Other.Area;
        if (Other.BestArea > BestArea)
        {
            UVMin      = Other.UVMin;
            UVMax      = Other.UVMax;
            BestNormal = Other.BestNormal;
            BestArea   = Other.BestArea;
            Source     = Other.Source;
        }
    }

    void Voxelize(const FBuildInput& In, FSurfaceVoxels& Out)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Meshes");

        Out.Levels.Reset();
        if (In.Indices.Num() < 3 || In.Positions.IsEmpty() || In.AttributeCount < 5u || !(In.VoxelSize > 0.0f) || In.Resolution < kMinResolution)
        {
            return;
        }

        THashMap<uint32, FVoxelCell> Voxels;
        VoxelizeLevel(In, In.Resolution, In.VoxelSize, Voxels);
        if (Voxels.IsEmpty())
        {
            return;
        }

        for (uint32 Resolution = In.Resolution; Resolution >= kMinResolution; Resolution /= 2u)
        {
            THashMap<uint32, FVoxelCell> Coarser;
            if (Resolution / 2u >= kMinResolution)
            {
                CoarsenLevel(Voxels, Resolution / 2u, Coarser);
            }
            Out.Levels.Add(Move(Voxels));
            Voxels = Move(Coarser);
        }
    }

    void Share(TSpan<const FSurfaceVoxels> Surfaces, FSharedVoxels& Out)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Meshes");

        Out.Levels.Reset();
        for (uint32 Surface = 0; Surface < (uint32)Surfaces.Num(); ++Surface)
        {
            const FSurfaceVoxels& Voxels = Surfaces[Surface];
            if (Out.Levels.Num() < Voxels.Levels.Num())
            {
                Out.Levels.SetNum(Voxels.Levels.Num());
            }
            for (int32 Level = 0; Level < Voxels.Levels.Num(); ++Level)
            {
                for (const auto& [Key, Cell] : Voxels.Levels[Level])
                {
                    FVoxelOwner& Owner = Out.Levels[Level].FindOrAdd(Key);
                    Owner.Area += Cell.Area;
                    if (Cell.Area > Owner.OwnerArea)
                    {
                        Owner.OwnerArea = Cell.Area;
                        Owner.Owner     = Surface;
                    }
                }
            }
        }
    }

    void Emit(const FBuildInput& In, const FSurfaceVoxels& Voxels, const FSharedVoxels* Shared, uint32 SurfaceIndex, TVector<FLevel>& OutLevels)
    {
        LUMINA_PROFILE_SCOPE();
        LUMINA_MEMORY_SCOPE("Meshes");

        OutLevels.Reset();
        float VoxelSize = In.VoxelSize;
        for (int32 Level = 0; Level < Voxels.Levels.Num(); ++Level, VoxelSize *= 2.0f)
        {
            const THashMap<uint32, FVoxelOwner>* SharedLevel = Shared != nullptr && Level < Shared->Levels.Num() ? &Shared->Levels[Level] : nullptr;
            EmitLevel(In, Voxels.Levels[Level], SharedLevel, SurfaceIndex, VoxelSize, OutLevels.EmplaceGetRef());
        }
    }

    void Build(const FBuildInput& In, TVector<FLevel>& OutLevels)
    {
        FSurfaceVoxels Voxels;
        Voxelize(In, Voxels);
        Emit(In, Voxels, nullptr, 0u, OutLevels);
    }
}
