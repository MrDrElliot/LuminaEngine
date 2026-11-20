#include "RuntimePCH.h"
#include "TerrainMeshletBuilder.h"
#include "Core/Templates/NumericLimits.h"
#include "Core/Math/SIMD/SIMD.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/TerrainComponent.h"

namespace Lumina::TerrainMeshletBuilder
{
    namespace
    {
        struct FLayout
        {
            int32 Resolution;
            int32 QuadsPerChunk;
            int32 ChunksPerSide;
            int32 MeshletQuadSide;
            int32 NumChunks;
            int32 NumMeshlets;
            float HalfSize;
            float Stride;
            FVector2 OriginXZ;
            float WorldOriginY;
            float MaxHeight;
            float DilateXZ;
            float DilateY;
            // Prefix sums over chunks, since a chunk on the far edge has fewer quads and so fewer meshlets per level.
            TVector<uint32> ChunkMeshletOffsets;
        };

        uint32 CountChunkMeshlets(const FIntVector2& Extent)
        {
            uint32 Count = 0;
            for (uint32 Level = 0; Level < GTerrainLODLevels; ++Level)
            {
                Count += (uint32)(GetTerrainLevelMeshletsSide(Extent.x, Level) * GetTerrainLevelMeshletsSide(Extent.y, Level));
            }
            return Count;
        }

        bool ComputeLayout(const STerrainComponent& Terrain, const FVector3& WorldOrigin, FLayout& Out)
        {
            const int32 Resolution = Terrain.Resolution;
            const int32 ChunkRes   = Terrain.ChunkResolution;
            if (Resolution < 2 || ChunkRes < 2)
            {
                return false;
            }
            if (Terrain.Heightmap.Num() != Resolution * Resolution)
            {
                return false;
            }

            Out.Resolution           = Resolution;
            Out.QuadsPerChunk        = ChunkRes - 1;
            Out.ChunksPerSide        = GetTerrainChunksPerSide(Resolution, Out.QuadsPerChunk);
            Out.MeshletQuadSide      = GTerrainMeshletQuads;
            Out.NumChunks            = Out.ChunksPerSide * Out.ChunksPerSide;
            Out.ChunkMeshletOffsets.SetNum(Out.NumChunks + 1);
            Out.ChunkMeshletOffsets[0] = 0u;
            for (int32 Chunk = 0; Chunk < Out.NumChunks; ++Chunk)
            {
                const FIntVector2 Origin((Chunk % Out.ChunksPerSide) * Out.QuadsPerChunk, (Chunk / Out.ChunksPerSide) * Out.QuadsPerChunk);
                Out.ChunkMeshletOffsets[Chunk + 1] = Out.ChunkMeshletOffsets[Chunk] + CountChunkMeshlets(GetTerrainChunkExtent(Origin, Out.QuadsPerChunk, Resolution));
            }
            Out.NumMeshlets          = (int32)Out.ChunkMeshletOffsets[Out.NumChunks];

            Out.HalfSize     = Terrain.TileWorldSize * 0.5f;
            Out.Stride       = Terrain.TileWorldSize / float(Resolution - 1);
            Out.OriginXZ     = FVector2(WorldOrigin.x - Out.HalfSize, WorldOrigin.z - Out.HalfSize);
            Out.WorldOriginY = WorldOrigin.y;
            Out.MaxHeight    = Terrain.MaxHeight;
            Out.DilateXZ     = Out.Stride * 0.5f;
            // Y dilation, since matrix rounding can push Y just outside the CPU range and cull silhouettes.
            Out.DilateY      = Math::Max(0.05f, Out.MaxHeight * 0.01f);
            return true;
        }

        // Over every heightmap sample the meshlet spans, so a coarse level's box still holds the fine surface it skips.
        FVector2 ComputeMeshletBounds(FTerrainMeshletInfo& Meshlet, const FTerrainChunkInfo& Chunk, const FIntVector2& FineOrigin,
                                       const FIntVector2& FineVerts, const TVector<float>& Heightmap, const FLayout& L)
        {
            const int32 SampleX0 = Chunk.QuadOrigin.x + FineOrigin.x;
            const int32 SampleY0 = Chunk.QuadOrigin.y + FineOrigin.y;
            const int32 NVertsX  = FineVerts.x;
            const int32 NVertsY  = FineVerts.y;

            // Clamped samples only repeat an endpoint, so extremes over the clamped span match per-sample clamping and each row is one read.
            const int32 X0 = Math::Clamp(SampleX0, 0, L.Resolution - 1);
            const int32 X1 = Math::Clamp(SampleX0 + NVertsX - 1, 0, L.Resolution - 1);
            const int32 Y0 = Math::Clamp(SampleY0, 0, L.Resolution - 1);
            const int32 Y1 = Math::Clamp(SampleY0 + NVertsY - 1, 0, L.Resolution - 1);
            const int32 Span = X1 - X0 + 1;

            using namespace SIMD;
            VFloat8 WideMin = VFloat8::Broadcast(TNumericLimits<float>::Infinity());
            VFloat8 WideMax = VFloat8::Broadcast(-TNumericLimits<float>::Infinity());
            float TailMin =  TNumericLimits<float>::Infinity();
            float TailMax = -TNumericLimits<float>::Infinity();

            for (int32 Y = Y0; Y <= Y1; ++Y)
            {
                const float* Row = Heightmap.GetData() + (size_t)Y * (size_t)L.Resolution + (size_t)X0;

                int32 x = 0;
                for (; x + 8 <= Span; x += 8)
                {
                    const VFloat8 H = VFloat8::Load(Row + x);
                    WideMin = Min(WideMin, H);
                    WideMax = Max(WideMax, H);
                }
                for (; x < Span; ++x)
                {
                    TailMin = Math::Min(TailMin, Row[x]);
                    TailMax = Math::Max(TailMax, Row[x]);
                }
            }

            // The scale is hoisted out of the reduction, and a negative MaxHeight swaps which end is which.
            const float LowScaled  = Math::Min(TailMin, HorizontalMin(WideMin)) * L.MaxHeight;
            const float HighScaled = Math::Max(TailMax, HorizontalMax(WideMax)) * L.MaxHeight;
            const float MinH = Math::Min(LowScaled, HighScaled);
            const float MaxH = Math::Max(LowScaled, HighScaled);

            const float WorldXMin = L.OriginXZ.x + float(SampleX0) * L.Stride - L.DilateXZ;
            const float WorldXMax = L.OriginXZ.x + float(SampleX0 + NVertsX - 1) * L.Stride + L.DilateXZ;
            const float WorldZMin = L.OriginXZ.y + float(SampleY0) * L.Stride - L.DilateXZ;
            const float WorldZMax = L.OriginXZ.y + float(SampleY0 + NVertsY - 1) * L.Stride + L.DilateXZ;

            Meshlet.BoundsMin = FVector3(WorldXMin, L.WorldOriginY + MinH - L.DilateY, WorldZMin);
            Meshlet.BoundsMax = FVector3(WorldXMax, L.WorldOriginY + MaxH + L.DilateY, WorldZMax);
            return FVector2(MinH, MaxH);
        }

        // Recompute one chunk's AABB from the heightmap by rebuilding all its meshlets.
        void RebuildChunk(FTerrainCPUState& State, int32 cx, int32 cy, const TVector<float>& Heightmap, const FLayout& L)
        {
            const int32 ChunkIndex = cy * L.ChunksPerSide + cx;
            FTerrainChunkInfo& Chunk = State.Chunks[ChunkIndex];
            Chunk.ChunkCoord    = FIntVector2(cx, cy);
            Chunk.QuadOrigin    = FIntVector2(cx * L.QuadsPerChunk, cy * L.QuadsPerChunk);
            Chunk.MeshletOffset = L.ChunkMeshletOffsets[ChunkIndex];
            Chunk.MeshletCount  = L.ChunkMeshletOffsets[ChunkIndex + 1] - Chunk.MeshletOffset;
            const FIntVector2 Extent = GetTerrainChunkExtent(Chunk.QuadOrigin, L.QuadsPerChunk, L.Resolution);

            float ChunkHeightMin =  TNumericLimits<float>::Infinity();
            float ChunkHeightMax = -TNumericLimits<float>::Infinity();

            uint32 Cursor = Chunk.MeshletOffset;
            for (uint32 Level = 0; Level < GTerrainLODLevels; ++Level)
            {
                const FIntVector2 LevelQuads(GetTerrainLevelQuads(Extent.x, Level), GetTerrainLevelQuads(Extent.y, Level));
                const int32 SideX = GetTerrainLevelMeshletsSide(Extent.x, Level);
                const int32 SideY = GetTerrainLevelMeshletsSide(Extent.y, Level);
                for (int32 my = 0; my < SideY; ++my)
                {
                    for (int32 mx = 0; mx < SideX; ++mx)
                    {
                        FTerrainMeshletInfo& Meshlet = State.Meshlets[Cursor++];
                        Meshlet.ChunkIndex           = (uint32)ChunkIndex;
                        Meshlet.Level                = Level;
                        Meshlet.ChunkLocalQuadOrigin = FIntVector2(mx * L.MeshletQuadSide, my * L.MeshletQuadSide);
                        Meshlet.QuadExtent = FIntVector2(
                            Math::Min(L.MeshletQuadSide, LevelQuads.x - Meshlet.ChunkLocalQuadOrigin.x),
                            Math::Min(L.MeshletQuadSide, LevelQuads.y - Meshlet.ChunkLocalQuadOrigin.y));

                        const FIntVector2 FineOrigin(Meshlet.ChunkLocalQuadOrigin.x << Level, Meshlet.ChunkLocalQuadOrigin.y << Level);
                        const FIntVector2 FineEnd(Math::Min((Meshlet.ChunkLocalQuadOrigin.x + Meshlet.QuadExtent.x) << Level, Extent.x),
                                                  Math::Min((Meshlet.ChunkLocalQuadOrigin.y + Meshlet.QuadExtent.y) << Level, Extent.y));
                        const FVector2 HRange = ComputeMeshletBounds(Meshlet, Chunk, FineOrigin, FineEnd - FineOrigin + FIntVector2(1), Heightmap, L);
                        ChunkHeightMin = Math::Min(ChunkHeightMin, HRange.x);
                        ChunkHeightMax = Math::Max(ChunkHeightMax, HRange.y);
                    }
                }
            }

            const float ChunkXMin = L.OriginXZ.x + float(Chunk.QuadOrigin.x) * L.Stride - L.DilateXZ;
            const float ChunkXMax = L.OriginXZ.x + float(Chunk.QuadOrigin.x + Extent.x) * L.Stride + L.DilateXZ;
            const float ChunkZMin = L.OriginXZ.y + float(Chunk.QuadOrigin.y) * L.Stride - L.DilateXZ;
            const float ChunkZMax = L.OriginXZ.y + float(Chunk.QuadOrigin.y + Extent.y) * L.Stride + L.DilateXZ;

            Chunk.BoundsMin = FVector3(ChunkXMin, L.WorldOriginY + ChunkHeightMin - L.DilateY, ChunkZMin);
            Chunk.BoundsMax = FVector3(ChunkXMax, L.WorldOriginY + ChunkHeightMax + L.DilateY, ChunkZMax);
        }

        // Safe to fan out, since a chunk writes only its own entry and meshlet slice.
        void RebuildChunkRect(FTerrainCPUState& State, int32 CxMin, int32 CxMax, int32 CyMin, int32 CyMax,
                              const TVector<float>& Heightmap, const FLayout& L)
        {
            const int32 Width = CxMax - CxMin + 1;
            const int32 Count = Width * (CyMax - CyMin + 1);
            if (Width <= 0 || Count <= 0)
            {
                return;
            }

            // A chunk is thousands of heightmap samples, so the dispatch pays for itself at a handful.
            constexpr uint32 kParallelThreshold = 4;
            Jobs::ParallelForOrSerial((uint32)Count, kParallelThreshold, [&](uint32 Index)
            {
                const int32 i = (int32)Index;
                RebuildChunk(State, CxMin + (i % Width), CyMin + (i / Width), Heightmap, L);
            }, 1);
        }
    }

    void Build(STerrainComponent& Terrain, const FVector3& WorldOrigin)
    {
        FTerrainCPUState& State = Terrain.CPUState;
        State.PreparedOrigin = WorldOrigin;
        State.Chunks.Reset();
        State.Meshlets.Reset();

        FLayout L;
        if (!ComputeLayout(Terrain, WorldOrigin, L))
        {
            // Heightmap not sized yet; renderer will retry next frame after EnsureTerrainCpuBuffers().
            return;
        }

        State.Chunks.SetNum(L.NumChunks);
        State.Meshlets.SetNum(L.NumMeshlets);

        RebuildChunkRect(State, 0, L.ChunksPerSide - 1, 0, L.ChunksPerSide - 1, Terrain.Heightmap, L);
    }

    void UpdateRegion(STerrainComponent& Terrain, const FVector3& WorldOrigin, const FIntVector2& SampleMin, const FIntVector2& SampleMax)
    {
        FTerrainCPUState& State = Terrain.CPUState;

        FLayout L;
        if (!ComputeLayout(Terrain, WorldOrigin, L))
        {
            return;
        }

        // Structure must already match; otherwise fall back to a full build so indices stay valid.
        if (State.Chunks.Num() != L.NumChunks || State.Meshlets.Num() != L.NumMeshlets)
        {
            Build(Terrain, WorldOrigin);
            return;
        }

        // A sample on a chunk seam is the last of the chunk before it too, so the low edge reaches one sample back.
        const int32 CxMin = Math::Clamp((SampleMin.x - 1) / L.QuadsPerChunk, 0, L.ChunksPerSide - 1);
        const int32 CxMax = Math::Clamp(SampleMax.x / L.QuadsPerChunk, 0, L.ChunksPerSide - 1);
        const int32 CyMin = Math::Clamp((SampleMin.y - 1) / L.QuadsPerChunk, 0, L.ChunksPerSide - 1);
        const int32 CyMax = Math::Clamp(SampleMax.y / L.QuadsPerChunk, 0, L.ChunksPerSide - 1);

        RebuildChunkRect(State, CxMin, CxMax, CyMin, CyMax, Terrain.Heightmap, L);
    }

    void Translate(STerrainComponent& Terrain, const FVector3& Delta)
    {
        FTerrainCPUState& State = Terrain.CPUState;
        State.PreparedOrigin += Delta;
        for (FTerrainChunkInfo& Chunk : State.Chunks)
        {
            Chunk.BoundsMin += Delta;
            Chunk.BoundsMax += Delta;
        }
        for (FTerrainMeshletInfo& Meshlet : State.Meshlets)
        {
            Meshlet.BoundsMin += Delta;
            Meshlet.BoundsMax += Delta;
        }
    }
}
