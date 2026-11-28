#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/XeGTAO.slang"

// Indirect diffuse traced at half resolution, rgb the bounced light and a the cosine-weighted fraction of the hemisphere it came from.

#if !defined(SSGI_BLUR) && !defined(SSGI_TEMPORAL)

struct FPushConstants
{
    float4x4     HistoryViewProjection;
    uint2        OutputSize;
    uint2        WorkingDepthSize;
    FTexture2D   WorkingDepth;
    FTexture2D   SceneColorHistory;
    FRWTexture2D Output;
    uint         DepthScale;
    uint         SliceCount;
    uint         StepsPerSlice;
    float        EffectRadius;
    float        EffectFalloffRange;
    float        SampleDistributionPower;
    float        ThinOccluderCompensation;
    float        DepthMIPSamplingOffset;
    uint         NoiseIndex;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Whether a hit's surface faces the receiver, from the working depth around it, so the lit side of an object never bounces onto what lies behind it.
float HitFacing(FGTAOView V, FTexture2D WorkingDepth, float2 SampleUV, float MipLevel, float3 SamplePos, float3 CenterPos)
{
    const float2 Texel = V.ViewportPixelSize * exp2(MipLevel);
    const float  ZX    = WorkingDepth.SampleLevel(Samplers::PointClamp, SampleUV + float2(Texel.x, 0.0), MipLevel).x;
    const float  ZY    = WorkingDepth.SampleLevel(Samplers::PointClamp, SampleUV + float2(0.0, Texel.y), MipLevel).x;
    const float3 PX    = XeGTAO_ComputeViewspacePosition(V, SampleUV + float2(Texel.x, 0.0), ZX);
    const float3 PY    = XeGTAO_ComputeViewspacePosition(V, SampleUV + float2(0.0, Texel.y), ZY);

    float3 Normal = cross(PX - SamplePos, PY - SamplePos);
    const float Length = length(Normal);
    if (Length < 1e-8)
    {
        return 1.0;
    }
    Normal /= Length;
    // A visible surface faces the eye, which settles the sign the cross product leaves open.
    Normal = dot(Normal, -SamplePos) < 0.0 ? -Normal : Normal;
    // Diffuse radiance is the same toward every direction a surface faces, so only a face turned away goes dark, softened by the coarse normal.
    return smoothstep(-0.08, 0.04, dot(Normal, normalize(CenterPos - SamplePos)));
}

// Last frame's color at the view space position ViewPos with a = 1, or zero where that surface was off screen or behind the eye.
float4 HitRadiance(float4x4 ViewToHistoryClip, float3 ViewPos)
{
    const float4 PrevClip = mul(ViewToHistoryClip, float4(ViewPos, 1.0));
    if (PrevClip.w <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float2 PrevUV = (PrevClip.xy / PrevClip.w) * 0.5 + 0.5;
    if (any(PrevUV != saturate(PrevUV)))
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return float4(Pass().SceneColorHistory.SampleLevel(Samplers::LinearClamp, PrevUV, 0.0).rgb, 1.0);
}

[shader("compute")]
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 PixCoord = DTid.xy;
    if (any(PixCoord >= Pass().OutputSize))
    {
        return;
    }

    const FGTAOView   V          = MakeGTAOView(Pass().WorkingDepthSize);
    const uint2       DepthCoord = min(PixCoord * Pass().DepthScale, Pass().WorkingDepthSize - 1u);
    const float2      ScreenUV   = (float2(DepthCoord) + 0.5) * V.ViewportPixelSize;
    const FGTAOCenter Center     = XeGTAO_LoadCenter(V, Pass().WorkingDepth, DepthCoord);

    // The same nudge GTAOMain makes, so a texel never finds its own depth above its horizon.
    const float  ViewspaceZ = Center.ViewspaceZ * 0.99999;
    const float3 CenterPos  = XeGTAO_ComputeViewspacePosition(V, ScreenUV, ViewspaceZ);
    const float3 ViewVec    = normalize(-CenterPos);

    const float ScreenspaceRadius = Pass().EffectRadius / max(abs(V.NDCToViewMulPixelSize.x * ViewspaceZ), 1e-8);
    if (ScreenspaceRadius < XE_GTAO_PIXEL_TOO_CLOSE)
    {
        Pass().Output[PixCoord] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    const FGTAOFalloff Falloff = XeGTAO_MakeFalloff(Pass().EffectRadius, Pass().EffectFalloffRange, Pass().ThinOccluderCompensation);
    const float2       Noise   = XeGTAO_SpatioTemporalNoise(PixCoord, Pass().NoiseIndex);
    const float        MinS    = XE_GTAO_PIXEL_TOO_CLOSE / ScreenspaceRadius;

    // XeGTAO's view space is the engine's, so a hit reprojects without going back through scene depth.
    const float4x4 ViewToHistoryClip = mul(Pass().HistoryViewProjection, GetInverseCameraView());

    const uint SliceCount    = max(Pass().SliceCount, 1u);
    const uint StepsPerSlice = max(Pass().StepsPerSlice, 1u);

    float3 Bounce       = float3(0.0, 0.0, 0.0);
    float  BounceWeight = 0.0;
    float  Coverage     = 0.0;

    [loop]
    for (uint Slice = 0; Slice < SliceCount; ++Slice)
    {
        const FGTAOSlice S = XeGTAO_MakeSlice(V, (float(Slice) + Noise.x) / float(SliceCount) * XE_GTAO_PI,
                                              ScreenspaceRadius, ViewVec, Center.Normal);

        // Side 0 marches along Omega and opens toward positive angles, side 1 the mirror of it.
        float LowCos[2]     = { S.LowHorizonCos0, S.LowHorizonCos1 };
        float HorizonCos[2] = { S.LowHorizonCos0, S.LowHorizonCos1 };
        float Arc[2]        = { XeGTAO_SliceArc(S, XeGTAO_FastACos(S.LowHorizonCos0)),
                                XeGTAO_SliceArc(S, -XeGTAO_FastACos(S.LowHorizonCos1)) };

        float3 SliceBounce       = float3(0.0, 0.0, 0.0);
        float  SliceBounceWeight = 0.0;
        float  SliceCoverage     = 0.0;

        [loop]
        for (uint Step = 0; Step < StepsPerSlice; ++Step)
        {
            const float Distance = XeGTAO_StepDistance(Slice, Step, StepsPerSlice, Noise.y, Pass().SampleDistributionPower, MinS);

            float MipLevel;
            const float2 SampleOffset = XeGTAO_StepOffsetUV(V, S, Distance, Pass().DepthMIPSamplingOffset, MipLevel);

            [unroll]
            for (uint Side = 0; Side < 2; ++Side)
            {
                const float2 SampleUV = (Side == 0u) ? ScreenUV + SampleOffset : ScreenUV - SampleOffset;
                const float  SampleZ  = Pass().WorkingDepth.SampleLevel(Samplers::PointClamp, SampleUV, MipLevel).x;
                const float3 SamplePos = XeGTAO_ComputeViewspacePosition(V, SampleUV, SampleZ);
                const float  SampleCos = XeGTAO_SampleHorizonCos(Falloff, SamplePos - CenterPos, ViewVec, LowCos[Side]);
                if (SampleCos <= HorizonCos[Side])
                {
                    continue;
                }

                // Only the arc a sample newly hides above the horizon so far is light it can bounce in.
                const float Angle   = XeGTAO_FastACos(SampleCos);
                const float NewArc  = XeGTAO_SliceArc(S, (Side == 0u) ? Angle : -Angle);
                const float Covered = max(Arc[Side] - NewArc, 0.0);

                // Weighting by 1 / (1 + luma) keeps one hit on a specular highlight from becoming a firefly.
                const float4 Hit       = HitRadiance(ViewToHistoryClip, SamplePos);
                const float  HitWeight = Hit.a * Covered / (1.0 + Luminance(Hit.rgb));

                // A hit facing away still blocks the sky, it just sends no light this way, so only its radiance drops.
                const float Facing = HitWeight > 0.0 ? HitFacing(V, Pass().WorkingDepth, SampleUV, MipLevel, SamplePos, CenterPos) : 0.0;

                SliceBounce       += HitWeight * Facing * Hit.rgb;
                SliceBounceWeight += HitWeight;
                SliceCoverage     += Covered;
                HorizonCos[Side] = SampleCos;
                Arc[Side]        = NewArc;
            }
        }

        Bounce       += S.ProjNLen * SliceBounce;
        BounceWeight += S.ProjNLen * SliceBounceWeight;
        Coverage     += S.ProjNLen * SliceCoverage;
    }

    // The weighted mean radiance of the hits with history, scaled by how much of the hemisphere every hit covers.
    const float CoveredFraction = saturate(Coverage / float(SliceCount));
    Pass().Output[PixCoord] = float4(Bounce / max(BounceWeight, 1e-6) * CoveredFraction, CoveredFraction);
}

#elif defined(SSGI_TEMPORAL)

struct FPushConstants
{
    float4x4     HistoryViewProjection;
    uint2        Size;
    uint2        WorkingDepthSize;
    FTexture2D   WorkingDepth;
    FTexture2D   Current;
    FTexture2D   PrevAccum;
    FTexture2D   PrevDepth;
    FRWTexture2D OutAccum;
    FRWTexture2D OutDepth;
    uint         DepthScale;
    uint         bHistoryValid;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Frames of history a pixel can hold, so the newest trace always keeps this share of the result.
static const float kMaxHistoryFrames = 16.0;
// Last frame's surface may sit this fraction of the depth away and still count as the same one.
static const float kHistoryDepthTolerance = 0.04;

// Blends this frame's trace into the reprojected history, restarting wherever the surface behind was hidden last frame.
[shader("compute")]
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const int2 Pixel = int2(DTid.xy);
    if (any(DTid.xy >= Pass().Size))
    {
        return;
    }

    const FGTAOView V          = MakeGTAOView(Pass().WorkingDepthSize);
    const uint2     DepthCoord = min(DTid.xy * Pass().DepthScale, Pass().WorkingDepthSize - 1u);
    const float2    ScreenUV   = (float2(DepthCoord) + 0.5) * V.ViewportPixelSize;
    const float     Depth      = Pass().WorkingDepth.Load(int3(DepthCoord, 0)).x;
    const float4    Current    = Pass().Current.Load(int3(Pixel, 0));

    // The newest trace's own neighborhood bounds the history, so lighting that changed does not linger as a ghost.
    float4 Low  = Current;
    float4 High = Current;
    const int2 MaxTap = int2(Pass().Size) - 1;
    [unroll]
    for (int Y = -1; Y <= 1; ++Y)
    {
        [unroll]
        for (int X = -1; X <= 1; ++X)
        {
            const float4 Tap = Pass().Current.Load(int3(clamp(Pixel + int2(X, Y), int2(0, 0), MaxTap), 0));
            Low  = min(Low, Tap);
            High = max(High, Tap);
        }
    }

    float4 History = Current;
    float  Frames  = 0.0;
    if (Pass().bHistoryValid != 0u)
    {
        const float3 ViewPos  = XeGTAO_ComputeViewspacePosition(V, ScreenUV, Depth);
        const float4 PrevClip = mul(mul(Pass().HistoryViewProjection, GetInverseCameraView()), float4(ViewPos, 1.0));
        if (PrevClip.w > 0.0)
        {
            const float2 PrevUV = (PrevClip.xy / PrevClip.w) * 0.5 + 0.5;
            if (all(PrevUV == saturate(PrevUV)))
            {
                // Bilinear by hand, so each corner that was a different surface drops out instead of bleeding in.
                const float2 Texel    = PrevUV * float2(Pass().Size) - 0.5;
                const int2   Base     = int2(floor(Texel));
                const float2 F        = Texel - float2(Base);
                const bool   bOrtho   = IsCameraOrthographic();
                const float  Expected = bOrtho ? Depth : PrevClip.w;

                float4 Sum    = float4(0.0, 0.0, 0.0, 0.0);
                float  Count  = 0.0;
                float  Weight = 0.0;
                [unroll]
                for (uint i = 0u; i < 4u; ++i)
                {
                    const int2   Offset = int2(i & 1u, i >> 1u);
                    const int2   Tap    = clamp(Base + Offset, int2(0, 0), MaxTap);
                    const float2 Stored = Pass().PrevDepth.Load(int3(Tap, 0)).xy;
                    const float  Bilin  = (Offset.x == 1 ? F.x : 1.0 - F.x) * (Offset.y == 1 ? F.y : 1.0 - F.y);
                    const float  Same   = abs(Stored.x - Expected) <= Expected * kHistoryDepthTolerance ? 1.0 : 0.0;
                    const float  W      = Bilin * Same;
                    Sum    += Pass().PrevAccum.Load(int3(Tap, 0)) * W;
                    Count  += Stored.y * W;
                    Weight += W;
                }
                if (Weight > 0.05)
                {
                    History = clamp(Sum / Weight, Low, High);
                    Frames  = Count / Weight;
                }
            }
        }
    }

    const float NewFrames = min(Frames + 1.0, kMaxHistoryFrames);
    Pass().OutAccum[Pixel] = lerp(History, Current, 1.0 / NewFrames);
    Pass().OutDepth[Pixel] = float4(Depth, NewFrames, 0.0, 0.0);
}

#else

struct FPushConstants
{
    uint2        Size;
    uint2        WorkingDepthSize;
    int2         Direction;
    FTexture2D   WorkingDepth;
    FTexture2D   Source;
    FRWTexture2D Output;
    uint         DepthScale;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

// Narrower than before the trace was accumulated, since the history now carries most of the denoising.
static const int kBlurRadius = 2;

float TexelDepth(int2 Texel)
{
    const uint2 DepthCoord = min(uint2(Texel) * Pass().DepthScale, Pass().WorkingDepthSize - 1u);
    return Pass().WorkingDepth.Load(int3(DepthCoord, 0)).r;
}

// One axis of a depth-weighted blur over the half size trace, run once across and once down.
[shader("compute")]
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (any(DTid.xy >= Pass().Size))
    {
        return;
    }

    const int2  Center    = int2(DTid.xy);
    const float Z         = TexelDepth(Center);
    const float InvSigmaZ = 1.0 / (XE_GTAO_DEPTH_SIGMA * Z);
    const int2  MaxTap    = int2(Pass().Size) - 1;

    float4 Sum       = float4(0.0, 0.0, 0.0, 0.0);
    float  SumWeight = 0.0;

    [unroll]
    for (int i = -kBlurRadius; i <= kBlurRadius; ++i)
    {
        const int2   Tap    = clamp(Center + Pass().Direction * i, int2(0, 0), MaxTap);
        const float4 Value  = Pass().Source.Load(int3(Tap, 0));
        // Weighting by 1 / (1 + luma) keeps one bright history hit from surviving the blur as a block.
        const float  Weight = XeGTAO_DepthWeight(abs(TexelDepth(Tap) - Z), InvSigmaZ) / (1.0 + Luminance(Value.rgb));

        Sum       += Value * Weight;
        SumWeight += Weight;
    }

    Pass().Output[DTid.xy] = Sum / max(SumWeight, 1e-6);
}

#endif
