#pragma once
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/WorldDistanceField.slang"
#include "Includes/GBuffer.slang"
#include "Includes/TerrainHeightfield.slang"

// Sky visibility from cones traced through the world distance field, at half resolution, with the bent normal's lean.

// Mirror of Lumina::FDistanceFieldAOArgs.
struct FPushConstants
{
    uint2         OutputSize;
    uint2         DepthSize;
    FTexture2D     Depth;
    FTexture2D     GBufferB;
    FTexture2DUInt SlotImage;   // slot + 1 per pixel from the classify count, 0 where nothing deferred shades it
    FRWTexture2D   Output;
    float         MaxDistance;
    float         ViewDistance;
    FWorldDistanceField Field;
    uint                NumTerrains;
    FTerrainHeightfield Terrains[kMaxTerrainHeightfields];
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// One cone along the normal and a ring of eight, every one clear of the tangent plane so open ground stays fully lit.
static const uint  kNumCones        = 9u;
static const float kConeTanHalf     = 0.55;
static const float kRingCosPolar    = 0.6427876;
static const float kRingSinPolar    = 0.7660444;
// Occlusion fades in from this share of the reach, so the reach has no hard edge.
static const float kFadeStartFraction = 0.25;
// The environment below the horizon already stands for the ground, so a cone this far down counts no occluder it finds.
static const float kGroundConeY = -0.3;
static const float kSkyConeY    = 0.1;
// The ring is cosine weighted against the center cone, and both share one solid angle.
static const float kRingWeight      = kRingCosPolar;
static const float kTotalConeWeight = 1.0 + 8.0 * kRingWeight;

float3 ConeDirection(uint Cone, float3 N, float3 T, float3 B)
{
    if (Cone == 0u)
    {
        return N;
    }
    const float Phi = float(Cone - 1u) * (2.0 * 3.14159265 / 8.0);
    return normalize(N * kRingCosPolar + (T * cos(Phi) + B * sin(Phi)) * kRingSinPolar);
}

// Fixed sample distances rather than sphere tracing, so the nearest approach a cone finds moves smoothly with the receiver.
static const uint kSkyConeSteps = 10u;

float TraceSkyCone(FWorldDistanceField* Field, float3 Origin, float3 Dir, float Start, float MaxDistance, float FadeStart)
{
    const float InvFade = 1.0 / max(MaxDistance - FadeStart, 1e-4);
    const float Ratio   = MaxDistance / Start;

    float Visibility = 1.0;
    [loop]
    for (uint Step = 0; Step < kSkyConeSteps && Visibility > 0.0; ++Step)
    {
        const float t = Start * pow(Ratio, float(Step) / float(kSkyConeSteps - 1u));
        uint Level;
        const float Distance = Field.Sample(Origin + Dir * t, Level);
        if (Level >= kWorldDFLevels)
        {
            break;
        }
        const float Cone = saturate(Distance / (t * kConeTanHalf));
        Visibility = min(Visibility, lerp(Cone, 1.0, saturate((t - FadeStart) * InvFade)));
    }
    return Visibility;
}

void StoreOpen(uint2 Pixel)
{
    Pass().Output[Pixel] = float4(1.0, EncodeBentNormalOffset(float3(0.0, 0.0, 0.0)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 Pixel = DTid.xy;
    if (any(Pixel >= Pass().OutputSize))
    {
        return;
    }

    // The upsample compares against the same full resolution texel, so the trace stands on it too.
    const int2  Texel = int2(min(Pixel * 2u, Pass().DepthSize - 1u));
    const float Raw   = Pass().Depth.Load(int3(Texel, 0)).r;
    if (Raw <= 0.0)
    {
        StoreOpen(Pixel);
        return;
    }

    const float2 DepthSize = float2(Pass().DepthSize);
    const float2 UV        = (float2(Texel) + 0.5) / DepthSize;
    const float3 ViewPos   = ReconstructViewPos(UV, Raw, GetInverseCameraProjection());
    const float3 P         = LiftOntoTerrains(mul(GetInverseCameraView(), float4(ViewPos, 1.0)).xyz, Pass().Terrains, Pass().NumTerrains);
    const float  Distance  = length(P - GetCameraPosition());

    FWorldDistanceField* Field = &Pass().Field;
    const uint ReceiverLevel = Field.LevelAt(P);
    if (Distance >= Pass().ViewDistance || ReceiverLevel >= kWorldDFLevels)
    {
        StoreOpen(Pixel);
        return;
    }

    // The stored normal for deferred pixels, since depth's own normal follows a low poly mesh's facets.
    const float3 N = Pass().SlotImage.Load(Texel) != 0u
        ? normalize(OctDecode01(Pass().GBufferB.Load(int3(Texel, 0)).rg))
        : normalize(mul((float3x3)GetInverseCameraView(), GeometricViewNormal(Pass().Depth, Texel, Raw, ViewPos, DepthSize)));
    const float3 Ref = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 T   = normalize(cross(Ref, N));
    const float3 B   = cross(N, T);

    // Trilinear reads of a flat receiver's own distance are exact, so its cones clear it from half a voxel out without leaving the surface.
    const float3 Origin      = P + N * (0.02 + Distance * 0.001);
    const float  MaxDistance = Pass().MaxDistance;
    const float  FadeStart   = MaxDistance * kFadeStartFraction;
    const float  Start       = min(0.5 * Field.VoxelSize(ReceiverLevel), MaxDistance * 0.5);

    float ConeVisibility[kNumCones];
    [unroll]
    for (uint Cone = 0u; Cone < kNumCones; ++Cone)
    {
        ConeVisibility[Cone] = TraceSkyCone(Field, Origin, ConeDirection(Cone, N, T, B), Start, MaxDistance, FadeStart);
    }

    float  Visibility = 0.0;
    float3 Bent       = float3(0.0, 0.0, 0.0);
    [unroll]
    for (uint Cone = 0u; Cone < kNumCones; ++Cone)
    {
        const float3 Dir    = ConeDirection(Cone, N, T, B);
        const float  Open   = lerp(1.0, ConeVisibility[Cone], smoothstep(kGroundConeY, kSkyConeY, Dir.y));
        const float  Weight = (Cone == 0u ? 1.0 : kRingWeight) * Open;
        Visibility += Weight;
        Bent       += Dir * Weight;
    }
    Visibility /= kTotalConeWeight;

    // An open ring sums back to N, so the lean is only what the occluders took away.
    const float  BentLength = length(Bent);
    const float3 Lean       = BentLength > 1e-4 ? Bent / BentLength - N : float3(0.0, 0.0, 0.0);
    const float  Fade       = saturate((Distance - Pass().ViewDistance * 0.9) / (Pass().ViewDistance * 0.1));
    Pass().Output[Pixel] = float4(lerp(Visibility, 1.0, Fade), EncodeBentNormalOffset(Lean * (1.0 - Fade)));
}
