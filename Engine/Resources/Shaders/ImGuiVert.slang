#include "Includes/ImGuiCommon.slang"

[shader("vertex")]
FImGuiInterpolants VertexMain(uint VertexID : SV_VulkanVertexID, uint DrawIndex : SV_VulkanInstanceID)
{
    FImGuiArgs* Args = GetArgs<FImGuiArgs>();

    ImDrawVert V = ((ImDrawVert*)Args.VertexAddr)[VertexID];
    FImGuiDraw Draw = GetDraw(Args, DrawIndex);
    float2 Pixel = (V.Position - Args.ClipOffset) * Args.ClipScale;

    FImGuiInterpolants Output;
    Output.Clip[0]   = Pixel.x - Draw.ClipRect.x;
    Output.Clip[1]   = Draw.ClipRect.z - Pixel.x;
    Output.Clip[2]   = Pixel.y - Draw.ClipRect.y;
    Output.Clip[3]   = Draw.ClipRect.w - Pixel.y;
    Output.Position  = float4(V.Position * Args.Scale + Args.Translate, 0.0, 1.0);
    Output.Color     = UnpackColor(V.Color);
    Output.UV        = V.UV;
    Output.DrawIndex = DrawIndex;
    return Output;
}
