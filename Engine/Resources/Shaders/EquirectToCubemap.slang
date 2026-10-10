#include "Includes/Common.slang"
#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/Sky.slang"

// Intensity and yaw are baked here because the IBL convolutions read this cube, not the visible sky.

struct FPushConstants
{
    FTexture2D        Equirect;
    FRWTexture2DArray SkyCube;
    float             Intensity;
    float             CosYaw;
    float             SinYaw;
    uint              _Pad0;
};
FPushConstants* Pass() { return GetPassArgs<FPushConstants>(); }

#define EQUIRECT_TILE 8

[shader("compute")]
[numthreads(EQUIRECT_TILE, EQUIRECT_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    FRWTexture2DArray uSkyCube = Pass().SkyCube;

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

    // The gate where a raw HDR panorama's Inf/NaN would otherwise reach every IBL convolution.
    float3 Color = SanitizeRadiance(Pass().Equirect.SampleLevel(Samplers::LinearClamp, EquirectUV, 0.0).rgb);
    Color *= max(Pass().Intensity, 0.0);

    uSkyCube[DTid] = float4(Color, 1.0);
}
