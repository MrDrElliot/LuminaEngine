#include "Includes/GlobalRHI.slang"
#include "Includes/HDROutput.slang"

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

// Turns the SDR window composite and its highlight gain into the HDR10 signal the swapchain presents.
[shader("fragment")]
float4 FragmentMain(VSOutput Input) : SV_Target0
{
    FHDROutputArgs* Args = GetArgs<FHDROutputArgs>();
    const float4 Encoded = Args.Composite.SampleLevel(Samplers::PointClamp, Input.UV, 0.0);
    const float3 Light   = DecodeDisplayComposite(Encoded, Args.GainStops);
    return float4(EncodeHDR10(Light, Args.PaperWhiteNits, Args.PeakNits), 1.0);
}
