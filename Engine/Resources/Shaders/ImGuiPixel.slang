#include "Includes/ImGuiCommon.slang"

[shader("fragment")]
float4 FragmentMain(FImGuiInterpolants Input) : SV_Target0
{
    FImGuiArgs* Args = GetArgs<FImGuiArgs>();
    FImGuiDraw Draw = GetDraw(Args, Input.DrawIndex);

    // Uniform across a quad, since a quad never spans two commands.
    float4 Texel;
    if (Draw.bIsArray != 0u)
    {
        Texel = FTexture2DArray(Draw.TextureID).Sample(Args.Sampler, float3(Input.UV, (float)Draw.ArraySlice));
    }
    else
    {
        Texel = FTexture2D(Draw.TextureID).Sample(Args.Sampler, Input.UV);
    }

    if (Draw.DisplayMode == IMGUI_DISPLAY_SCENE)
    {
        return float4(Input.Color.rgb * Texel.rgb, Texel.a);
    }

    if (Draw.DisplayMode == IMGUI_DISPLAY_HDR)
    {
        // ACES rather than the scene's operator, so an asset previews the same whichever level is open.
        float3 Color = ApplyTonemap(Texel.rgb * max(Draw.Exposure, 0.0), TONEMAPPER_ACES);
        Texel.rgb = EncodeDisplayGamma(Color);
    }
    else if (Draw.DisplayMode == IMGUI_DISPLAY_LINEAR)
    {
        Texel.rgb = EncodeSRGB(Texel.rgb * max(Draw.Exposure, 0.0));
    }

    // ImGui tints are already display-encoded, so multiplying after the transform keeps a tinted HDR image matching an untinted one.
    return Input.Color * Texel;
}
