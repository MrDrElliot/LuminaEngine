#include "RuntimePCH.h"
#include <limits>
#include "TerrainMeshletBuilder.h"
#include "TaskSystem/TaskSystem.h"
#include "World/Entity/Components/TerrainComponent.h"

namespace Lumina::TerrainMeshletBuilder
{
    namespace
    {
        // Sample (X, Y) clamped, returns world-space height (post-MaxHeight scale).
        FORCEINLINE float SampleWorldHeight(
            const TVector<float>& Heightmap,
            int32                 SampleX,
            int32                 SampleY,
            int32                 Resolution,
            float                 MaxHeight)
        {
            SampleX = Math::Clamp(SampleX, 0, Resolution - 1);
            SampleY = Math::Clamp(SampleY, 0, Resolution - 1);
            const size_t Index = (size_t)SampleY * (size_t)Resolution + (size_t)SampleX;
            return Heightmap[Index] * MaxHeight;
        }

        struct FLayout
        {
            int32 Resolution;
            int32 QuadsPerChunk;
            int32 ChunksPerSide;
            int32 MeshletQuadSide;
            int32 MeshletsPerChunkSide;
            int32 MeshletsPerChunk;
            int32 NumChunks;
            int32 NumMeshlets;
            float HalfSize;
            float Stride;
            FVector2 OriginXZ;
            float WorldOriginY;
            float MaxHeight;
            float DilateXZ;
            float DilateY;
        };

        bool ComputeLayout(const STerrainComponent& Terrain, const FVector3& WorldOrigin, FLayout& Out)
        {
            const int32 Resolution = Terrain.Resolution;
            const int32 ChunkRes   = Terrain.ChunkResolution;
            if (Resolution < 2 || ChunkRes < 2)
            {
                return false;
            }
            if ((int32)Terrain.Heightmap.size() != Resolution * Resolution)
            {
                return false;
            }

            Out.Resolution           = Resolution;
            Out.QuadsPerChunk        = ChunkRes - 1;
            Out.ChunksPerSide        = Math::Max(1, (Resolution - 1) / Out.QuadsPerChunk);
            Out.MeshletQuadSide      = GTerrainMeshletQuads;
            Out.MeshletsPerChunkSide = (Out.QuadsPerChunk + Out.MeshletQuadSide - 1) / Out.MeshletQuadSide;
            Out.MeshletsPerChunk     = Out.MeshletsPerChunkSide * Out.MeshletsPerChunkSide;
            Out.NumChunks            = Out.ChunksPerSide * Out.ChunksPerSide;
            Out.NumMeshlets          = Out.NumChunks * Out.MeshletsPerChunk;

            Out.HalfSize     = Terrain.TileWorldSize * 0.5f;
            Out.Stride       = Terrain.TileWorldSize / float(Resolution - 1);
            Out.OriginXZ     = FVector2(WorldOrigin.x - Out.HalfSize, WorldOrigin.z - Out.HalfSize);
            Out.WorldOriginY = WorldOrigin.y;
            Out.MaxHeight    = Terrain.MaxHeight;
            Out.DilateXZ     = Out.Stride * 0.5f;
            // Y dilation: matrix rounding can produce Y values microscopically outside the CPU range -> silhouette culling.
            Out.DilateY      = Math::Max(0.05f, Out.MaxHeight * 0.01f);
            return true;
        }

        FVector2 ComputeMeshletBounds(FTerrainMeshletInfo& Meshlet, const FTerrainChunkInfo& Chunk,
                                       const TVector<float>& Heightmap, const FLayout& L)
        {
            const int32 SampleX0 = Chunk.QuadOrigin.x + Meshlet.ChunkLocalQuadOrigin.x;
            const int32 SampleY0 = Chunk.QuadOrigin.y + Meshlet.ChunkLocalQuadOrigin.y;
            const int32 NVertsX  = Meshlet.QuadExtent.x + 1;
            const int32 NVertsY  = Meshlet.QuadExtent.y + 1;

            float MinH =  std::numeric_limits<float>::infinity();
            float MaxH = -std::numeric_limits<float>::infinity();
            for (int32 vy = 0; vy < NVertsY; ++vy)
            {
                for (int32 vx = 0; vx < NVertsX; ++vx)
                {
                    const float H = SampleWorldHeight(Heightmap, SampleX0 + vx, SampleY0 + vy, L.Resolution, L.MaxHeight);
                    MinH = Math::Min(MinH, H);
                    MaxH = Math::Max(MaxH, H);
                }
            }

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
            Chunk.MeshletOffset = (uint32)(ChunkIndex * L.MeshletsPerChunk);
            Chunk.MeshletCount  = (uint32)L.MeshletsPerChunk;

            float ChunkHeightMin =  std::numeric_limits<float>::infinity();
            float ChunkHeightMax = -std::numeric_limits<float>::infinity();

            for (int32 my = 0; my < L.MeshletsPerChunkSide; ++my)
            {
                for (int32 mx = 0; mx < L.MeshletsPerChunkSide; ++mx)
                {
                    const int32 LocalMeshletIndex  = my * L.MeshletsPerChunkSide + mx;
                    const int32 MeshletGlobalIndex = ChunkIndex * L.MeshletsPerChunk + LocalMeshletIndex;

                    FTerrainMeshletInfo& Meshlet = State.Meshlets[MeshletGlobalIndex];
                    Meshlet.ChunkIndex           = (uint32)ChunkIndex;
                    Meshlet.ChunkLocalQuadOrigin = FIntVector2(mx * L.MeshletQuadSide, my * L.MeshletQuadSide);

                    const int32 RemainingX = L.QuadsPerChunk - Meshlet.ChunkLocalQuadOrigin.x;
                    const int32 RemainingY = L.QuadsPerChunk - Meshlet.ChunkLocalQuadOrigin.y;
                    Meshlet.QuadExtent = FIntVector2(
                        Math::Min(L.MeshletQuadSide, RemainingX),
                        Math::Min(L.MeshletQuadSide, RemainingY));

                    const FVector2 HRange = ComputeMeshletBounds(Meshlet, Chunk, Heightmap, L);
                    ChunkHeightMin = Math::Min(ChunkHeightMin, HRange.x);
                    ChunkHeightMax = Math::Max(ChunkHeightMax, HRange.y);
                }
            }

            const float ChunkXMin = L.OriginXZ.x + float(Chunk.QuadOrigin.x) * L.Stride - L.DilateXZ;
            const float ChunkXMax = L.OriginXZ.x + float(Chunk.QuadOrigin.x + L.QuadsPerChunk) * L.Stride + L.DilateXZ;
            const float ChunkZMin = L.OriginXZ.y + float(Chunk.QuadOrigin.y) * L.Stride - L.DilateXZ;
            const float ChunkZMax = L.OriginXZ.y + float(Chunk.QuadOrigin.y + L.QuadsPerChunk) * L.Stride + L.DilateXZ;

            Chunk.BoundsMin = FVector3(ChunkXMin, L.WorldOriginY + ChunkHeightMin - L.DilateY, ChunkZMin);
            Chunk.BoundsMax = FVector3(ChunkXMax, L.WorldOriginY + ChunkHeightMax + L.DilateY, ChunkZMax);
        }

        // Safe to fan out: a chunk writes only its own Chunks entry and its own MeshletsPerChunk slice.
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
            constexpr int32 kParallelThreshold = 4;
            if (Count < kParallelThreshold || GTaskSystem == nullptr)
            {
                for (int32 i = 0; i < Count; ++i)
                {
                    RebuildChunk(State, CxMin + (i % Width), CyMin + (i / Width), Heightmap, L);
                }
                return;
            }

            Task::ParallelFor((uint32)Count, [&](uint32 Index)
            {
                const int32 i = (int32)Index;
                RebuildChunk(State, CxMin + (i % Width), CyMin + (i / Width), Heightmap, L);
            }, 1);
        }
    }

    void Build(STerrainComponent& Terrain, const FVector3& WorldOrigin)
    {
        FTerrainCPUState& State = Terrain.CPUState;
        State.Chunks.clear();
        State.Meshlets.clear();

        FLayout L;
        if (!ComputeLayout(Terrain, WorldOrigin, L))
        {
            // Heightmap not sized yet; renderer will retry next frame after EnsureTerrainCpuBuffers().
            return;
        }

        State.Chunks.resize(L.NumChunks);
        State.Meshlets.resize(L.NumMeshlets);

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
        if ((int32)State.Chunks.size() != L.NumChunks || (int32)State.Meshlets.size() != L.NumMeshlets)
        {
            Build(Terrain, WorldOrigin);
            return;
        }

        // Only chunks overlapping the dirty sample rect need their bounds recomputed.
        const int32 CxMin = Math::Clamp(SampleMin.x / L.QuadsPerChunk, 0, L.ChunksPerSide - 1);
        const int32 CxMax = Math::Clamp(SampleMax.x / L.QuadsPerChunk, 0, L.ChunksPerSide - 1);
        const int32 CyMin = Math::Clamp(SampleMin.y / L.QuadsPerChunk, 0, L.ChunksPerSide - 1);
        const int32 CyMax = Math::Clamp(SampleMax.y / L.QuadsPerChunk, 0, L.ChunksPerSide - 1);

        RebuildChunkRect(State, CxMin, CxMax, CyMin, CyMax, Terrain.Heightmap, L);
    }
}
