#include "Includes/SceneGlobals.slang"

// Underwater post-process.

// Must stay byte-identical to FWaterUnderwaterParams (SceneRenderTypes.h).
struct FWaterUnderwaterParams
{
    float4 PlaneNormalAndHeight;  // xyz = up normal, w = surface world Y under the camera
    float4 FogColorDensity;       // rgb = fog color, w = density (per meter)
    float4 TintDistortion;        // rgb = view tint, w = screen distortion amount
    float4 DeepColor;             // rgb = deep/absorption color
};

struct FUnderwaterPC
{
    uint64_t Params;      // FWaterUnderwaterParams, see LoadBlock
    uint SceneColorIndex;   // bindless 2D SRV: composited HDR scene copy
    uint SceneDepthIndex;   // bindless 2D SRV: opaque depth (raw reverse-Z NDC)
};
FUnderwaterPC* Pass() { return GetPassArgs<FUnderwaterPC>(); }

struct PSInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

[shader("fragment")]
float4 FragmentMain(PSInput Input) : SV_Target0
{
    FWaterUnderwaterParams P = LoadBlock<FWaterUnderwaterParams>(Pass().Params);

    float2 UV       = Input.UV;
    float  RawDepth = gTextures2D[Pass().SceneDepthIndex].SampleLevel(gSamplers[SAMPLER_POINT_CLAMP], UV, 0.0).r;

    float3 CameraPos = GetCameraPosition();
    // Reverse-Z: 0 = far/sky. Reconstructing at a tiny depth gives a far point along the view ray, so the
    // sky case still produces a valid ray that exits the water somewhere above the camera.
    float  D        = max(RawDepth, 1e-4);
    float3 WorldPos = ReconstructWorldPos(UV, D, GetInverseCameraProjection(), GetInverseCameraView());

    float PlaneY = P.PlaneNormalAndHeight.w;
    float CamH   = CameraPos.y - PlaneY;   // < 0 underwater
    float SceneH = WorldPos.y  - PlaneY;

    // Underwater sub-segment [tA,tB] of the view ray (t in [0,1] from camera to scene hit).
    float dH     = SceneH - CamH;
    float tCross = (abs(dH) > 1e-5) ? saturate(-CamH / dH) : ((CamH < 0.0) ? 1.0 : 0.0);
    float tA = 0.0;
    float tB = 0.0;
    if (CamH < 0.0 && SceneH < 0.0)      { tA = 0.0;    tB = 1.0;    }
    else if (CamH < 0.0)                 { tA = 0.0;    tB = tCross; }
    else if (SceneH < 0.0)               { tA = tCross; tB = 1.0;    }

    float UnderDist = max(tB - tA, 0.0) * length(WorldPos - CameraPos);

    // Screen warp while submerged (only meaningful where the camera itself is underwater).
    float  Distort = P.TintDistortion.w * step(0.0, -CamH);
    float2 Warp    = float2(sin(UV.y * 40.0 + GetTime() * 1.5), cos(UV.x * 40.0 + GetTime() * 1.3)) * Distort;
    float2 SampleUV = saturate(UV + Warp);

    float3 SceneColor = gTextures2D[Pass().SceneColorIndex].SampleLevel(gSamplers[SAMPLER_LINEAR_CLAMP], SampleUV, 0.0).rgb;

    // Beer-Lambert toward the fog color, with an overall tint on the transmitted scene.
    float  Trans  = exp(-UnderDist * P.FogColorDensity.w);
    float3 Tinted = SceneColor * P.TintDistortion.rgb;
    float3 Result = lerp(P.FogColorDensity.rgb, Tinted, saturate(Trans));

    return float4(Result, 1.0);
}
