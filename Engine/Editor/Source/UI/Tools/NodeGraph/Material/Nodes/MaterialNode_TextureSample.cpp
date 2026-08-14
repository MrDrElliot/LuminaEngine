#include "MaterialNode_TextureSample.h"
#include "Renderer/RHICore.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Assets/AssetTypes/Textures/TextureArray.h"
#include "Core/Object/Cast.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

namespace Lumina
{
    // Emitted as a SAMPLER_* literal indexing the same stock table the RHI creates, so all three
    // orderings must agree. GlobalRHI.slang is the third copy and has no compile-time link to these.
    static_assert((uint32)EMaterialSampler::LinearWrap   == (uint32)RHI::EStockSampler::LinearWrap);
    static_assert((uint32)EMaterialSampler::LinearClamp  == (uint32)RHI::EStockSampler::LinearClamp);
    static_assert((uint32)EMaterialSampler::LinearMirror == (uint32)RHI::EStockSampler::LinearMirror);
    static_assert((uint32)EMaterialSampler::PointWrap    == (uint32)RHI::EStockSampler::PointWrap);
    static_assert((uint32)EMaterialSampler::PointClamp   == (uint32)RHI::EStockSampler::PointClamp);
    static_assert((uint32)EMaterialSampler::AnisoWrap    == (uint32)RHI::EStockSampler::AnisoWrap);
    static_assert((uint32)EMaterialSampler::AnisoClamp   == (uint32)RHI::EStockSampler::AnisoClamp);

    FStringView MaterialSamplerToSlang(EMaterialSampler Sampler)
    {
        switch (Sampler)
        {
        case EMaterialSampler::LinearClamp:  return "SAMPLER_LINEAR_CLAMP";
        case EMaterialSampler::LinearMirror: return "SAMPLER_LINEAR_MIRROR";
        case EMaterialSampler::PointWrap:    return "SAMPLER_POINT_WRAP";
        case EMaterialSampler::PointClamp:   return "SAMPLER_POINT_CLAMP";
        case EMaterialSampler::AnisoWrap:    return "SAMPLER_ANISO_WRAP";
        case EMaterialSampler::AnisoClamp:   return "SAMPLER_ANISO_CLAMP";
        case EMaterialSampler::LinearWrap:
        default:                             return "SAMPLER_LINEAR_WRAP";
        }
    }

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
        // CTextureArray derives from CTexture, so the picker accepts one. Sampling it emits SampleTexture2D
        // against a 2D_ARRAY view -- a descriptor mismatch that resolves to the null slot, purple and mute.
        if (Texture.IsValid() && Texture->IsA<CTextureArray>())
        {
            // Declared even though the error aborts the compile: downstream nodes bind to this variable by
            // name, so leaving it undeclared turns one clear error into a cascade of undefined identifiers.
            Compiler.AddRaw("float4 " + FullName + " = float4(0.0, 0.0, 0.0, 1.0);\n");

            EdNodeGraph::FError NodeError;
            NodeError.Node        = this;
            NodeError.Name        = "Texture Sample";
            NodeError.Description = "TextureSample cannot sample a Texture Array. Use a TextureSampleArray node "
                                "instead, which takes a Slice input to pick the layer.";
            Compiler.AddError(NodeError);
            return;
        }

        if (bDynamic && !ParameterName.IsNone())
        {
            Compiler.TextureSampleParameter(FullName, ParameterName, Texture, UV, this, MaterialSamplerToSlang(Sampler));
        }
        else
        {
            Compiler.TextureSample(FullName, Texture, UV, this, MaterialSamplerToSlang(Sampler));
        }
    }

    void CMaterialExpression_TextureSample::SetNodeValue(void* Value)
    {
        Texture = (CTexture*)Value;
    }

    void CMaterialExpression_TextureSample::DrawNodeBody()
    {
        // An array assigned here is a user error the compiler rejects, and the plain Texture2D path would
        // sample the null slot anyway. An empty body reads as nothing-valid-assigned, not a purple square.
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
