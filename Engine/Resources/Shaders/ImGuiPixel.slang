#include "Includes/ImGuiCommon.slang"

[shader("fragment")]
float4 FragmentMain(FImGuiInterpolants Input) : SV_Target0
{
    FImGuiArgs* Args = GetArgs<FImGuiArgs>();

    // Uniform across the draw (it comes from the per-draw args block), so this is not divergent.
    float4 Texel = (Args.bIsArray != 0u)
                 ? SampleTexture2DArray(Args.TextureID, Args.SamplerIndex, Input.UV, Args.ArraySlice)
                 : SampleTexture2D(Args.TextureID, Args.SamplerIndex, Input.UV);

    if (Args.DisplayMode == IMGUI_DISPLAY_HDR)
    {
        // ACES specifically, not the scene's configured operator: the preview's job is to show the
        // texture, and a per-world grading choice would make the same asset look different depending
        // on which level happened to be open.
        float3 Color = ApplyTonemap(Texel.rgb * max(Args.Exposure, 0.0), TONEMAPPER_ACES);
        Texel.rgb = EncodeDisplayGamma(Color);
    }

    // Vertex color stays a straight multiply: ImGui tints are already display-encoded, so applying
    // them after the transform is what keeps a tinted HDR image matching an untinted one.
    return Input.Color * Texel;
}
