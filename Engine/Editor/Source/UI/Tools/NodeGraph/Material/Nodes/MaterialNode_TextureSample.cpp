#include "MaterialNode_TextureSample.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Object/Cast.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

namespace Lumina
{
    void CMaterialExpression_TextureSample::BuildNode()
    {
        CMaterialOutput* ValuePin = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "RGBA", ENodePinDirection::Output));
        ValuePin->SetShouldDrawEditor(true);
        ValuePin->SetHideDuringConnection(false);
        ValuePin->SetPinName("RGBA");
        ValuePin->SetComponentMask(EComponentMask::RGBA);
        ValuePin->SetInputType(EMaterialInputType::Texture);

        UV = Cast<CMaterialInput>(CreatePin(CMaterialInput::StaticClass(), "UV", ENodePinDirection::Input));
        UV->SetPinColor(IM_COL32(255, 10, 10, 255));
        UV->SetHideDuringConnection(false);
        UV->SetPinName("UV");
        
        CMaterialOutput* R = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "R", ENodePinDirection::Output));
        R->SetPinColor(IM_COL32(255, 10, 10, 255));
        R->SetHideDuringConnection(false);
        R->SetPinName("R");
        R->SetComponentMask(EComponentMask::R);
        
        CMaterialOutput* G = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "G", ENodePinDirection::Output));
        G->SetPinColor(IM_COL32(10, 255, 10, 255));
        G->SetHideDuringConnection(false);
        G->SetPinName("G");
        G->SetComponentMask(EComponentMask::G);
        
        CMaterialOutput* B = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "B", ENodePinDirection::Output));
        B->SetPinColor(IM_COL32(10, 10, 255, 255));
        B->SetHideDuringConnection(false);
        B->SetPinName("B");
        B->SetComponentMask(EComponentMask::B);
        
        CMaterialOutput* A = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "A", ENodePinDirection::Output));
        A->SetHideDuringConnection(false);
        A->SetPinName("A");
        A->SetComponentMask(EComponentMask::A);
    }

    void CMaterialExpression_TextureSample::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        if (bDynamic && !ParameterName.IsNone())
        {
            Compiler.TextureSampleParameter(FullName, ParameterName, Texture, UV);
        }
        else
        {
            Compiler.TextureSample(FullName, Texture, UV);
        }
    }

    void CMaterialExpression_TextureSample::SetNodeValue(void* Value)
    {
        Texture = (CTexture*)Value;
    }

    void CMaterialExpression_TextureSample::DrawNodeBody()
    {
        if (Texture.IsValid() && Texture->GetResourceID() >= 0)
        {
            ImGui::Image(ImGuiX::ToImTextureRef((uint32)Texture->GetResourceID()), ImVec2(126.0f, 126.f));
        }
    }

    void CMaterialExpression_TextureSample::DrawContextMenu()
    {
        const char* MenuItem = bDynamic ? "Make Static Texture" : "Make Texture Parameter";
        if (ImGui::MenuItem(MenuItem))
        {
            bDynamic = !bDynamic;
            if (bDynamic && ParameterName.IsNone())
            {
                ParameterName = "TextureParam";
            }
        }
    }
}
