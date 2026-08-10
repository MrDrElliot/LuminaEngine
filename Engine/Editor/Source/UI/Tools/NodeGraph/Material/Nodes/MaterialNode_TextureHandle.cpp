#include "MaterialNode_TextureHandle.h"

#include "Assets/AssetTypes/Textures/TextureArray.h"
#include "Core/Object/Cast.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"

namespace Lumina
{
    void CMaterialExpression_TextureHandle::BuildNode()
    {
        Super::BuildNode();

        // A bindless index, so no component editor and no mask: nothing may ever swizzle a scalar uint.
        Output->SetPinName("Handle");
        Output->SetShouldDrawEditor(false);
        Output->SetHideDuringConnection(false);
        Output->SetInputType(EMaterialInputType::TextureHandle);
        Output->SetComponentMask(EComponentMask::None);
        Output->SetPinColor(IM_COL32(235, 185, 70, 255));
    }

    void CMaterialExpression_TextureHandle::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        // The variable is declared on every path, including the error one: downstream nodes bind to it by
        // name, so bailing without it turns one clear error into a cascade of undefined identifiers.
        if (!Texture.IsValid() || Texture->GetResourceID() < 0)
        {
            Compiler.AddRaw("uint " + FullName + " = " + ZeroLiteral(EMaterialInputType::TextureHandle) + ";\n");

            EdNodeGraph::FError Error;
            Error.Node        = this;
            Error.Name        = "Texture Handle";
            Error.Description = "TextureHandle has no texture assigned, so there is no slot to resolve. Assign "
                                "one in the node's details panel. Unlike a sample, a handle has no neutral "
                                "fallback -- index 0 is simply whichever texture happens to occupy slot 0.";
            Compiler.AddError(Error);
            return;
        }

        // Same binding path a sample takes, and deduped against it, so a texture used by both a
        // TextureSample and a TextureHandle still occupies one slot.
        const int32 Index = (bDynamic && !ParameterName.IsNone())
                          ? Compiler.BindTextureParameter(ParameterName, Texture)
                          : Compiler.BindTexture(Texture);

        // The slot index is the compile-time constant; the descriptor ID it holds is read at runtime,
        // which is what lets a parameterised handle be re-pointed per instance without a recompile.
        Compiler.AddRaw("uint " + FullName + " = GetMaterialTexture(MaterialIndex, " + eastl::to_string(Index) + ");\n");

        // Uniform across the surface, hence a zero screen-space derivative. Nothing consumes this today
        // (a uint cannot reach a UV pin), but recording it keeps the handle out of the Unknown bucket.
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

        // An array is a perfectly valid thing to take a handle to, but ImGui draws through gTextures2D
        // and would read the array's descriptor as a 2D one -- a purple square that looks like an error.
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
