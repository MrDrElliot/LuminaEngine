#include "Includes/ImGuiCommon.slang"

[shader("vertex")]
FImGuiInterpolants VertexMain(uint VertexID : SV_VulkanVertexID)
{
    FImGuiArgs* Args = GetArgs<FImGuiArgs>();

    ImDrawVert V = ((ImDrawVert*)Args.VertexAddr)[VertexID];

    FImGuiInterpolants Output;
    Output.Position = float4(V.Position * Args.Scale + Args.Translate, 0.0, 1.0);
    Output.Color    = UnpackColor(V.Color);
    Output.UV       = V.UV;
    return Output;
}
