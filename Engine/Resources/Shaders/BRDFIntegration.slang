#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHIStorage.slang"
#include "Includes/IBL.slang"

// Bakes the split-sum BRDF integration LUT once at init, indexed by (NdotV, Roughness) -> (scale, bias);
// runtime IBL specular collapses to one multiply-add against the pre-filtered cubemap.

// Matches FBRDFArgs in ForwardRenderScene.cpp (BakeBRDFLUT).
struct FBRDFArgs
{
    uint OutUAV;    // storage heap slot of the LUT
    uint _Pad0;
    uint _Pad1;
    uint _Pad2;
};

FBRDFArgs* Pass() { return GetPassArgs<FBRDFArgs>(); }

#define BRDF_LUT_TILE 8

[shader("compute")]
[numthreads(BRDF_LUT_TILE, BRDF_LUT_TILE, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    uint Width, Height;
    gRWTextures2D[Pass().OutUAV].GetDimensions(Width, Height);

    if (DTid.x >= Width || DTid.y >= Height)
    {
        return;
    }

    // Sample at texel centers so a linear-clamp fetch at UV 0 / 1 reads
    // the corresponding edge values without bleeding past the table.
    float NdotV     = (float(DTid.x) + 0.5) / float(Width);
    float Roughness = (float(DTid.y) + 0.5) / float(Height);

    float2 BRDF = IntegrateBRDF(NdotV, Roughness);
    gRWTextures2D[Pass().OutUAV][DTid.xy] = float4(BRDF, 0.0, 0.0);
}
