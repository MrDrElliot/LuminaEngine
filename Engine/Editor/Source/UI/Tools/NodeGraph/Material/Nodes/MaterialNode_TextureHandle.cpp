#include "MaterialNode_TextureHandle.h"

#include "Assets/AssetTypes/Textures/TextureArray.h"
#include "Core/Object/Cast.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    void CMaterialExpression_TextureHandle::BuildNode()
    {
        Super::BuildNode();

        // A bindless index, so no component editor and no mask, since nothing may swizzle a scalar uint.
        Output->SetPinName("Handle");
        Output->SetShouldDrawEditor(false);
        Output->SetHideDuringConnection(false);
        Output->SetInputType(EMaterialInputType::TextureHandle);
        Output->SetComponentMask(EComponentMask::None);
        Output->SetPinColor(IM_COL32(235, 185, 70, 255));
    }

    void CMaterialExpression_TextureHandle::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        // Downstream nodes bind by name, so bailing without it turns one error into a cascade.
        if (!Texture.IsValid() || Texture->GetResourceID() < 0)
        {
            Compiler.AddRaw("uint " + FullName + " = " + ZeroLiteral(EMaterialInputType::TextureHandle) + ";\n");

            EdNodeGraph::FError NodeError;
            NodeError.Node        = this;
            NodeError.Name        = "Texture Handle";
            NodeError.Description = "TextureHandle has no texture assigned, so there is no slot to resolve. Assign "
                                "one in the node's details panel. Unlike a sample, a handle has no neutral "
                                "fallback -- index 0 is simply whichever texture happens to occupy slot 0.";
            Compiler.AddError(NodeError);
            return;
        }

        // Deduped against the sample path, so a texture used by both still occupies one slot.
        const int32 Index = (bDynamic && !ParameterName.IsNone())
                          ? Compiler.BindTextureParameter(ParameterName, Texture)
                          : Compiler.BindTexture(Texture);

        // The descriptor ID is read at runtime, which lets a parameterized handle be re-pointed per instance.
        Compiler.AddRaw("uint " + FullName + " = GetMaterialTexture(MaterialIndex, " + Format("{}", Index) + ");\n");

        // Nothing consumes this today, but recording it keeps the handle out of the Unknown bucket.
        Compiler.RegisterDeriv(FullName, FMaterialCompiler::EDerivState::Zero);
    }

    void CMaterialExpression_TextureHandle::SetNodeValue(void* Value)
    {
        Texture = (CTexture*)Value;
    }

    void CMaterialExpression_TextureHandle::DrawNodeBody()
    {
        if (!Texture.IsValid() || Texture->GetResourceID() < 0)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "No texture assigned");
            return;
        }

        // ImGui draws through the 2D table and would read an array descriptor as 2D, showing an error square.
        if (Texture->IsA<CTextureArray>())
        {
            ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.64f, 1.0f), "%s (array)", Texture->GetName().c_str());
            return;
        }

        ImGui::Image(ImGuiX::ToImTextureRef((uint32)Texture->GetResourceID()), ImVec2(126.0f, 126.0f));
    }

    void CMaterialExpression_TextureHandle::DrawContextMenu()
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
