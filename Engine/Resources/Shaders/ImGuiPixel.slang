#include "Includes/ImGuiCommon.slang"

[shader("fragment")]
float4 FragmentMain(FImGuiInterpolants Input) : SV_Target0
{
    FImGuiArgs* Args = GetArgs<FImGuiArgs>();

    // Uniform across the draw (it comes from the per-draw args block), so this is not divergent.
    float4 Texel = (Args.bIsArray != 0u)
                 ? FTexture2DArray(Args.TextureID).Sample(Args.Sampler, float3(Input.UV, (float)Args.ArraySlice))
                 : FTexture2D(Args.TextureID).Sample(Args.Sampler, Input.UV);

    if (Args.DisplayMode == IMGUI_DISPLAY_HDR)
    {
        // ACES rather than the scene's operator, so an asset previews the same whichever level is open.
        float3 Color = ApplyTonemap(Texel.rgb * max(Args.Exposure, 0.0), TONEMAPPER_ACES);
        Texel.rgb = EncodeDisplayGamma(Color);
    }
    else if (Args.DisplayMode == IMGUI_DISPLAY_LINEAR)
    {
        Texel.rgb = ImGuiEncodeSRGB(Texel.rgb * max(Args.Exposure, 0.0));
    }

    // ImGui tints are already display-encoded, so multiplying after the transform keeps a tinted HDR image matching an untinted one.
    return Input.Color * Texel;
}
