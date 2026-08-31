#include "MaterialNode_Parallax.h"

#include "Core/Object/Cast.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"

#include "MaterialNodePinHelpers.h"

namespace Lumina
{
    void CMaterialExpression_ParallaxOcclusionMapping::BuildNode()
    {
        // The base creates one unnamed Output, while this node publishes three named ones instead.
        UV             = MakeIn(this, "UV", EMaterialInputType::Float2);
        HeightScale    = MakeIn(this, "Height Scale");
        MinSamples     = MakeIn(this, "Min Samples");
        MaxSamples     = MakeIn(this, "Max Samples");
        LODThreshold   = MakeIn(this, "LOD Threshold");
        ShadowSamples  = MakeIn(this, "Shadow Samples");
        ShadowSoftness = MakeIn(this, "Shadow Softness");

        UVOut = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "UV", ENodePinDirection::Output));
        UVOut->SetPinName("UV");
        UVOut->SetShouldDrawEditor(true);
        UVOut->SetHideDuringConnection(false);
        UVOut->SetInputType(EMaterialInputType::Float2);
        UVOut->SetComponentMask(EComponentMask::None);

        ShadowOut = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "Shadow", ENodePinDirection::Output));
        ShadowOut->SetPinName("Shadow");
        ShadowOut->SetShouldDrawEditor(true);
        ShadowOut->SetHideDuringConnection(false);
        ShadowOut->SetInputType(EMaterialInputType::Float);
        ShadowOut->SetComponentMask(EComponentMask::None);

        HeightOut = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "Height", ENodePinDirection::Output));
        HeightOut->SetPinName("Height");
        HeightOut->SetShouldDrawEditor(true);
        HeightOut->SetHideDuringConnection(false);
        HeightOut->SetInputType(EMaterialInputType::Float);
        HeightOut->SetComponentMask(EComponentMask::None);
    }

    void CMaterialExpression_ParallaxOcclusionMapping::SetNodeValue(void* Value)
    {
        HeightMap = (CTexture*)Value;
    }

    void CMaterialExpression_ParallaxOcclusionMapping::DrawNodeBody()
    {
        if (HeightMap.IsValid() && HeightMap->GetResourceID() >= 0)
        {
            ImGui::Image(ImGuiX::ToImTextureRef((uint32)HeightMap->GetResourceID()), ImVec2(126.0f, 126.0f));
        }
    }

    void CMaterialExpression_ParallaxOcclusionMapping::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        int32 TextureIndex = -1;
        if (HeightMap.IsValid() && HeightMap->GetResourceID() >= 0)
        {
            TextureIndex = bDynamic && !ParameterName.IsNone()
                         ? Compiler.BindTextureParameter(ParameterName, HeightMap, this)
                         : Compiler.BindTexture(HeightMap, this);
        }

        FMaterialCompiler::FParallaxInputs Inputs;
        Inputs.UV             = UV;
        Inputs.HeightScale    = HeightScale;
        Inputs.MinSamples     = MinSamples;
        Inputs.MaxSamples     = MaxSamples;
        Inputs.LODThreshold   = LODThreshold;
        Inputs.ShadowSamples  = ShadowSamples;
        Inputs.ShadowSoftness = ShadowSoftness;

        Compiler.ParallaxOcclusionMapping(this, TextureIndex, Inputs, UVOut, ShadowOut, HeightOut);
    }
}
