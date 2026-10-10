#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/IBL.slang"

// Split-sum LUT mapping (NdotV, Roughness) to (scale, bias).

// Matches FBRDFArgs in DefaultSceneRenderer.cpp (BakeBRDFLUT).
struct FBRDFArgs
{
    FRWTexture2D Out;
    // Passed, since GetDimensions on a dynamically indexed bindless RW image crashed AMDVLK under GPU-AV.
    uint         Width;
    uint         Height;
    uint         _Pad0;
};

FBRDFArgs* Pass() { return GetPassArgs<FBRDFArgs>(); }

#define BRDF_LUT_TILE 8

[shader("compute")]
[numthreads(BRDF_LUT_TILE, BRDF_LUT_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint Width  = Pass().Width;
    const uint Height = Pass().Height;

    if (DTid.x >= Width || DTid.y >= Height)
    {
        return;
    }

    // Texel centers, so a linear-clamp fetch at UV 0 or 1 reads the edge values without bleeding past the table.
    float NdotV     = (float(DTid.x) + 0.5) / float(Width);
    float Roughness = (float(DTid.y) + 0.5) / float(Height);

    float2 BRDF = IntegrateBRDF(NdotV, Roughness);
    Pass().Out[DTid.xy] = float4(BRDF, 0.0, 0.0);
}
