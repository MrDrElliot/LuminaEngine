#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"

// Editor selection outline, composited into the final LDR target after tone mapping and SMAA.
//
// The mask is free: the Picker RT already holds a per-pixel entity id for CPU picking, and it is
// written by the VisBuffer resolve AND the billboard, widget and world-text passes -- so outlining it
// covers every one of those without a single extra draw. There is no selection geometry pass here.
//
// Membership is a bitset over the entity INDEX rather than a list to search: the Picker stores the whole
// entity handle (index | version), so the shader masks off the version and tests one bit. O(1) per pixel,
// and no cap on how much can be selected.

// Field order here is LAYOUT-driven, not readability-driven, and must stay in step with FSelectionOutlinePC
// in DefaultSceneRenderer.cpp.
//
// An args struct reached through a BDA pointer is a Block in the PhysicalStorageBuffer storage class, so it
// follows relaxed storage-buffer layout -- under which a vector may not straddle a 16-byte boundary. Slang
// gives a float4 only 4-byte alignment here, so with OutlineColor written last it landed at offset 24 and
// spanned 24..40, across the boundary at 32. spirv-val rejects that outright
// (VUID-VkShaderModuleCreateInfo-pCode-08737) and vkCreateShaderModule fails, so the pass gets no pipeline
// at all -- a layout mistake shows up as a dead feature, not a wrong-looking one.
//
// First is the one position that needs no explicit padding and stays aligned no matter what is added around
// it. It is a real 16-byte alignment rather than a formal one: pass args come from the transient allocator,
// whose blocks are kDefaultAlign (16) aligned. Any future vector member wants the same treatment -- group
// them at the top, ahead of the scalars.
struct FSelectionOutlineArgs
{
    float4 OutlineColor;           // rgb, a = opacity
    uint*  SelectionBits;          // one bit per entity slot, indexed by entity INDEX
    uint   PickerIndex;
    uint   SelectionBitWords;      // 0 = nothing selected; the pass is skipped before it gets here
    uint   EntityIndexMask;        // FEntity::IndexMask, supplied by the CPU rather than hardcoded
    float  Thickness;              // outline half-width in pixels
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
    if (Word >= Pass().SelectionBitWords)
    {
        return false;
    }

    // Indexed straight off the args: a BDA pointer must not be parked in a local (see the SPIR-V
    // pointer-validity rule) -- load through it where it is read.
    return (Pass().SelectionBits[Word] & (1u << (Index & 31u))) != 0u;
}

struct FragmentInput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

[shader("fragment")]
float4 FragmentMain(FragmentInput In) : SV_Target0
{
    const int2 Texel  = int2(In.Position.xy);
    const int2 Extent = int2(Scene().ScreenSize.xy);

    // Outline sits OUTSIDE the object: only unselected pixels are candidates. Drawing it inside would
    // eat the silhouette of small objects, and two adjacent selected objects would grow a seam between
    // them -- neither is what "highlight the outline" means.
    if (IsEntitySelected(LoadTexture2DUInt(Pass().PickerIndex, Texel)))
    {
        discard;
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // Chebyshev ring rather than a Sobel: this is a binary mask, so there is no gradient to estimate --
    // the question is only "is a selected pixel within Thickness of me", and the square ring answers it
    // with a uniform width on diagonals as well as axes.
    const int Radius = int(ceil(Pass().Thickness));

    bool bNear = false;
    for (int Y = -Radius; Y <= Radius && !bNear; ++Y)
    {
        for (int X = -Radius; X <= Radius; ++X)
        {
            const int2 Sample = clamp(Texel + int2(X, Y), int2(0, 0), Extent - int2(1, 1));
            if (IsEntitySelected(LoadTexture2DUInt(Pass().PickerIndex, Sample)))
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

    // Straight alpha; the pass blends over the tone-mapped image so the colour lands exactly as authored.
    return float4(Pass().OutlineColor.rgb, Pass().OutlineColor.a);
}
