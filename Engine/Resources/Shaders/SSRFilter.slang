#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"

// Builds one level of the reflection blur chain from the level below, a 2x downsample followed by a separable 7-tap Gaussian.

struct FSSRChainLevelArgs
{
    FTexture2D   Chain;  // the whole chain, read at SourceMip
    uint         SourceMip;
    FRWTexture2D Dest;
    uint         DestMip;

    uint SourceW;
    uint SourceH;
    uint DestW;
    uint DestH;
};
FSSRChainLevelArgs* Pass() { return GetPassArgs<FSSRChainLevelArgs>(); }

static const float kGaussian[7] =
{
    0.07130343198685299,
    0.1315141208431224,
    0.18987923288883812,
    0.21460642856237303,
    0.18987923288883812,
    0.1315141208431224,
    0.07130343198685299,
};

// An 8x8 group writes 8x8 texels and needs three more on each side for the kernel.
groupshared float4 gsCache[16][16];
groupshared float4 gsHorizontal[8][16];

// A miss weighs little in a sharp level and nearly as much as a hit in a wide one, so rough reflections thin out gradually.
float MissAwareWeight(float4 C)
{
    return lerp(saturate(float(Pass().DestMip) * 0.2), 1.0, C.a);
}

float4 SampleParent(int2 Texel)
{
    const float2 UV = (float2(Texel) + 0.5) / float2(Pass().DestW, Pass().DestH);
    return Pass().Chain.SampleLevel(Samplers::LinearClamp, UV, float(Pass().SourceMip));
}

float4 WeightedAverage(float4 Sum, float Weight)
{
    return Weight > 0.0 ? Sum / Weight : float4(0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID)
{
    const int2 Global = int2(DTid.xy);
    const int2 Local  = int2(GTid.xy);

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const int2 Offset = int2(i & 1, i >> 1);
        gsCache[Local.x * 2 + Offset.x][Local.y * 2 + Offset.y] = SampleParent(Global + Local - 4 + Offset);
    }
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (int Row = 0; Row < 2; ++Row)
    {
        const int Y = Local.y * 2 + Row;
        float4 Sum    = float4(0.0);
        float  Weight = 0.0;
        [unroll]
        for (int k = 0; k < 7; ++k)
        {
            const float4 C = gsCache[Local.x + 1 + k][Y];
            const float  W = kGaussian[k] * MissAwareWeight(C);
            Sum    += C * W;
            Weight += W;
        }
        gsHorizontal[Local.x][Y] = WeightedAverage(Sum, Weight);
    }
    GroupMemoryBarrierWithGroupSync();

    if (DTid.x >= Pass().DestW || DTid.y >= Pass().DestH)
    {
        return;
    }

    float4 Sum    = float4(0.0);
    float  Weight = 0.0;
    [unroll]
    for (int k = 0; k < 7; ++k)
    {
        const float4 C = gsHorizontal[Local.x][Local.y + 1 + k];
        const float  W = kGaussian[k] * MissAwareWeight(C);
        Sum    += C * W;
        Weight += W;
    }
    Pass().Dest[Global] = WeightedAverage(Sum, Weight);
}
