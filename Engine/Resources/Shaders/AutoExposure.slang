#pragma once
#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"

// Auto exposure: one workgroup log-averages a 16x16 grid over the HDR frame (geometric mean =
// key luminance), then eases the persistent 1x1 R32F adapted value toward it like an eye adapting.
// Bindless resources via push-constant indices, no pass-local binding set.

#define TILE_DIM     16
#define THREAD_COUNT (TILE_DIM * TILE_DIM)

struct FPushConstants
{
    uint  HDRIndex;         // bindless SRV index of HDR scene color
    uint  AdaptUAV;         // bindless UAV index of the 1x1 adapted-luminance image
    float DeltaTime;        // seconds since last frame, for adaptation rate
    float AdaptationSpeed;  // eye-adaptation rate (per second)
};

FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

groupshared float gLogLum[THREAD_COUNT];

[shader("compute")]
[numthreads(TILE_DIM, TILE_DIM, 1)]
void ComputeMain(uint3 LocalID : SV_GroupThreadID)
{
    uint Local = LocalID.y * TILE_DIM + LocalID.x;

    // Sample the cell center of a TILE_DIM x TILE_DIM grid over the whole frame.
    float2 UV = (float2(LocalID.xy) + 0.5) / float(TILE_DIM);
    float3 Color = gTextures2D[Pass().HDRIndex].SampleLevel(
        gSamplers[SAMPLER_LINEAR_CLAMP], UV, 0.0).rgb;

    float Luma = max(Luminance(Color), 1e-5);
    gLogLum[Local] = log(Luma);

    GroupMemoryBarrierWithGroupSync();

    // Parallel sum reduction over the 256 log-luminance samples.
    [unroll]
    for (uint s = THREAD_COUNT / 2; s > 0; s >>= 1)
    {
        if (Local < s)
        {
            gLogLum[Local] += gLogLum[Local + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (Local == 0)
    {
        float TargetLum = exp(gLogLum[0] / float(THREAD_COUNT));

        float PrevLum = gRWTextures2D[Pass().AdaptUAV][int2(0, 0)].r;
        // Seed the feedback on the first run (1x1 image starts uncleared). The range test also
        // catches NaN/Inf (any NaN comparison is false), so we snap to the target, not garbage.
        if (!(PrevLum > 1e-6 && PrevLum < 1e6))
        {
            PrevLum = TargetLum;
        }

        // Exponential approach, framerate independent.
        float Blend     = 1.0 - exp(-Pass().DeltaTime * Pass().AdaptationSpeed);
        float AdaptedLum = PrevLum + (TargetLum - PrevLum) * Blend;

        gRWTextures2D[Pass().AdaptUAV][int2(0, 0)] = float4(AdaptedLum, 0.0, 0.0, 0.0);
    }
}
