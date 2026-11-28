#include "MaterialOutputNode.h"


#include "UI/Tools/NodeGraph/Material/MaterialInput.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"
#include "UI/Tools/NodeGraph/Material/MaterialNodeGraph.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Containers/StringFormat.h"
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
        const bool bParticle    = MaterialType == EMaterialType::Particle;
        const bool bLightFunction = MaterialType == EMaterialType::LightFunction;

        // UI keeps Opacity as its brush alpha, while PostProcess does not.
        const bool bFullscreen = bPostProcess || bUI || bLightFunction;
        // Sprites are never lit, so Emissive is their whole color and the surface pins have no consumer.
        const bool bUnshaded = bFullscreen || bParticle;
        // There is no DBuffer slot for Specular, and WPO and Emissive do not apply to a projected decal.
        if (BaseColorPin)            BaseColorPin->SetDisabled(bUnshaded);
        if (MetallicPin)             MetallicPin->SetDisabled(bUnshaded);
        if (RoughnessPin)            RoughnessPin->SetDisabled(bUnshaded);
        if (SpecularPin)             SpecularPin->SetDisabled(bUnshaded || bDecal);
        if (AOPin)                   AOPin->SetDisabled(bUnshaded);
        if (NormalPin)               NormalPin->SetDisabled(bUnshaded);
        if (OpacityPin)              OpacityPin->SetDisabled(bPostProcess || bLightFunction);
        // Self-shadowing modulates the sun's direct contribution, which only surface materials receive.
        if (SelfShadowPin)           SelfShadowPin->SetDisabled(bUnshaded || bDecal);
        // Clearcoat also needs the Shading Model set, so an enabled pin is necessary but not sufficient.
        if (ClearcoatPin)            ClearcoatPin->SetDisabled(bUnshaded || bDecal);
        if (ClearcoatRoughnessPin)   ClearcoatRoughnessPin->SetDisabled(bUnshaded || bDecal);
        if (TransmissionPin)         TransmissionPin->SetDisabled(bUnshaded || bDecal);
        if (ThicknessPin)            ThicknessPin->SetDisabled(bUnshaded || bDecal);
        // Lobes the lighting evaluates, which a sprite never receives and a projected decal has no slot for.
        for (CEdNodeGraphPin* LobePin : { SubsurfaceColorPin, SubsurfaceRadiusPin, AnisotropyPin, AnisotropyRotationPin,
                                          SecondRoughnessPin, SecondRoughnessWeightPin, FuzzAmountPin, FuzzColorPin,
                                          FuzzRoughnessPin, ThinFilmThicknessPin, ThinFilmIORPin, BottomNormalPin })
        {
            if (LobePin) LobePin->SetDisabled(bUnshaded || bDecal);
        }
        if (SurfacePin)              SurfacePin->SetDisabled(bFullscreen);
        // A sprite corner is a real vertex, so a particle graph may still displace it.
        if (WorldPositionOffsetPin)  WorldPositionOffsetPin->SetDisabled(bFullscreen || bDecal);

        // Emissive is the fullscreen output color (PostProcess/UI); decals have no emissive DBuffer slot in v1.
        if (EmissivePin)             EmissivePin->SetDisabled(bDecal);

        Super::DrawNodeTitleBar();
    }

    void CMaterialOutputNode::BuildNode()
    {
        SurfacePin = CreatePin(CMaterialInput::StaticClass(), "Surface", ENodePinDirection::Input);
        static_cast<CMaterialInput*>(SurfacePin)->SetInputType(EMaterialInputType::Surface);

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

        // Unconnected means 1, unshadowed, which is exactly the pre-parallax behavior.
        SelfShadowPin = CreatePin(CMaterialInput::StaticClass(), "Self Shadow", ENodePinDirection::Input);
        SelfShadowPin->SetPinName("Self Shadow");

        // A clear dielectric layer over the base, and a strength of 0 is the same as not having one.
        ClearcoatPin = CreatePin(CMaterialInput::StaticClass(), "Clearcoat", ENodePinDirection::Input);
        ClearcoatPin->SetPinName("Clearcoat");

        ClearcoatRoughnessPin = CreatePin(CMaterialInput::StaticClass(), "Clearcoat Roughness", ENodePinDirection::Input);
        ClearcoatRoughnessPin->SetPinName("Clearcoat Roughness");

        TransmissionPin = CreatePin(CMaterialInput::StaticClass(), "Transmission", ENodePinDirection::Input);
        TransmissionPin->SetPinName("Transmission");

        ThicknessPin = CreatePin(CMaterialInput::StaticClass(), "Thickness", ENodePinDirection::Input);
        ThicknessPin->SetPinName("Thickness");

        // Subsurface shading model only, the color light picks up inside and how far past the shadow edge it spreads.
        SubsurfaceColorPin  = CreatePin(CMaterialInput::StaticClass(), "Subsurface Color", ENodePinDirection::Input);
        SubsurfaceRadiusPin = CreatePin(CMaterialInput::StaticClass(), "Subsurface Radius", ENodePinDirection::Input);

        // Stretches the highlight along the tangent at positive values and across it at negative ones, as on brushed metal.
        AnisotropyPin         = CreatePin(CMaterialInput::StaticClass(), "Anisotropy", ENodePinDirection::Input);
        AnisotropyRotationPin = CreatePin(CMaterialInput::StaticClass(), "Anisotropy Rotation", ENodePinDirection::Input);

        // A second, rougher specular lobe blended in by its weight, the soft halo around a highlight on hazy plastic or skin.
        SecondRoughnessPin       = CreatePin(CMaterialInput::StaticClass(), "Second Roughness", ENodePinDirection::Input);
        SecondRoughnessWeightPin = CreatePin(CMaterialInput::StaticClass(), "Second Roughness Weight", ENodePinDirection::Input);

        // A fiber layer on top of everything, the soft rim of velvet, peach skin and dust.
        FuzzAmountPin    = CreatePin(CMaterialInput::StaticClass(), "Fuzz Amount", ENodePinDirection::Input);
        FuzzColorPin     = CreatePin(CMaterialInput::StaticClass(), "Fuzz Color", ENodePinDirection::Input);
        FuzzRoughnessPin = CreatePin(CMaterialInput::StaticClass(), "Fuzz Roughness", ENodePinDirection::Input);

        // A film a fraction of a micrometer thick over the base, which shifts its reflections through the colors of soap and oil.
        ThinFilmThicknessPin = CreatePin(CMaterialInput::StaticClass(), "Thin Film Thickness", ENodePinDirection::Input);
        ThinFilmIORPin       = CreatePin(CMaterialInput::StaticClass(), "Thin Film IOR", ENodePinDirection::Input);

        // Clearcoat model only, the normal map of the base beneath the coat, such as metallic flakes or a carbon weave.
        BottomNormalPin = CreatePin(CMaterialInput::StaticClass(), "Bottom Normal", ENodePinDirection::Input);

        // When connected, the vertex shader adds the graph emission to WorldPos before view and projection.
        WorldPositionOffsetPin = CreatePin(CMaterialInput::StaticClass(), "World Position Offset (WPO)", ENodePinDirection::Input);
        WorldPositionOffsetPin->SetPinName("World Position Offset (XYZ)");

        // Declared so tooltips, pin shapes and conversion badges know what each attribute takes.
        for (CEdNodeGraphPin* VectorPin : { BaseColorPin, EmissivePin, NormalPin, TransmissionPin, SubsurfaceColorPin, FuzzColorPin, BottomNormalPin, WorldPositionOffsetPin })
        {
            static_cast<CMaterialInput*>(VectorPin)->SetInputType(EMaterialInputType::Float3);
        }
    }

    // Handles component widening/narrowing; shared by pixel-stage assignments, vertex-stage WPO and pin previews.
    static FString EmitAssignmentFrom(const FString& Target, const FString& MemberName, CMaterialOutput* ConnectedPin, const FString& DefaultValue, int32 RequiredComponents)
    {
        FString Out = "\t" + Target + "." + MemberName + " = ";
        if (ConnectedPin == nullptr)
        {
            return Out + DefaultValue + ";\n";
        }
        // A function-call output binds its own local, while everything else reads the node's FullName.
        FString NodeName              = ConnectedPin->ResolvedVar.IsEmpty()
                                      ? ConnectedPin->GetOwningNode()->GetNodeFullName()
                                      : ConnectedPin->ResolvedVar;
        int32 ConnectedComponents     = FMaterialCompiler::GetComponentCount(ConnectedPin->GetComponentMask());
        // If mask is None (count==0) fall back to intrinsic width; avoids a float3() wrap with a wider argument.
        if (ConnectedComponents == 0)
        {
            ConnectedComponents = FMaterialCompiler::GetComponentCount(ConnectedPin->InputType);
        }
        FString Swizzle               = GetSwizzleForMask(ConnectedPin->GetComponentMask());
        if (!Swizzle.IsEmpty())
        {
            ConnectedComponents = Swizzle.Len() - 1;
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
            else if (ConnectedComponents == 4) Out += (Swizzle.IsEmpty() ? Value + ".rgb;\n" : Value + ";\n");
            else if (ConnectedComponents == 2) Out += "float3(" + Value + ", 0.0);\n";
            else                               Out += "float3(" + Value + ");\n";
        }
        else
        {
            Out += Value + ";\n";
        }
        return Out;
    }

    FString CMaterialOutputNode::EmitFieldAssignment(const FString& Target, const FString& Member, CEdNodeGraphPin* Pin, const FString& Default, int32 Width)
    {
        // A reroute emits no variable, so binding to its node name would reference an undeclared identifier.
        CMaterialOutput* ConnectedPin = Pin != nullptr && Pin->HasConnection()
            ? FMaterialCompiler::ResolveThroughReroutes(Pin->GetConnection<CMaterialOutput>(0))
            : nullptr;
        return EmitAssignmentFrom(Target, Member, ConnectedPin, Default, Width);
    }

    FString CMaterialOutputNode::EmitNormalDecode(const FString& Target, const FString& Member)
    {
        const FString Field = Target + "." + Member;
        return "\t" + Field + ".xy = " + Field + ".xy * 2.0 - 1.0;\n"
             + "\t" + Field + ".z  = sqrt(saturate(1.0 - dot(" + Field + ".xy, " + Field + ".xy)));\n";
    }

    // Must match what EmitFieldAssignment wrote, or a dead-end reroute default gets corrupted.
    bool CMaterialOutputNode::IsPinDriven(CEdNodeGraphPin* Pin)
    {
        return Pin != nullptr && Pin->HasConnection() && FMaterialCompiler::ResolveThroughReroutes(Pin->GetConnection<CMaterialOutput>(0)) != nullptr;
    }

    void CMaterialOutputNode::GeneratePreviewDefinition(FMaterialCompiler& Compiler, CMaterialOutput* Pin)
    {
        // A surface previews as itself, lit, since no single channel of it says what it looks like.
        if (Pin != nullptr && Pin->InputType == EMaterialInputType::Surface)
        {
            const FString Var = Pin->ResolvedVar.IsEmpty() ? Pin->GetOwningNode()->GetNodeFullName() : Pin->ResolvedVar;
            Compiler.AddPixelOutput("\n\n\tFMaterialPixelInputs Material = " + Var + ";\n");
            Compiler.AddVertexOutput("\n\tMaterial.WorldPositionOffset = float3(0.0, 0.0, 0.0);\n");
            return;
        }
        FString PixelOut = "\n\n\tFMaterialPixelInputs Material = DefaultMaterialInputs();\n";
        PixelOut += "\tMaterial.Diffuse = float3(0.0, 0.0, 0.0);\n";
        PixelOut += "\tMaterial.Metallic = 0.0;\n";
        PixelOut += "\tMaterial.Specular = 0.0;\n";
        PixelOut += "\tMaterial.Roughness = 1.0;\n";
        PixelOut += EmitAssignmentFrom("Material", "Emissive", Pin, "float3(0.0, 0.0, 0.0)", 3);
        Compiler.AddPixelOutput(PixelOut);
        Compiler.AddVertexOutput("\n\tMaterial.WorldPositionOffset = float3(0.0, 0.0, 0.0);\n");
    }

    void CMaterialOutputNode::GetPixelStagePins(TVector<CEdNodeGraphPin*>& OutPins) const
    {
        // Adding a pixel-stage pin means adding it in BOTH places, see the header for what breaks.
        CEdNodeGraphPin* const Pins[] =
        {
            BaseColorPin, MetallicPin, RoughnessPin, SpecularPin,
            EmissivePin,  AOPin,       NormalPin,    OpacityPin,
            SelfShadowPin, ClearcoatPin, ClearcoatRoughnessPin, TransmissionPin, ThicknessPin,
            SubsurfaceColorPin, SubsurfaceRadiusPin, AnisotropyPin, AnisotropyRotationPin,
            SecondRoughnessPin, SecondRoughnessWeightPin, FuzzAmountPin, FuzzColorPin, FuzzRoughnessPin,
            ThinFilmThicknessPin, ThinFilmIORPin, BottomNormalPin, SurfacePin,
        };

        OutPins.Reserve(OutPins.Num() + std::size(Pins));
        for (CEdNodeGraphPin* Pin : Pins)
        {
            OutPins.Add(Pin);
        }
    }

    void CMaterialOutputNode::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        TVector<CEdNodeGraphPin*> FieldPins;
        GetPixelStagePins(FieldPins);

        // A connected surface replaces the defaults, and only the pins driven directly are layered over it.
        const bool bSurface = IsPinDriven(SurfacePin);
        FString PixelOut = "\n\n\tFMaterialPixelInputs Material = ";
        PixelOut += bSurface
            ? Compiler.GetTypedInputValue(static_cast<CMaterialInput*>(SurfacePin), "DefaultMaterialInputs()").Value
            : FString("DefaultMaterialInputs()");
        PixelOut += ";\n";

        // An unconnected Emissive passes SceneColor through, leaves a light unmasked, and is black on a surface.
        FString EmissiveDefault = "float3(0.0, 0.0, 0.0)";
        if (Compiler.GetMaterialType() == EMaterialType::PostProcess)
        {
            EmissiveDefault = "SceneColor.rgb";
        }
        else if (Compiler.GetMaterialType() == EMaterialType::LightFunction)
        {
            EmissiveDefault = "float3(1.0, 1.0, 1.0)";
        }

        bool bExtendedLobes = Compiler.IsExtendedSurfaceDriven();
        for (int32 i = 0; i < (int32)std::size(kMaterialSurfaceFields); ++i)
        {
            const FMaterialSurfaceField& Field = kMaterialSurfaceFields[i];
            CEdNodeGraphPin* Pin = FieldPins[i];
            if (bSurface && !IsPinDriven(Pin))
            {
                continue;
            }
            const bool bEmissive = Pin == EmissivePin;
            PixelOut += EmitFieldAssignment("Material", Field.Member, Pin, bEmissive ? EmissiveDefault : FString(Field.Default), Field.Width);
            if (Field.bNormalMap && IsPinDriven(Pin))
            {
                PixelOut += EmitNormalDecode("Material", Field.Member);
            }
            bExtendedLobes |= Field.bExtendedLobe && IsPinDriven(Pin);
        }

        // A decal leaves every channel its graph does not drive to the surface underneath.
        if (Compiler.GetMaterialType() == EMaterialType::Decal)
        {
            const bool bSurfaceChannels = bSurface || IsPinDriven(RoughnessPin) || IsPinDriven(MetallicPin) || IsPinDriven(AOPin);
            PixelOut += Lumina::Format("\n#define DECAL_CHANNEL_MASK float4({}, {}, {}, {})\n",
                bSurface || IsPinDriven(BaseColorPin) ? "1.0" : "0.0", bSurface || IsPinDriven(NormalPin) ? "1.0" : "0.0",
                bSurfaceChannels ? "1.0" : "0.0", bSurface || IsPinDriven(EmissivePin) ? "1.0" : "0.0");
        }

        Compiler.AddPixelOutput(PixelOut);

        // Vertex template declares FMaterialVertexInputs Material above the token; only emit assignment.
        FString VertexOut;
        VertexOut += "\n";
        VertexOut += EmitFieldAssignment("Material", "WorldPositionOffset", WorldPositionOffsetPin, "float3(0.0, 0.0, 0.0)", 3);
        Compiler.AddVertexOutput(VertexOut);
        Compiler.SetWorldPositionOffsetDriven(IsPinDriven(WorldPositionOffsetPin));

        // Make Surface nodes upstream have already reported the lobes they drive.
        Compiler.SetExtendedSurfaceDriven(bExtendedLobes);
    }
}
