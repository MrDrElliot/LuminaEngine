#include "MaterialNode_TextureSampleArray.h"

#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"

#include "MaterialNodePinHelpers.h"

namespace Lumina
{
    void CMaterialExpression_TextureSampleArray::BuildNode()
    {
        Super::BuildNode();

        UV    = MakeIn(this, "UV", EMaterialInputType::Float2);
        Slice = MakeIn(this, "Slice");

        Output->SetInputType(EMaterialInputType::Float4);
    }

    void CMaterialExpression_TextureSampleArray::SetNodeValue(void* Value)
    {
        TextureArray = (CTextureArray*)Value;
    }

    void CMaterialExpression_TextureSampleArray::DrawNodeBody()
    {
        if (!TextureArray.IsValid() || TextureArray->GetResourceID() < 0 || TextureArray->GetNumLayers() == 0)
        {
            return;
        }

        // Slice 0 as the thumbnail. Must go through the array display state: the heap slot holds a
        // 2D_ARRAY view, and ImGui's default path reads gTextures2D[], which would resolve to the
        // null slot rather than the image.
        ImDrawList* DrawList = ImGui::GetWindowDrawList();
        ImGuiX::BeginArrayPreview(DrawList, 0);
        ImGui::Image(ImGuiX::ToImTextureRef((uint32)TextureArray->GetResourceID()), ImVec2(126.0f, 126.0f));
        ImGuiX::EndArrayPreview(DrawList);
    }

    void CMaterialExpression_TextureSampleArray::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        int32 TextureIndex = -1;
        if (TextureArray.IsValid() && TextureArray->GetResourceID() >= 0)
        {
            TextureIndex = bDynamic && !ParameterName.IsNone()
                         ? Compiler.BindTextureParameter(ParameterName, TextureArray)
                         : Compiler.BindTexture(TextureArray);
        }

        const uint32 NumLayers = TextureArray.IsValid() ? TextureArray->GetNumLayers() : 0u;
        Compiler.TextureSampleArray(this, TextureIndex, NumLayers, UV, Slice);
    }
}
