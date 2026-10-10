#include "Includes/SceneGlobals.slang"
#include "Includes/GlobalRHI.slang"

// Laid out like FSelectionOutlineArgs in SelectionOutline.slang, since both stages read the one args block.
struct FSelectionOutlineQuadArgs
{
    float4        OutlineColor;
    GPUSpan<uint> SelectionBits;
    uint          PickerIndex;
    uint          EntityIndexMask;
    float         Thickness;
    uint          _Pad0;
    GPUSpan<uint> TileList;
    uint          _Pad1;
    uint          _Pad2;
    uint          _Pad3;
    uint          _Pad4;
};
FSelectionOutlineQuadArgs* Pass() { return GetPassArgs<FSelectionOutlineQuadArgs>(); }

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

// One instance per listed tile, covering exactly its 8x8 pixels so each fragment runs the full-screen pass's own test.
[shader("vertex")]
VSOutput VertexMain(uint VertexIndex : SV_VertexID, uint InstanceIndex : SV_InstanceID)
{
    const uint   Packed = Pass().TileList.Get(InstanceIndex);
    const float2 Origin = float2(float(Packed & 0xFFFFu), float(Packed >> 16u)) * 8.0;

    const uint2  Corners[6] = { uint2(0u, 0u), uint2(1u, 0u), uint2(0u, 1u), uint2(1u, 0u), uint2(1u, 1u), uint2(0u, 1u) };
    const float2 Screen     = float2(GetDisplaySize());
    const float2 Pixel      = min(Origin + float2(Corners[VertexIndex % 6u]) * 8.0, Screen);

    VSOutput Output;
    Output.UV       = Pixel / Screen;
    Output.Position = float4(Output.UV * 2.0 - 1.0, 1.0, 1.0);
    return Output;
}
