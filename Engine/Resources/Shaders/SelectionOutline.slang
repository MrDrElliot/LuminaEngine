#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"

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

bool IsEntitySelected(uint PickerValue)
{
    // 0 is the Picker's cleared/background value, and the null handle is all-ones; neither is a slot.
    if (PickerValue == 0u || PickerValue == 0xFFFFFFFFu)
    {
        return false;
    }

    const uint Index = PickerValue & Pass().EntityIndexMask;
    const uint Word  = Index >> 5u;

    // Out-of-range reads return zero, meaning not selected.
    return (Pass().SelectionBits.Get(Word) & (1u << (Index & 31u))) != 0u;
}

struct FragmentInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

// The outline runs over display pixels while the picker holds one id per render pixel.
int2 DisplayToPickerTexel(int2 DisplayTexel)
{
    const float2 Scale = float2(GetScreenSize()) / float2(max(GetDisplaySize(), uint2(1u, 1u)));
    return clamp(int2((float2(DisplayTexel) + 0.5) * Scale), int2(0, 0), int2(GetScreenSize()) - int2(1, 1));
}

[shader("fragment")]
float4 FragmentMain(FragmentInput In) : SV_Target0
{
    const int2 Texel  = int2(In.Position.xy);
    const int2 Extent = int2(GetDisplaySize());

    // Only unselected pixels are candidates, so small silhouettes and adjacent selections stay intact.
    if (IsEntitySelected(Pass().Picker.Load(DisplayToPickerTexel(Texel))))
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
            if (IsEntitySelected(Pass().Picker.Load(DisplayToPickerTexel(Sample))))
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
