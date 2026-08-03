#include "MaterialOutputNode.h"


#include "UI/Tools/NodeGraph/Material/MaterialInput.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialNodeGraph.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Core/Object/Cast.h"

namespace Lumina
{
    FStringView CMaterialOutputNode::GetNodeDisplayName() const
    {
        return "Material Output";
    }
    
    FStringView CMaterialOutputNode::GetNodeTooltip() const
    {
        return "The final output to the shader";
    }


    void CMaterialOutputNode::DrawNodeTitleBar()
    {
        // Cheap per-draw refresh; tracks MaterialType changes without a separate hook.
        EMaterialType MaterialType = EMaterialType::PBR;
        if (CMaterialNodeGraph* Graph = Cast<CMaterialNodeGraph>(GetOwningGraph()))
        {
            if (CMaterial* OwningMaterial = Graph->GetMaterial())
            {
                MaterialType = OwningMaterial->GetMaterialType();
            }
        }

        const bool bPostProcess = MaterialType == EMaterialType::PostProcess;
        const bool bUI          = MaterialType == EMaterialType::UI;
        const bool bDecal       = MaterialType == EMaterialType::Decal;

        // PostProcess/UI: surface attributes inert, WPO meaningless; UI keeps Opacity (brush alpha), PostProcess does not.
        const bool bFullscreen = bPostProcess || bUI;
        // Decal writes BaseColor/Normal/Roughness/Metallic/AO into the DBuffer; Opacity is its coverage.
        // No DBuffer slot for Specular, and WPO/Emissive don't apply to a projected decal (v1).
        if (BaseColorPin)            BaseColorPin->SetDisabled(bFullscreen);
        if (MetallicPin)             MetallicPin->SetDisabled(bFullscreen);
        if (RoughnessPin)            RoughnessPin->SetDisabled(bFullscreen);
        if (SpecularPin)             SpecularPin->SetDisabled(bFullscreen || bDecal);
        if (AOPin)                   AOPin->SetDisabled(bFullscreen);
        if (NormalPin)               NormalPin->SetDisabled(bFullscreen);
        if (OpacityPin)              OpacityPin->SetDisabled(bPostProcess);
        // Self-shadowing modulates the sun's direct contribution, which only surface materials receive.
        if (SelfShadowPin)           SelfShadowPin->SetDisabled(bFullscreen || bDecal);
        if (WorldPositionOffsetPin)  WorldPositionOffsetPin->SetDisabled(bFullscreen || bDecal);

        // Emissive is the fullscreen output color (PostProcess/UI); decals have no emissive DBuffer slot in v1.
        if (EmissivePin)             EmissivePin->SetDisabled(bDecal);

        Super::DrawNodeTitleBar();
    }

    void CMaterialOutputNode::BuildNode()
    {
        // Base Color (Albedo)
        BaseColorPin = CreatePin(CMaterialInput::StaticClass(), "Base Color (RGBA)", ENodePinDirection::Input);
        BaseColorPin->SetPinName("Base Color (RGBA)");
    
        // Metallic (Determines if the material is metal or non-metal)
        MetallicPin = CreatePin(CMaterialInput::StaticClass(), "Metallic", ENodePinDirection::Input);
        MetallicPin->SetPinName("Metallic");
        
        // Roughness (Controls how smooth or rough the surface is)
        RoughnessPin = CreatePin(CMaterialInput::StaticClass(), "Roughness", ENodePinDirection::Input);
        RoughnessPin->SetPinName("Roughness");

        // Specular (Affects intensity of reflections for non-metals)
        SpecularPin = CreatePin(CMaterialInput::StaticClass(), "Specular", ENodePinDirection::Input);
        SpecularPin->SetPinName("Specular");

        // Emissive (Self-illumination, for glowing objects)
        EmissivePin = CreatePin(CMaterialInput::StaticClass(), "Emissive", ENodePinDirection::Input);
        EmissivePin->SetPinName("Emissive (RGB)");

        // Ambient Occlusion (Shadows in crevices to add realism)
        AOPin = CreatePin(CMaterialInput::StaticClass(), "Ambient Occlusion", ENodePinDirection::Input);
        AOPin->SetPinName("Ambient Occlusion");

        // Normal Map (For surface detail)
        NormalPin = CreatePin(CMaterialInput::StaticClass(), "Normal Map (XYZ)", ENodePinDirection::Input);
        NormalPin->SetPinName("Normal Map (XYZ)");
        
        // Opacity (For transparent materials)
        OpacityPin = CreatePin(CMaterialInput::StaticClass(), "Opacity", ENodePinDirection::Input);
        OpacityPin->SetPinName("Opacity");

        // Self Shadow: height-field self-occlusion against the sun, from ParallaxOcclusionMapping's
        // Shadow output. Unconnected = 1 (unshadowed), which is exactly the pre-POM behavior.
        SelfShadowPin = CreatePin(CMaterialInput::StaticClass(), "Self Shadow", ENodePinDirection::Input);
        SelfShadowPin->SetPinName("Self Shadow");

        // World Position Offset: vertex-stage world-space displacement routed through the vertex chunk.
        // If connected, the vertex shader adds the graph emission to WorldPos before View/Projection.
        WorldPositionOffsetPin = CreatePin(CMaterialInput::StaticClass(), "World Position Offset (WPO)", ENodePinDirection::Input);
        WorldPositionOffsetPin->SetPinName("World Position Offset (XYZ)");
    }

    // Handles component widening/narrowing; shared by pixel-stage assignments and vertex-stage WPO.
    static FString EmitMaterialAssignment(const FString& MemberName, CEdNodeGraphPin* Pin, const FString& DefaultValue, int32 RequiredComponents)
    {
        FString Out = "\tMaterial." + MemberName + " = ";

        if (!Pin || !Pin->HasConnection())
        {
            return Out + DefaultValue + ";\n";
        }

        // Resolve first: a reroute emits no variable, so binding to its node name would reference an
        // identifier that was never declared. A chain that dead-ends reads as unconnected.
        CMaterialOutput* ConnectedPin = FMaterialCompiler::ResolveThroughReroutes(Pin->GetConnection<CMaterialOutput>(0));
        if (ConnectedPin == nullptr)
        {
            return Out + DefaultValue + ";\n";
        }
        // A material-function call output pin emits its own per-output local and binds it via
        // ResolvedVar; everything else reads the source node's single FullName variable.
        FString NodeName              = ConnectedPin->ResolvedVar.empty()
                                      ? ConnectedPin->GetOwningNode()->GetNodeFullName()
                                      : ConnectedPin->ResolvedVar;
        int32 ConnectedComponents     = FMaterialCompiler::GetComponentCount(ConnectedPin->GetComponentMask());
        // If mask is None (count==0) fall back to intrinsic width; avoids a float3() wrap with a wider argument.
        if (ConnectedComponents == 0)
        {
            ConnectedComponents = FMaterialCompiler::GetComponentCount(ConnectedPin->InputType);
        }
        FString Swizzle               = GetSwizzleForMask(ConnectedPin->GetComponentMask());
        if (!Swizzle.empty())
        {
            ConnectedComponents = (int32)Swizzle.length() - 1;
        }
        FString Value = NodeName + Swizzle;

        if (RequiredComponents == 1)
        {
            if (ConnectedComponents == 1) Out += Value + ";\n";
            else                          Out += Value + ".r;\n";
        }
        else if (RequiredComponents == 3)
        {
            if (ConnectedComponents == 3)      Out += Value + ";\n";
            else if (ConnectedComponents == 4) Out += (Swizzle.empty() ? Value + ".rgb;\n" : Value + ";\n");
            else if (ConnectedComponents == 2) Out += "float3(" + Value + ", 0.0);\n";
            else                               Out += "float3(" + Value + ");\n";
        }
        else
        {
            Out += Value + ";\n";
        }
        return Out;
    }

    void CMaterialOutputNode::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        FString PixelOut;
        PixelOut += "\n\n";
        // Seeded from the shared neutral surface: every field below is then overwritten explicitly, but a
        // field added to FMaterialPixelInputs without a matching pin here still starts initialized.
        PixelOut += "\tFMaterialPixelInputs Material = DefaultMaterialInputs();\n";

        // PostProcess: unconnected Emissive = passthrough (SceneColor); surface materials default to black.
        const bool bPostProcess = Compiler.GetMaterialType() == EMaterialType::PostProcess;
        const FString EmissiveDefault = bPostProcess ? FString("SceneColor.rgb") : FString("float3(0.0, 0.0, 0.0)");

        PixelOut += EmitMaterialAssignment("Diffuse",          BaseColorPin, "float3(1.0, 1.0, 1.0)", 3);
        PixelOut += EmitMaterialAssignment("Metallic",         MetallicPin,  "0.0",                    1);
        PixelOut += EmitMaterialAssignment("Roughness",        RoughnessPin, "1.0",                    1);
        PixelOut += EmitMaterialAssignment("Specular",         SpecularPin,  "0.5",                    1);
        PixelOut += EmitMaterialAssignment("Emissive",         EmissivePin,  EmissiveDefault,          3);
        PixelOut += EmitMaterialAssignment("AmbientOcclusion", AOPin,        "1.0",                    1);
        PixelOut += EmitMaterialAssignment("Normal",           NormalPin,    "float3(0.0, 0.0, 1.0)", 3);
        PixelOut += EmitMaterialAssignment("Opacity",          OpacityPin,   "1.0",                    1);
        PixelOut += EmitMaterialAssignment("SelfShadow",       SelfShadowPin, "1.0",                   1);

        // Always reconstruct Z from the decoded XY (Z = sqrt(1 - x^2 - y^2) >= 0 for a unit normal):
        // correct for BC7 and BC5 and avoids format detection, which broke BC5 maps through intermediate nodes.
        // Must match what EmitMaterialAssignment actually wrote: a reroute chain that dead-ends emits
        // the default, and decoding that would corrupt it.
        if (NormalPin->HasConnection()
            && FMaterialCompiler::ResolveThroughReroutes(NormalPin->GetConnection<CMaterialOutput>(0)) != nullptr)
        {
            PixelOut += "\tMaterial.Normal.xy = Material.Normal.xy * 2.0 - 1.0;\n";
            PixelOut += "\tMaterial.Normal.z  = sqrt(saturate(1.0 - dot(Material.Normal.xy, Material.Normal.xy)));\n";
        }

        Compiler.AddPixelOutput(PixelOut);

        // Vertex template declares FMaterialVertexInputs Material above the token; only emit assignment.
        FString VertexOut;
        VertexOut += "\n";
        VertexOut += EmitMaterialAssignment("WorldPositionOffset", WorldPositionOffsetPin, "float3(0.0, 0.0, 0.0)", 3);
        Compiler.AddVertexOutput(VertexOut);
    }
}
