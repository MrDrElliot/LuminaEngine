#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"
#include "Includes/SelectionOutlineCommon.slang"

// Masks the Picker RT, so every pass that writes entity ids gets outlined with no extra draw.

// Mirror of FSelectionOutlinePC, with vectors first since a float4 straddling 16 bytes fails spirv-val.
struct FSelectionOutlineArgs
{
    float4         OutlineColor;           // rgb, a = opacity
    GPUSpan<uint>  SelectionBits;   // one bit per entity index
    FTexture2DUInt Picker;
    uint           EntityIndexMask;        // FEntity::IndexMask, supplied by the CPU rather than hardcoded
    float          Thickness;              // outline half-width in pixels
    uint           _Pad0;
    GPUSpan<uint>  TileList;        // read by SelectionOutlineTileQuad.slang, which shares this block
    uint           _Pad1;
    uint           _Pad2;
    uint           _Pad3;
    uint           _Pad4;
};
FSelectionOutlineArgs* Pass() { return GetPassArgs<FSelectionOutlineArgs>(); }

struct FragmentInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

[shader("fragment")]
float4 FragmentMain(FragmentInput In) : SV_Target0
{
    const int2 Texel  = int2(In.Position.xy);
    const int2 Extent = int2(GetDisplaySize());

    // Only unselected pixels are candidates, so small silhouettes and adjacent selections stay intact.
    if (IsEntitySelected(Pass().Picker.Load(DisplayToPickerTexel(Texel)), Pass().SelectionBits, Pass().EntityIndexMask))
    {
        discard;
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // Chebyshev ring, since a binary mask has no gradient for Sobel to estimate.
    const int Radius = int(ceil(Pass().Thickness));

    bool bNear = false;
    for (int Y = -Radius; Y <= Radius && !bNear; ++Y)
    {
        for (int X = -Radius; X <= Radius; ++X)
        {
            const int2 Sample = clamp(Texel + int2(X, Y), int2(0, 0), Extent - int2(1, 1));
            if (IsEntitySelected(Pass().Picker.Load(DisplayToPickerTexel(Sample)), Pass().SelectionBits, Pass().EntityIndexMask))
            {
                bNear = true;
                break;
            }
        }
    }

    if (!bNear)
    {
        discard;
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // Straight alpha, blended over the tone-mapped image.
    return float4(Pass().OutlineColor.rgb, Pass().OutlineColor.a);
}
