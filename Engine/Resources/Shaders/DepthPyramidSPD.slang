#pragma once
#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"

// Reverse-Z camera pyramids reduce with min and standard-Z cascade atlases with max; ReduceMax is a push constant so the branch is wave-uniform.

#define TILE_THREAD_WIDTH 16
#define TILE_THREAD_COUNT (TILE_THREAD_WIDTH * TILE_THREAD_WIDTH)

#define SPD_MAX_MIPS 12

struct FPushConstants
{
    uint2         PyramidSize;
    uint          NumMips;
    uint          NumWorkGroups;
    float2        InvPyramidSize;
    FTexture2D    SrcDepth;
    uint          ReduceMax;       // 0 = min (reverse-Z camera depth), 1 = max (standard-Z shadow depth)
    GPUSpan<uint> AtomicCounter;
    uint          MipUAV[SPD_MAX_MIPS];
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Value that leaves a reduction unchanged, for the lanes whose destination texel is outside the pyramid.
float ReduceIdentity()
{
    return Pass().ReduceMax != 0u ? 0.0f : 1.0f;
}

// Ping-pong halves so no slot is read and written across a barrier.
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
    // Other workgroups read mips 5 and 11 in phase 2, so these stores need the Coherent decoration.
    if (Mip == 5 || Mip == 11)
    {
        FRWTexture2DCoherent(Pass().MipUAV[Mip])[Pos] = float4(Value, 0, 0, 0);
    }
    else
    {
        FRWTexture2D(Pass().MipUAV[Mip])[Pos] = float4(Value, 0, 0, 0);
    }
}

float LoadMip5Coherent(int2 Pos, int2 MaxIdx)
{
    return FRWTexture2DCoherent(Pass().MipUAV[5])[clamp(Pos, int2(0, 0), MaxIdx)].x;
}

// One reduction-sampler tap covers the 2x2 source block, so the sampler must match the mode or it over-culls.
float SampleSrcDepth(float2 UV)
{
    const uint Sampler = Pass().ReduceMax != 0u ? SAMPLER_MAX_REDUCTION : SAMPLER_MIN_REDUCTION;
    return Pass().SrcDepth.SampleLevel(FSampler(Sampler), UV, 0.0f).x;
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

            FRWTexture2D(Pass().MipUAV[0])[DstP] = float4(V, 0, 0, 0);
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

            FRWTexture2D(Pass().MipUAV[6])[DstP] = float4(V, 0, 0, 0);
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

    // Lanes past the mip edge still write LDS, or the next level reduces a slot left over from two levels ago.
    if (all(TileThread < RemainingTile))
    {
        const float A = SpdLDS[PingSrc * TILE_THREAD_COUNT + (SrcP.y + 0) * TILE_THREAD_WIDTH + (SrcP.x + 0)];
        const float B = SpdLDS[PingSrc * TILE_THREAD_COUNT + (SrcP.y + 0) * TILE_THREAD_WIDTH + (SrcP.x + 1)];
        const float C = SpdLDS[PingSrc * TILE_THREAD_COUNT + (SrcP.y + 1) * TILE_THREAD_WIDTH + (SrcP.x + 0)];
        const float D = SpdLDS[PingSrc * TILE_THREAD_COUNT + (SrcP.y + 1) * TILE_THREAD_WIDTH + (SrcP.x + 1)];
        const float V = Reduce4(A, B, C, D);

        SpdLDS[PingDst * TILE_THREAD_COUNT + TileThread.y * TILE_THREAD_WIDTH + TileThread.x] = V;
        if (all(uint2(DstP) < DstSize))
        {
            StoreMip(uint(DstMip), DstP, V);
        }
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

    // Mip-5 stores must reach device scope before the counter publishes them, and be acquired after it.
    DeviceMemoryBarrierWithGroupSync();

    if (all(TT == uint2(0, 0)))
    {
        uint OldCount;
        InterlockedAdd(Pass().AtomicCounter.At(0u)[0], 1u, OldCount);
        SpdIsLastWorkgroup = ((OldCount + 1u) == Pass().NumWorkGroups);
    }

    AllMemoryBarrierWithGroupSync();

    if (!SpdIsLastWorkgroup) return;

    if (all(TT == uint2(0, 0)))
    {
        Pass().AtomicCounter.Set(0u, 0u);
    }

    // Walk mip 5 in 64x64 sub-tiles.
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
