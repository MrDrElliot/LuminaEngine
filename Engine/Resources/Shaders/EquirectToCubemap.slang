#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"
#include "Includes/Sky.slang"

// Bakes an HDR equirect panorama into the scene's 6-face cube map (HDRI path; downstream IBL is identical).
// Addressing is Sky_EquirectUV, shared with the visible sky pass.
//
// Intensity and yaw are applied HERE, not just on the visible sky: this cube is what the irradiance and
// GGX prefilter convolve from, so baking them in is what keeps ambient and reflections agreeing with the
// background. The bake is gated on the environment params changing, so both are free per-frame.

// No pass-local set: equirect read via bindless 2D SRV, cube written via bindless 2D-array UAV.
struct FPushConstants
{
    uint  EquirectSRV;
    uint  SkyCubeUAV;
    float Intensity;
    float CosYaw;
    float SinYaw;
    uint  _Pad0;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

#define EQUIRECT_TILE 8

[shader("compute")]
[numthreads(EQUIRECT_TILE, EQUIRECT_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    RWTexture2DArray<float4> uSkyCube = gRWTextures2DArray[Pass().SkyCubeUAV];

    uint Width, Height, Layers;
    uSkyCube.GetDimensions(Width, Height, Layers);

    if (DTid.x >= Width || DTid.y >= Height || DTid.z >= Layers)
    {
        return;
    }

    // Texel-centered face UV; same convention as SkyCubeCapture so IBL addressing matches either source.
    float2 FaceUV = (float2(DTid.xy) + 0.5) / float2(Width, Height);
    float3 RayDir = CubeFaceDirection(DTid.z, FaceUV);

    float2 EquirectUV = Sky_EquirectUV(RayDir, Pass().CosYaw, Pass().SinYaw);

    // Sanitize here: this is the gate between the raw HDR panorama (which may hold Inf/NaN) and the IBL
    // cube the convolutions read, so a finite cube keeps the whole downstream chain stable.
    float3 Color = SanitizeRadiance(SampleTexture2DLevel(Pass().EquirectSRV, SAMPLER_LINEAR_CLAMP, EquirectUV, 0.0).rgb);
    Color *= max(Pass().Intensity, 0.0);

    uSkyCube[DTid] = float4(Color, 1.0);
}
