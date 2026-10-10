#include "MaterialNode_Triplanar.h"
#include "Assets/AssetTypes/Textures/TextureArray.h"
#include "Core/Object/Cast.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

#include "MaterialNodePinHelpers.h"

namespace Lumina
{
    void CMaterialExpression_TriplanarSample::BuildNode()
    {
        // Same pin layout as TextureSample, so the masked outputs resolve against the one float4 it declares.
        CMaterialOutput* ValuePin = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "RGBA", ENodePinDirection::Output));
        ValuePin->SetShouldDrawEditor(true);
        ValuePin->SetHideDuringConnection(false);
        ValuePin->SetPinName("RGBA");
        ValuePin->SetComponentMask(EComponentMask::RGBA);
        ValuePin->SetInputType(EMaterialInputType::Texture);

        TextureHandle = MakeIn(this, "Texture", EMaterialInputType::TextureHandle);
        Position  = MakeIn(this, "Position", EMaterialInputType::Float3);
        Normal    = MakeIn(this, "Normal", EMaterialInputType::Float3);
        Tiling    = MakeIn(this, "Tiling");
        Sharpness = MakeIn(this, "Sharpness");

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

    void CMaterialExpression_TriplanarSample::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        // A wired handle wins, so the asset must not claim a texture slot it will never be sampled from.
        CTexture* AssetTexture = TextureHandle->HasConnection() ? nullptr : Texture.Get();

        // Binding an array as 2D would read the null descriptor; the compiler emits the neutral value for -1.
        int32 TextureIndex = -1;
        if (AssetTexture != nullptr && AssetTexture->GetResourceID() >= 0 && !AssetTexture->IsA<CTextureArray>())
        {
            TextureIndex = bDynamic && !ParameterName.IsNone()
                         ? Compiler.BindTextureParameter(ParameterName, AssetTexture, this)
                         : Compiler.BindTexture(AssetTexture, this);
        }

        FMaterialCompiler::FTriplanarInputs Inputs;
        Inputs.TextureHandle = TextureHandle;
        Inputs.Position      = Position;
        Inputs.Normal        = Normal;
        Inputs.Tiling        = Tiling;
        Inputs.Sharpness     = Sharpness;

        const EMaterialSampler Resolved = ResolveMaterialSampler(Sampler, AssetTexture);
        Compiler.TriplanarSample(FullName, this, TextureIndex, Inputs, MaterialSamplerToSlang(Resolved), bNormalMap);
    }

    void CMaterialExpression_TriplanarSample::SetNodeValue(void* Value)
    {
        Texture = (CTexture*)Value;
    }

    void CMaterialExpression_TriplanarSample::DrawNodeBody()
    {
        if (!TextureHandle->HasConnection() && Texture.IsValid() && Texture->GetResourceID() >= 0 && !Texture->IsA<CTextureArray>())
        {
            ImGuiX::TextureImage(Texture.Get(), ImVec2(126.0f, 126.0f));
        }
    }

    void CMaterialExpression_TriplanarSample::DrawContextMenu()
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
