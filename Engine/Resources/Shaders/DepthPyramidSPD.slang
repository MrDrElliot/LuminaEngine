#pragma once
#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"

// Single-pass downsampler for a conservative HZB (up to 12 mips per dispatch). Bindless source SRV +
// per-mip UAVs, BDA atomic counter for the cross-WG hand-off; no pass-local binding set.
//
// Two reduction modes, because the engine has two depth conventions: the camera pyramid is reverse-Z and
// wants MIN (the farthest surface), the cascade atlas is standard Z and wants MAX. Both are "the surface an
// occludee must be behind to be hidden"; the sign is the only difference. ReduceMax is a push constant, so
// the branch is wave-uniform.

#define TILE_THREAD_WIDTH 16
#define TILE_THREAD_COUNT (TILE_THREAD_WIDTH * TILE_THREAD_WIDTH)

#define SPD_MAX_MIPS 12

struct FPushConstants
{
    uint2  PyramidSize;
    uint   NumMips;
    uint   NumWorkGroups;
    float2 InvPyramidSize;
    uint   SrcDepthIndex;
    uint   ReduceMax;       // 0 = min (reverse-Z camera depth), 1 = max (standard-Z shadow depth)
    uint*  AtomicCounter;
    uint   MipUAV[SPD_MAX_MIPS];
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Value that leaves a reduction unchanged, for the lanes whose destination texel is outside the pyramid.
float ReduceIdentity()
{
    return Pass().ReduceMax != 0u ? 0.0f : 1.0f;
}

// Ping-pong LDS: alternating reduction passes write to disjoint halves so no
// slot is read+written across a barrier.
groupshared float SpdLDS[2 * TILE_THREAD_COUNT];
groupshared bool  SpdIsLastWorkgroup;

float Reduce4(float a, float b, float c, float d)
{
    if (Pass().ReduceMax != 0u)
    {
        return max(max(a, b), max(c, d));
    }
    return min(min(a, b), min(c, d));
}

void StoreMip(uint Mip, int2 Pos, float Value)
{
    // Mips 5 and 11 are read by other workgroups in phase 2; route through the
    // coherent alias so the SPIR-V Coherent decoration lands on these stores.
    if (Mip == 5 || Mip == 11)
    {
        gRWTextures2DCoherent[Pass().MipUAV[Mip]][Pos] = float4(Value, 0, 0, 0);
    }
    else
    {
        gRWTextures2D[Pass().MipUAV[Mip]][Pos] = float4(Value, 0, 0, 0);
    }
}

float LoadMip5Coherent(int2 Pos, int2 MaxIdx)
{
    return gRWTextures2DCoherent[Pass().MipUAV[5]][clamp(Pos, int2(0, 0), MaxIdx)].x;
}

// Mip 0 is a 2x1-per-axis reduction of the source, so the reduction sampler has to match the mode: one tap
// covers the 2x2 source block exactly. Sampling a subset here would under-report the extent and over-cull.
float SampleSrcDepth(float2 UV)
{
    const uint Sampler = Pass().ReduceMax != 0u ? SAMPLER_MAX_REDUCTION : SAMPLER_MIN_REDUCTION;
    return gTextures2D[Pass().SrcDepthIndex].SampleLevel(gSamplers[Sampler], UV, 0.0f).x;
}

float4 DownsampleMipN0_BaseCase(uint2 Tile, uint2 TileThread)
{
    const float Identity = ReduceIdentity();
    float4 Quad = float4(Identity, Identity, Identity, Identity);

    [unroll]
    for (uint qi = 0; qi < 4; ++qi)
    {
        const int2 SubI = int2(qi >> 1, qi & 1);
        const int2 DstP = int2(Tile * TILE_THREAD_WIDTH + TileThread) * 2 + SubI;

        if (all(uint2(DstP) < Pass().PyramidSize))
        {
            const float2 UV = (float2(DstP) + 0.5f) * Pass().InvPyramidSize;
            const float  V  = SampleSrcDepth(UV);

            gRWTextures2D[Pass().MipUAV[0]][DstP] = float4(V, 0, 0, 0);
            Quad[qi] = V;
        }
    }
    return Quad;
}

float4 DownsampleMipN0_FromMip5(uint2 Tile, uint2 TileThread)
{
    const float Identity = ReduceIdentity();
    float4 Quad = float4(Identity, Identity, Identity, Identity);

    const uint2 SrcMipSize = max(Pass().PyramidSize >> 5u, uint2(1, 1));
    const uint2 DstMipSize = max(Pass().PyramidSize >> 6u, uint2(1, 1));
    const int2  SrcMaxIdx  = int2(SrcMipSize) - 1;

    [unroll]
    for (uint qi = 0; qi < 4; ++qi)
    {
        const int2 SubI = int2(qi >> 1, qi & 1);
        const int2 DstP = int2(Tile * TILE_THREAD_WIDTH + TileThread) * 2 + SubI;
        const int2 SrcP = DstP * 2;

        if (all(uint2(DstP) < DstMipSize))
        {
            const float A = LoadMip5Coherent(SrcP + int2(0, 0), SrcMaxIdx);
            const float B = LoadMip5Coherent(SrcP + int2(0, 1), SrcMaxIdx);
            const float C = LoadMip5Coherent(SrcP + int2(1, 0), SrcMaxIdx);
            const float D = LoadMip5Coherent(SrcP + int2(1, 1), SrcMaxIdx);
            const float V = Reduce4(A, B, C, D);

            gRWTextures2D[Pass().MipUAV[6]][DstP] = float4(V, 0, 0, 0);
            Quad[qi] = V;
        }
    }
    return Quad;
}

void DownsampleMipN1(int StartMip, uint2 Tile, uint2 TileThread, float4 Quad)
{
    const int   DstMip  = StartMip + 2;
    const float V       = Reduce4(Quad.x, Quad.y, Quad.z, Quad.w);
    const int2  DstP    = int2(Tile * TILE_THREAD_WIDTH + TileThread);
    const uint2 DstSize = max(Pass().PyramidSize >> uint(DstMip), uint2(1, 1));

    if (all(uint2(DstP) < DstSize))
    {
        StoreMip(uint(DstMip), DstP, V);
    }

    SpdLDS[TileThread.y * TILE_THREAD_WIDTH + TileThread.x] = V;
}

void DownsampleMipN2_N5(int StartMip, uint2 Tile, uint2 TileThread, int MipIndex)
{
    const int  DstMip  = StartMip + 3 + MipIndex;
    const uint PingSrc = uint(MipIndex & 1);
    const uint PingDst = uint((MipIndex + 1) & 1);

    GroupMemoryBarrierWithGroupSync();

    const uint2 RemainingTile = uint2(1, 1) * uint(TILE_THREAD_WIDTH >> (MipIndex + 1));
    const int2  DstP    = int2(TileThread + Tile * RemainingTile);
    const int2  SrcP    = int2(TileThread) * 2;
    const uint2 DstSize = max(Pass().PyramidSize >> uint(DstMip), uint2(1, 1));
    const bool  Active  = all(TileThread < RemainingTile) && all(uint2(DstP) < DstSize);

    if (Active)
    {
        const float A = SpdLDS[PingSrc * TILE_THREAD_COUNT + (SrcP.y + 0) * TILE_THREAD_WIDTH + (SrcP.x + 0)];
        const float B = SpdLDS[PingSrc * TILE_THREAD_COUNT + (SrcP.y + 0) * TILE_THREAD_WIDTH + (SrcP.x + 1)];
        const float C = SpdLDS[PingSrc * TILE_THREAD_COUNT + (SrcP.y + 1) * TILE_THREAD_WIDTH + (SrcP.x + 0)];
        const float D = SpdLDS[PingSrc * TILE_THREAD_COUNT + (SrcP.y + 1) * TILE_THREAD_WIDTH + (SrcP.x + 1)];
        const float V = Reduce4(A, B, C, D);

        SpdLDS[PingDst * TILE_THREAD_COUNT + TileThread.y * TILE_THREAD_WIDTH + TileThread.x] = V;
        StoreMip(uint(DstMip), DstP, V);
    }
}

void DownsampleTile_Phase1(uint2 Tile, uint2 TileThread)
{
    if (Pass().NumMips < 1) return;
    const float4 Quad = DownsampleMipN0_BaseCase(Tile, TileThread);

    if (Pass().NumMips < 2) return;
    DownsampleMipN1(-1, Tile, TileThread, Quad);

    [unroll]
    for (int mi = 0; mi < 4; ++mi)
    {
        if (int(Pass().NumMips) <= -1 + 3 + mi) return;
        DownsampleMipN2_N5(-1, Tile, TileThread, mi);
    }
}

void DownsampleTile_Phase2(uint2 Tile, uint2 TileThread)
{
    if (Pass().NumMips <= 6) return;
    const float4 Quad = DownsampleMipN0_FromMip5(Tile, TileThread);

    if (Pass().NumMips <= 7) return;
    DownsampleMipN1(5, Tile, TileThread, Quad);

    [unroll]
    for (int mi = 0; mi < 4; ++mi)
    {
        if (int(Pass().NumMips) <= 5 + 3 + mi) return;
        DownsampleMipN2_N5(5, Tile, TileThread, mi);
    }
}

[shader("compute")]
[numthreads(TILE_THREAD_WIDTH, TILE_THREAD_WIDTH, 1)]
void ComputeMain(
    uint3 GroupID  : SV_GroupID,
    uint3 ThreadID : SV_GroupThreadID)
{
    const uint2 Tile = GroupID.xy;
    const uint2 TT   = ThreadID.xy;

    DownsampleTile_Phase1(Tile, TT);

    if (Pass().NumMips <= 6) return;

    // Cross-WG hand-off: the atomic on the BDA counter is the only Device-scope
    // memory event; the bool broadcasts the result through a workgroup barrier.
    if (all(TT == uint2(0, 0)))
    {
        uint OldCount;
        InterlockedAdd(Pass().AtomicCounter[0], 1u, OldCount);
        SpdIsLastWorkgroup = ((OldCount + 1u) == Pass().NumWorkGroups);
    }

    GroupMemoryBarrierWithGroupSync();

    if (!SpdIsLastWorkgroup) return;

    if (all(TT == uint2(0, 0)))
    {
        Pass().AtomicCounter[0] = 0u;
    }

    // Walk mip-5 in 64x64 sub-tiles. Pyramids up to 2048-wide fit in one.
    const uint2 Mip5Size      = max(Pass().PyramidSize >> 5u, uint2(1, 1));
    const uint2 Mip5TileCount = max(Mip5Size >> 6u, uint2(1, 1));

    for (uint ty = 0; ty < Mip5TileCount.y; ++ty)
    {
        for (uint tx = 0; tx < Mip5TileCount.x; ++tx)
        {
            DownsampleTile_Phase2(uint2(tx, ty), TT);
        }
    }
}
