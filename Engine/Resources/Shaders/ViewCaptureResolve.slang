#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"

// Copies a finished capture into its render target, turning the pixels nothing was drawn on into the capture's background.

// Must match FViewCaptureResolveArgs in SceneRenderer_ViewCapture.cpp.
struct FViewCaptureResolveArgs
{
    FTexture2D   Color;
    FTexture2D   Depth;
    FRWTexture2D Target;
    uint         bComposite;
    uint2        Size;
    uint2        _Pad;
    // Linear, composited under the capture.
    float4       BackgroundColor;
};
FViewCaptureResolveArgs* Pass() { return GetPassArgs<FViewCaptureResolveArgs>(); }

float3 EncodeSRGB(float3 Linear)
{
    const float3 C = saturate(Linear);
    return select(C <= 0.0031308, C * 12.92, 1.055 * pow(C, 1.0 / 2.4) - 0.055);
}

float MaxChannel(float3 C)
{
    return max(C.r, max(C.g, C.b));
}

bool IsCovered(int2 P)
{
    return Pass().Depth.Load(clamp(P, int2(0, 0), int2(Pass().Size) - 1)).r > 0.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 DTid : SV_DispatchThreadID)
{
    if (any(DTid.xy >= Pass().Size))
    {
        return;
    }

    const int2   P     = int2(DTid.xy);
    const float3 Color = Pass().Color.Load(P).rgb;
    if (Pass().bComposite == 0u)
    {
        Pass().Target[P] = float4(Color, 1.0);
        return;
    }

    // The background rendered black, so an edge the anti-aliasing blended is its surface color scaled by coverage, read back against the brightest drawn neighbor.
    float Alpha = 1.0;
    if (!IsCovered(P))
    {
        float Neighbor = 0.0;
        const int2 Offsets[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };
        for (uint i = 0; i < 4u; ++i)
        {
            if (IsCovered(P + Offsets[i]))
            {
                Neighbor = max(Neighbor, MaxChannel(Pass().Color.Load(clamp(P + Offsets[i], int2(0, 0), int2(Pass().Size) - 1)).rgb));
            }
        }
        Alpha = Neighbor > 0.0 ? saturate(MaxChannel(Color) / Neighbor) : 0.0;
    }

    // Composited premultiplied, then stored straight, which is how UI and materials read a texture's alpha.
    const float4 Background    = float4(EncodeSRGB(Pass().BackgroundColor.rgb), Pass().BackgroundColor.a);
    const float3 Premultiplied = (Alpha > 0.0 ? Color : float3(0.0, 0.0, 0.0)) + (1.0 - Alpha) * Background.rgb * Background.a;
    const float  Coverage      = Alpha + (1.0 - Alpha) * Background.a;
    Pass().Target[P] = float4(Coverage > 0.0 ? saturate(Premultiplied / Coverage) : float3(0.0, 0.0, 0.0), Coverage);
}
