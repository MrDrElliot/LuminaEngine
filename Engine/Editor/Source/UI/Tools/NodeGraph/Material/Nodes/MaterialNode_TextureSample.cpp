#include "MaterialNode_TextureSample.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Assets/AssetTypes/Textures/TextureArray.h"
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
        // CTextureArray derives from CTexture, so the asset picker on this node accepts one. Sampling it
        // would emit SampleTexture2D against a heap slot holding a VIEW_TYPE_2D_ARRAY view -- a descriptor
        // type mismatch that resolves to the null slot, giving a purple material with no other diagnostic.
        // Caught here so it reads as a graph error naming the fix instead.
        if (Texture.IsValid() && Texture->IsA<CTextureArray>())
        {
            // Declared even though an error aborts the compile before any shader is built: downstream
            // nodes bind to this node's variable by name, so leaving it undeclared would turn one clear
            // graph error into a cascade of undefined identifiers if that ordering ever changes.
            Compiler.AddRaw("float4 " + FullName + " = float4(0.0, 0.0, 0.0, 1.0);\n");

            EdNodeGraph::FError Error;
            Error.Node        = this;
            Error.Name        = "Texture Sample";
            Error.Description = "TextureSample cannot sample a Texture Array. Use a TextureSampleArray node "
                                "instead, which takes a Slice input to pick the layer.";
            Compiler.AddError(Error);
            return;
        }

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
        // An array assigned here is a user error the compiler rejects (see GenerateDefinition), and
        // drawing it through the plain Texture2D path would sample the null slot anyway. Leave the body
        // empty so the node reads as "nothing valid assigned" rather than showing a purple square.
        if (Texture.IsValid() && Texture->GetResourceID() >= 0 && !Texture->IsA<CTextureArray>())
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
