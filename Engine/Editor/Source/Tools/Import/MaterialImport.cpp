#include "EditorPCH.h"
#include "MaterialImport.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/Material/MaterialInstance.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Assets/Factories/Factory.h"
#include "Assets/Factories/TextureFactory/TextureFactory.h"
#include "Core/Math/Math.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/NodeGraph/NodeGraphOps.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "UI/Tools/NodeGraph/Material/MaterialGraphCompile.h"
#include "UI/Tools/NodeGraph/Material/MaterialNodeGraph.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNodeExpression.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_Constants.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_Inputs.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_Math.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_TextureSample.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialOutputNode.h"
#include "FileSystem/FileSystem.h"
#include "Log/Log.h"
#include "Tools/Import/ImportPaths.h"
#include "Containers/StringFormat.h"

namespace Lumina
{
    namespace MaterialGraphBuilder
    {
        CMaterialNodeGraph* CreateHeadlessGraph(CMaterial* Material)
        {
            CMaterialNodeGraph* Graph = NewObject<CMaterialNodeGraph>(Material->GetPackage(), "AssetMaterialGraph");
            Graph->SetMaterial(Material);

            // Skips Initialize, which creates an ImGui context that is not safe off the main thread.
            Graph->CreateNode(CMaterialOutputNode::StaticClass());
            return Graph;
        }

        CMaterialOutputNode* GetOutputNode(CMaterialNodeGraph* Graph)
        {
            for (const TObjectPtr<CEdGraphNode>& Node : Graph->Nodes)
            {
                if (Node.IsValid() && Node->IsA<CMaterialOutputNode>())
                {
                    return static_cast<CMaterialOutputNode*>(Node.Get());
                }
            }
            return nullptr;
        }

        CEdGraphNode* AddNode(CMaterialNodeGraph* Graph, CClass* NodeClass, float X, float Y)
        {
            CEdGraphNode* Node = Graph->CreateNode(NodeClass);
            if (Node)
            {
                Node->SetGridPos(X, Y);
            }
            return Node;
        }

        void Connect(CEdNodeGraphPin* OutputPin, CEdNodeGraphPin* InputPin)
        {
            if (OutputPin == nullptr || InputPin == nullptr)
            {
                return;
            }

            CMaterialNodeGraph* Graph = OutputPin->GetOwningNode() != nullptr
                ? Cast<CMaterialNodeGraph>(OutputPin->GetOwningNode()->GetOwningGraph())
                : nullptr;

            FString Error;
            if (!NodeGraphOps::ConnectPins(Graph, OutputPin, InputPin, Error))
            {
                LOG_WARN("Material import: refused to wire {} to {}. {}",
                    OutputPin->GetPinName(), InputPin->GetPinName(), Error);
            }
        }

        void FinalizeGraph(CMaterialNodeGraph* Graph)
        {
            Graph->ValidateGraph();
        }
    }

    namespace
    {
        using namespace Import::Mesh;

        EBlendMode ToBlendMode(EImportAlphaMode Mode)
        {
            switch (Mode)
            {
            case EImportAlphaMode::Mask:  return EBlendMode::Masked;
            case EImportAlphaMode::Blend: return EBlendMode::Translucent;
            default:                      return EBlendMode::Opaque;
            }
        }

        // Parameter-name stems for the five texture slots, in EMaterialTextureSlot order.
        constexpr const char* GSlotNames[(size_t)EMaterialTextureSlot::Count] =
        {
            "BaseColor", "MetallicRoughness", "Normal", "Emissive", "Occlusion", "Metallic", "Roughness",
        };

        // Both bits are compiled in, so they key the MASTER while the values stay per-instance.
        uint32 BuildUVSignature(const FMeshImportMaterial& Src)
        {
            uint32 Signature = 0;
            for (size_t Slot = 0; Slot < (size_t)EMaterialTextureSlot::Count; ++Slot)
            {
                const FTextureUVTransform& UVT = Src.UVTransforms[Slot];
                const bool bTransformed = !UVT.IsIdentity();

                Signature |= (UVT.TexCoordSet >= 1 ? 1u : 0u) << (Slot * 2);
                Signature |= (bTransformed     ? 1u : 0u) << (Slot * 2 + 1);
            }
            return Signature;
        }

        /** EImportSampler and EMaterialSampler are declared with matching values; this is the documented cast. */
        EMaterialSampler ToMaterialSampler(EImportSampler Sampler)
        {
            return static_cast<EMaterialSampler>(Sampler);
        }

        // The sampler index is a shader constant, so it belongs in the master key.
        uint32 BuildSamplerSignature(const FMeshImportMaterial& Src)
        {
            uint32 Signature = 0;
            for (size_t Slot = 0; Slot < (size_t)EMaterialTextureSlot::Count; ++Slot)
            {
                Signature |= ((uint32)Src.Samplers[Slot] & 0x7u) << (Slot * 3);
            }
            return Signature;
        }

        /** Dielectric reflectance the engine's Specular term encodes, derived from IOR. */
        float ComputeEngineSpecular(const FMeshImportMaterial& Src)
        {
            // Schlick F0 for a dielectric against air, so IOR 1.5 gives F0 0.04 and the engine's 0.5.
            const float IOR = Math::Max(Src.IOR, 1.0f);
            const float Ratio = (IOR - 1.0f) / (IOR + 1.0f);
            const float F0 = Ratio * Ratio * Math::Max(Src.SpecularFactor, 0.0f);
            return Math::Clamp(F0 / 0.08f, 0.0f, 1.0f);
        }

        /** Feature-signature bits; each one adds nodes to the graph, so each has to key its own master. */
        enum EMaterialFeatureBits : uint32
        {
            MFB_NormalScale      = BIT(0),
            MFB_OcclusionStrength = BIT(1),
            MFB_Specular         = BIT(2),
            MFB_Clearcoat        = BIT(3),
            /** Set for every material of an import whose geometry carries a color attribute. */
            MFB_VertexColor      = BIT(4),
            /** Metalness and roughness arrive as two single-channel maps rather than one packed ORM. */
            MFB_SplitMetalRough  = BIT(5),
            // One bit per optional map, because a slot sampled against a default texture still costs a fetch on every pixel.
            MFB_BaseColorTexture = BIT(6),
            MFB_MetalRoughTexture = BIT(7),
            MFB_NormalTexture    = BIT(8),
            MFB_EmissiveTexture  = BIT(9),
            MFB_Emissive         = BIT(10),
            MFB_OcclusionTexture = BIT(11),
        };

        // A packed map always wins, so a source supplying both never builds the split chain.
        bool UsesSplitMetalRough(const FMeshImportMaterial& Src)
        {
            return Src.MetallicRoughnessImage == INDEX_NONE
                && (Src.MetallicImage != INDEX_NONE || Src.RoughnessImage != INDEX_NONE);
        }

        uint32 BuildFeatureSignature(const FMeshImportMaterial& Src, bool bHasVertexColors)
        {
            uint32 Signature = bHasVertexColors ? MFB_VertexColor : 0u;
            if (UsesSplitMetalRough(Src))      { Signature |= MFB_SplitMetalRough; }
            if (Src.BaseColorImage != INDEX_NONE)         { Signature |= MFB_BaseColorTexture; }
            if (Src.MetallicRoughnessImage != INDEX_NONE) { Signature |= MFB_MetalRoughTexture; }
            if (Src.NormalImage != INDEX_NONE)            { Signature |= MFB_NormalTexture; }
            if (Src.OcclusionImage != INDEX_NONE)         { Signature |= MFB_OcclusionTexture; }
            // glTF multiplies the emissive map by the factor, so a black factor leaves the map unread.
            const bool bEmissiveFactor = Src.EmissiveColor.x > 0.0f || Src.EmissiveColor.y > 0.0f || Src.EmissiveColor.z > 0.0f;
            if (bEmissiveFactor)                                       { Signature |= MFB_Emissive; }
            if (bEmissiveFactor && Src.EmissiveImage != INDEX_NONE)    { Signature |= MFB_EmissiveTexture; }
            // Scale and strength only shape a map, so without one they would build nodes nothing reads.
            if ((Signature & MFB_NormalTexture) != 0 && Src.NormalScale != 1.0f)          { Signature |= MFB_NormalScale; }
            if ((Signature & MFB_OcclusionTexture) != 0 && Src.OcclusionStrength != 1.0f) { Signature |= MFB_OcclusionStrength; }
            // 0.5 is the shading default, so an unauthored IOR needs no node at all.
            if (Math::Abs(ComputeEngineSpecular(Src) - 0.5f) > 0.001f) { Signature |= MFB_Specular; }
            // Must match GetMaster exactly, since unlit wins and would otherwise build unread coat nodes.
            if (Src.ClearcoatFactor > 0.0f && !Src.bUnlit) { Signature |= MFB_Clearcoat; }
            return Signature;
        }

        void BuildPBRGraph(CMaterialNodeGraph* Graph, CTexture* White, CTexture* FlatNormal, bool bNeedsOpacity,
                           uint32 UVSignature, const EImportSampler* Samplers, uint32 FeatureSignature)
        {
            using namespace MaterialGraphBuilder;

            CMaterialOutputNode* Output = GetOutputNode(Graph);
            if (Output == nullptr)
            {
                return;
            }

            // Bump these to spread the auto-generated graph further apart.
            constexpr float ColTex = 0.0f;      // texture + factor inputs
            constexpr float ColMul = 620.0f;    // multiply nodes
            constexpr float ColOut = 1320.0f;   // output node
            constexpr float VS     = 1.7f;      // vertical spacing scale

            Output->SetGridPos(ColOut, 360.0f * VS);

            // UV chains sit to the LEFT of the texture column so they read as inputs to it.
            constexpr float ColUV = -700.0f;

            // Set 0 with no transform stays unwired, and scale rides Tiling so the chain rule applies.
            auto ApplySlotUV = [&](CMaterialExpression_TextureSample* Sample, EMaterialTextureSlot Slot, float Y)
            {
                // Filtering/addressing is per slot and compiled in, so it rides the same pass as the UV set.
                Sample->Sampler = ToMaterialSampler(Samplers[(size_t)Slot]);

                const uint32 Bits         = (UVSignature >> ((size_t)Slot * 2)) & 0x3u;
                const bool   bUseSet1     = (Bits & 0x1u) != 0;
                const bool   bTransformed = (Bits & 0x2u) != 0;

                if (!bUseSet1 && !bTransformed)
                {
                    return;
                }

                const FString Stem = GSlotNames[(size_t)Slot];

                auto* Coords = AddNode<CMaterialExpression_TexCoords>(Graph, ColUV, Y);
                Coords->TextureIndex = bUseSet1 ? 1u : 0u;

                if (!bTransformed)
                {
                    Connect(Coords->GetOutputPins()[0].Get(), Sample->UV);
                    return;
                }

                auto* ScaleParam = AddNode<CMaterialExpression_ConstantFloat2>(Graph, ColUV - 320.0f, Y);
                ScaleParam->bDynamic      = true;
                ScaleParam->ParameterName = FName((Stem + "UVScale").c_str());
                ScaleParam->Value         = FVector4(1.0f, 1.0f, 0.0f, 0.0f);
                Connect(ScaleParam->GetOutputPins()[0].Get(), Coords->Tiling);

                auto* RotationParam = AddNode<CMaterialExpression_ConstantFloat>(Graph, ColUV - 320.0f, Y + 95.0f);
                RotationParam->bDynamic      = true;
                RotationParam->ParameterName = FName((Stem + "UVRotation").c_str());
                RotationParam->Value         = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
                Connect(RotationParam->GetOutputPins()[0].Get(), Coords->Rotation);

                auto* OffsetParam = AddNode<CMaterialExpression_ConstantFloat2>(Graph, ColUV - 320.0f, Y + 190.0f);
                OffsetParam->bDynamic      = true;
                OffsetParam->ParameterName = FName((Stem + "UVOffset").c_str());
                OffsetParam->Value         = FVector4(0.0f, 0.0f, 0.0f, 0.0f);

                auto* Add = AddNode<CMaterialExpression_Addition>(Graph, ColUV + 320.0f, Y);
                Connect(Coords->GetOutputPins()[0].Get(), Add->A);
                Connect(OffsetParam->GetOutputPins()[0].Get(), Add->B);
                Connect(Add->Output, Sample->UV);
            };

            // Base color = BaseColorTexture.rgb * BaseColorFactor.rgb, or the factor alone without a map.
            auto* FacBase = AddNode<CMaterialExpression_ConstantFloat4>(Graph, ColTex, 190.0f * VS);
            FacBase->bDynamic = true;
            FacBase->ParameterName = "BaseColorFactor";
            FacBase->Value = FVector4(1.0f, 1.0f, 1.0f, 1.0f);

            CMaterialExpression_TextureSample* TexBase = nullptr;
            CEdNodeGraphPin* BaseColorSource = FacBase->GetOutputPins()[0].Get();
            if ((FeatureSignature & MFB_BaseColorTexture) != 0)
            {
                TexBase = AddNode<CMaterialExpression_TextureSample>(Graph, ColTex, 0.0f * VS);
                TexBase->bDynamic = true;
                TexBase->ParameterName = "BaseColorTexture";
                TexBase->Texture = White;
                ApplySlotUV(TexBase, EMaterialTextureSlot::BaseColor, 0.0f * VS);

                auto* MulBase = AddNode<CMaterialExpression_Multiplication>(Graph, ColMul, 60.0f * VS);
                Connect(TexBase->GetOutputPins()[0].Get(), MulBase->A);   // RGBA
                Connect(FacBase->GetOutputPins()[0].Get(), MulBase->B);
                BaseColorSource = MulBase->Output;
            }

            if ((FeatureSignature & MFB_VertexColor) == 0)
            {
                Connect(BaseColorSource, Output->BaseColorPin);
            }
            else
            {
                // 285 meshes sharing 7 white-factored materials is a vertex-colored scene that would render white.
                auto* VertColor = AddNode<CMaterialExpression_VertexColor>(Graph, ColTex, 250.0f * VS);

                auto* MulVertex = AddNode<CMaterialExpression_Multiplication>(Graph, ColMul + 160.0f, 60.0f * VS);
                Connect(BaseColorSource, MulVertex->A);
                Connect(VertColor->GetOutputPins()[0].Get(), MulVertex->B);
                Connect(MulVertex->Output, Output->BaseColorPin);
            }

            // glTF packs both into one image while FBX authors a separate single-channel map per channel.
            const bool bSplitMetalRough = (FeatureSignature & MFB_SplitMetalRough) != 0;

            auto* FacMetal = AddNode<CMaterialExpression_ConstantFloat>(Graph, ColTex, 570.0f * VS);
            FacMetal->bDynamic = true;
            FacMetal->ParameterName = "MetallicFactor";
            FacMetal->Value = FVector4(1.0f, 0.0f, 0.0f, 0.0f);

            auto* FacRough = AddNode<CMaterialExpression_ConstantFloat>(Graph, ColTex, 650.0f * VS);
            FacRough->bDynamic = true;
            FacRough->ParameterName = "RoughnessFactor";
            FacRough->Value = FVector4(1.0f, 0.0f, 0.0f, 0.0f);

            CEdNodeGraphPin* MetalSource = nullptr;
            CEdNodeGraphPin* RoughSource = nullptr;

            if (!bSplitMetalRough && (FeatureSignature & MFB_MetalRoughTexture) == 0)
            {
                Connect(FacMetal->GetOutputPins()[0].Get(), Output->MetallicPin);
                Connect(FacRough->GetOutputPins()[0].Get(), Output->RoughnessPin);
            }
            else if (!bSplitMetalRough)
            {
                auto* TexMR = AddNode<CMaterialExpression_TextureSample>(Graph, ColTex, 380.0f * VS);
                TexMR->bDynamic = true;
                TexMR->ParameterName = "MetallicRoughnessTexture";
                TexMR->Texture = White;
                ApplySlotUV(TexMR, EMaterialTextureSlot::MetallicRoughness, 380.0f * VS);

                MetalSource = TexMR->GetOutputPins()[3].Get();   // B channel
                RoughSource = TexMR->GetOutputPins()[2].Get();   // G channel
            }
            else
            {
                auto* TexMetal = AddNode<CMaterialExpression_TextureSample>(Graph, ColTex, 380.0f * VS);
                TexMetal->bDynamic = true;
                TexMetal->ParameterName = "MetallicTexture";
                TexMetal->Texture = White;
                ApplySlotUV(TexMetal, EMaterialTextureSlot::Metallic, 380.0f * VS);

                auto* TexRough = AddNode<CMaterialExpression_TextureSample>(Graph, ColTex, 490.0f * VS);
                TexRough->bDynamic = true;
                TexRough->ParameterName = "RoughnessTexture";
                TexRough->Texture = White;
                ApplySlotUV(TexRough, EMaterialTextureSlot::Roughness, 490.0f * VS);

                MetalSource = TexMetal->GetOutputPins()[1].Get();   // R channel
                RoughSource = TexRough->GetOutputPins()[1].Get();   // R channel
            }

            if (MetalSource != nullptr)
            {
                auto* MulMetal = AddNode<CMaterialExpression_Multiplication>(Graph, ColMul, 380.0f * VS);
                Connect(MetalSource, MulMetal->A);
                Connect(FacMetal->GetOutputPins()[0].Get(), MulMetal->B);
                Connect(MulMetal->Output, Output->MetallicPin);

                auto* MulRough = AddNode<CMaterialExpression_Multiplication>(Graph, ColMul, 490.0f * VS);
                Connect(RoughSource, MulRough->A);
                Connect(FacRough->GetOutputPins()[0].Get(), MulRough->B);
                Connect(MulRough->Output, Output->RoughnessPin);
            }

            // Feed the raw RGB, since the output node decodes and reconstructs z. Unwired, the geometric normal stands.
            CMaterialExpression_TextureSample* TexNormal = nullptr;
            if ((FeatureSignature & MFB_NormalTexture) != 0)
            {
                TexNormal = AddNode<CMaterialExpression_TextureSample>(Graph, ColTex, 730.0f * VS);
                TexNormal->bDynamic = true;
                TexNormal->ParameterName = "NormalTexture";
                TexNormal->Texture = FlatNormal;
                ApplySlotUV(TexNormal, EMaterialTextureSlot::Normal, 730.0f * VS);
            }

            if (TexNormal != nullptr && (FeatureSignature & MFB_NormalScale) == 0)
            {
                Connect(TexNormal->GetOutputPins()[0].Get(), Output->NormalPin);
            }
            else if (TexNormal != nullptr)
            {
                // Scaling all three channels about 0.5 in ENCODED space is equivalent and costs three nodes.
                auto* Center = AddNode<CMaterialExpression_ConstantFloat>(Graph, ColTex - 320.0f, 730.0f * VS);
                Center->Value = FVector4(0.5f, 0.0f, 0.0f, 0.0f);

                auto* ScaleParam = AddNode<CMaterialExpression_ConstantFloat>(Graph, ColTex - 320.0f, 790.0f * VS);
                ScaleParam->bDynamic      = true;
                ScaleParam->ParameterName = "NormalScale";
                ScaleParam->Value         = FVector4(1.0f, 0.0f, 0.0f, 0.0f);

                auto* Sub = AddNode<CMaterialExpression_Subtraction>(Graph, ColMul - 320.0f, 730.0f * VS);
                Connect(TexNormal->GetOutputPins()[0].Get(), Sub->A);
                Connect(Center->GetOutputPins()[0].Get(), Sub->B);

                auto* Mul = AddNode<CMaterialExpression_Multiplication>(Graph, ColMul - 160.0f, 730.0f * VS);
                Connect(Sub->Output, Mul->A);
                Connect(ScaleParam->GetOutputPins()[0].Get(), Mul->B);

                auto* Add = AddNode<CMaterialExpression_Addition>(Graph, ColMul, 730.0f * VS);
                Connect(Mul->Output, Add->A);
                Connect(Center->GetOutputPins()[0].Get(), Add->B);
                Connect(Add->Output, Output->NormalPin);
            }

            // Emissive = EmissiveTexture.rgb * EmissiveColor.rgb, left unwired when the factor is black.
            if ((FeatureSignature & MFB_Emissive) != 0)
            {
                auto* FacEmissive = AddNode<CMaterialExpression_ConstantFloat4>(Graph, ColTex, 1110.0f * VS);
                FacEmissive->bDynamic = true;
                FacEmissive->ParameterName = "EmissiveColor";
                FacEmissive->Value = FVector4(0.0f, 0.0f, 0.0f, 1.0f);

                if ((FeatureSignature & MFB_EmissiveTexture) != 0)
                {
                    auto* TexEmissive = AddNode<CMaterialExpression_TextureSample>(Graph, ColTex, 920.0f * VS);
                    TexEmissive->bDynamic = true;
                    TexEmissive->ParameterName = "EmissiveTexture";
                    TexEmissive->Texture = White;
                    ApplySlotUV(TexEmissive, EMaterialTextureSlot::Emissive, 920.0f * VS);

                    auto* MulEmissive = AddNode<CMaterialExpression_Multiplication>(Graph, ColMul, 960.0f * VS);
                    Connect(TexEmissive->GetOutputPins()[0].Get(), MulEmissive->A);
                    Connect(FacEmissive->GetOutputPins()[0].Get(), MulEmissive->B);
                    Connect(MulEmissive->Output, Output->EmissivePin);
                }
                else
                {
                    Connect(FacEmissive->GetOutputPins()[0].Get(), Output->EmissivePin);
                }
            }

            // Ambient occlusion = OcclusionTexture.r, left unwired without a map.
            CMaterialExpression_TextureSample* TexOcclusion = nullptr;
            if ((FeatureSignature & MFB_OcclusionTexture) != 0)
            {
                TexOcclusion = AddNode<CMaterialExpression_TextureSample>(Graph, ColTex, 1300.0f * VS);
                TexOcclusion->bDynamic = true;
                TexOcclusion->ParameterName = "OcclusionTexture";
                TexOcclusion->Texture = White;
                ApplySlotUV(TexOcclusion, EMaterialTextureSlot::Occlusion, 1300.0f * VS);
            }

            if (TexOcclusion != nullptr && (FeatureSignature & MFB_OcclusionStrength) == 0)
            {
                Connect(TexOcclusion->GetOutputPins()[1].Get(), Output->AOPin);   // R channel
            }
            else if (TexOcclusion != nullptr)
            {
                // Strength fades the occlusion map toward unoccluded rather than scaling it.
                auto* One = AddNode<CMaterialExpression_ConstantFloat>(Graph, ColTex - 320.0f, 1300.0f * VS);
                One->Value = FVector4(1.0f, 0.0f, 0.0f, 0.0f);

                auto* StrengthParam = AddNode<CMaterialExpression_ConstantFloat>(Graph, ColTex - 320.0f, 1360.0f * VS);
                StrengthParam->bDynamic      = true;
                StrengthParam->ParameterName = "OcclusionStrength";
                StrengthParam->Value         = FVector4(1.0f, 0.0f, 0.0f, 0.0f);

                auto* Sub = AddNode<CMaterialExpression_Subtraction>(Graph, ColMul - 320.0f, 1300.0f * VS);
                Connect(TexOcclusion->GetOutputPins()[1].Get(), Sub->A);   // R channel
                Connect(One->GetOutputPins()[0].Get(), Sub->B);

                auto* Mul = AddNode<CMaterialExpression_Multiplication>(Graph, ColMul - 160.0f, 1300.0f * VS);
                Connect(Sub->Output, Mul->A);
                Connect(StrengthParam->GetOutputPins()[0].Get(), Mul->B);

                auto* Add = AddNode<CMaterialExpression_Addition>(Graph, ColMul, 1300.0f * VS);
                Connect(Mul->Output, Add->A);
                Connect(One->GetOutputPins()[0].Get(), Add->B);
                Connect(Add->Output, Output->AOPin);
            }

            // Derived from IOR at import time, so it needs a parameter rather than a math chain.
            if ((FeatureSignature & MFB_Specular) != 0)
            {
                auto* SpecularParam = AddNode<CMaterialExpression_ConstantFloat>(Graph, ColTex, 1600.0f * VS);
                SpecularParam->bDynamic      = true;
                SpecularParam->ParameterName = "Specular";
                SpecularParam->Value         = FVector4(0.5f, 0.0f, 0.0f, 0.0f);
                Connect(SpecularParam->GetOutputPins()[0].Get(), Output->SpecularPin);
            }

            // The master's ShadingModel selects the coat lobe, and these only matter once it is Clearcoat.
            if ((FeatureSignature & MFB_Clearcoat) != 0)
            {
                auto* CoatParam = AddNode<CMaterialExpression_ConstantFloat>(Graph, ColTex, 1700.0f * VS);
                CoatParam->bDynamic      = true;
                CoatParam->ParameterName = "Clearcoat";
                CoatParam->Value         = FVector4(1.0f, 0.0f, 0.0f, 0.0f);
                Connect(CoatParam->GetOutputPins()[0].Get(), Output->ClearcoatPin);

                auto* CoatRoughParam = AddNode<CMaterialExpression_ConstantFloat>(Graph, ColTex, 1780.0f * VS);
                CoatRoughParam->bDynamic      = true;
                CoatRoughParam->ParameterName = "ClearcoatRoughness";
                CoatRoughParam->Value         = FVector4(0.03f, 0.0f, 0.0f, 0.0f);
                Connect(CoatRoughParam->GetOutputPins()[0].Get(), Output->ClearcoatRoughnessPin);
            }

            if (bNeedsOpacity)
            {
                auto* FacOpacity = AddNode<CMaterialExpression_ConstantFloat>(Graph, ColTex, 1490.0f * VS);
                FacOpacity->bDynamic = true;
                FacOpacity->ParameterName = "OpacityFactor";
                FacOpacity->Value = FVector4(1.0f, 0.0f, 0.0f, 0.0f);

                if (TexBase != nullptr)
                {
                    auto* MulOpacity = AddNode<CMaterialExpression_Multiplication>(Graph, ColMul, 1300.0f * VS);
                    Connect(TexBase->GetOutputPins()[4].Get(), MulOpacity->A);    // base color A
                    Connect(FacOpacity->GetOutputPins()[0].Get(), MulOpacity->B);
                    Connect(MulOpacity->Output, Output->OpacityPin);
                }
                else
                {
                    Connect(FacOpacity->GetOutputPins()[0].Get(), Output->OpacityPin);
                }
            }

            FinalizeGraph(Graph);
        }

        // Reserved rather than merely probed, so a concurrent import cannot pick the same generated name.
        FFixedString EnsureUniquePath(const FFixedString& Path)
        {
            FFixedString Reserved = Import::PathReservations::Reserve(Path);
            return Reserved.empty() ? Path : Reserved;
        }

        // An asset an earlier import left at Path, whether it is already loaded or still only on disk.
        template<typename T>
        T* FindExistingAsset(const FFixedString& Path)
        {
            FFixedString OnDisk = Path;
            CPackage::AddPackageExt(OnDisk);
            if (FindObject<CPackage>(Path) == nullptr && !VFS::Exists(OnDisk))
            {
                return nullptr;
            }
            return LoadObject<T>(OnDisk);
        }

        // Numbered siblings are what EnsureUniquePath hands out, so a match can sit past the first few.
        constexpr uint32 kMaxReuseCandidates = 32;

        struct FInstanceParam
        {
            FName                   Name;
            EMaterialParameterType  Type    = EMaterialParameterType::Scalar;
            float                   Scalar  = 0.0f;
            FVector4                Vector  = FVector4(0.0f);
            CTexture*               Texture = nullptr;
        };

        // Every override an instance of Src carries, so a new instance and an earlier one compare on the same terms.
        TVector<FInstanceParam> BuildInstanceParams(const FMeshImportMaterial& Src, TSpan<CTexture* const> ImageAssets, uint32 FeatureSignature)
        {
            TVector<FInstanceParam> Params;
            auto Scalar = [&](const FName& Name, float Value)
            {
                Params.push_back({ Name, EMaterialParameterType::Scalar, Value });
            };
            auto Vector = [&](const FName& Name, const FVector4& Value)
            {
                FInstanceParam Param{ Name, EMaterialParameterType::Vector };
                Param.Vector = Value;
                Params.push_back(Param);
            };

            Vector("BaseColorFactor", Src.BaseColorFactor);
            Scalar("MetallicFactor", Src.MetallicFactor);
            Scalar("RoughnessFactor", Src.RoughnessFactor);
            if ((FeatureSignature & MFB_Emissive) != 0)
            {
                Vector("EmissiveColor", FVector4(Src.EmissiveColor.x, Src.EmissiveColor.y, Src.EmissiveColor.z, 1.0f));
            }

            // A channel with no image keeps the neutral default, and a failed cook warns since it flattens the look.
            auto Texture = [&](const FName& Name, int32 ImageIndex)
            {
                if (ImageIndex < 0)
                {
                    return;
                }
                CTexture* Resolved = ((size_t)ImageIndex < ImageAssets.size()) ? ImageAssets[ImageIndex] : nullptr;
                if (Resolved == nullptr)
                {
                    LOG_WARN("[MaterialImport] '{}' texture '{}' (image {}) did not resolve; using neutral default.", Src.Name, Name, ImageIndex);
                    return;
                }
                FInstanceParam Param{ Name, EMaterialParameterType::Texture };
                Param.Texture = Resolved;
                Params.push_back(Param);
            };

            Texture("BaseColorTexture", Src.BaseColorImage);
            if (UsesSplitMetalRough(Src))
            {
                Texture("MetallicTexture", Src.MetallicImage);
                Texture("RoughnessTexture", Src.RoughnessImage);
            }
            else
            {
                Texture("MetallicRoughnessTexture", Src.MetallicRoughnessImage);
            }
            Texture("NormalTexture", Src.NormalImage);
            if ((FeatureSignature & MFB_EmissiveTexture) != 0)
            {
                Texture("EmissiveTexture", Src.EmissiveImage);
            }
            Texture("OcclusionTexture", Src.OcclusionImage);

            // The master bakes only which set and whether a chain exists, so scale and offset stay per-instance.
            const uint32 UVSignature = BuildUVSignature(Src);
            for (size_t Slot = 0; Slot < (size_t)EMaterialTextureSlot::Count; ++Slot)
            {
                if (((UVSignature >> (Slot * 2 + 1)) & 0x1u) == 0)
                {
                    continue;
                }
                const FTextureUVTransform& UVT = Src.UVTransforms[Slot];
                const FString Stem = GSlotNames[Slot];
                Vector(FName((Stem + "UVScale").c_str()), FVector4(UVT.Scale.x, UVT.Scale.y, 0.0f, 0.0f));
                Vector(FName((Stem + "UVOffset").c_str()), FVector4(UVT.Offset.x, UVT.Offset.y, 0.0f, 0.0f));
                // Radians, which is what the TexCoords Rotation pin expects.
                Scalar(FName((Stem + "UVRotation").c_str()), UVT.Rotation);
            }

            if ((FeatureSignature & MFB_NormalScale) != 0)
            {
                Scalar("NormalScale", Src.NormalScale);
            }
            if ((FeatureSignature & MFB_OcclusionStrength) != 0)
            {
                Scalar("OcclusionStrength", Src.OcclusionStrength);
            }
            if ((FeatureSignature & MFB_Specular) != 0)
            {
                Scalar("Specular", ComputeEngineSpecular(Src));
            }
            if ((FeatureSignature & MFB_Clearcoat) != 0)
            {
                Scalar("Clearcoat", Src.ClearcoatFactor);
                Scalar("ClearcoatRoughness", Src.ClearcoatRoughness);
            }
            if (ToBlendMode(Src.AlphaMode) != EBlendMode::Opaque)
            {
                Scalar("OpacityFactor", Src.BaseColorFactor.w);
            }
            return Params;
        }

        void ApplyInstanceParams(CMaterialInstance* Instance, const TVector<FInstanceParam>& Params)
        {
            for (const FInstanceParam& Param : Params)
            {
                switch (Param.Type)
                {
                case EMaterialParameterType::Scalar:  Instance->SetScalarValue(Param.Name, Param.Scalar);   break;
                case EMaterialParameterType::Vector:  Instance->SetVectorValue(Param.Name, Param.Vector);   break;
                case EMaterialParameterType::Texture: Instance->SetTextureValue(Param.Name, Param.Texture); break;
                }
            }
        }

        // Exact on purpose, since two sources that merely share a name ("lambert1", "Material") must not merge.
        bool InstanceMatches(const CMaterialInstance& Candidate, const CMaterial* Master, const TVector<FInstanceParam>& Params)
        {
            if (Candidate.Material.Get() != Master)
            {
                return false;
            }

            size_t EnabledOverrides = 0;
            for (const FMaterialParameterOverride& Override : Candidate.Overrides)
            {
                EnabledOverrides += Override.bEnabled ? 1 : 0;
            }
            if (EnabledOverrides != Params.size())
            {
                return false;
            }

            for (const FInstanceParam& Param : Params)
            {
                const FMaterialParameterOverride* Override = Candidate.FindOverride(Param.Name);
                if (Override == nullptr || !Override->bEnabled || Override->Type != Param.Type)
                {
                    return false;
                }
                switch (Param.Type)
                {
                case EMaterialParameterType::Scalar:
                    if (Math::Abs(Override->Scalar - Param.Scalar) > 1e-5f)
                    {
                        return false;
                    }
                    break;
                case EMaterialParameterType::Vector:
                    if (Math::Abs(Override->Vector.x - Param.Vector.x) > 1e-5f || Math::Abs(Override->Vector.y - Param.Vector.y) > 1e-5f
                        || Math::Abs(Override->Vector.z - Param.Vector.z) > 1e-5f || Math::Abs(Override->Vector.w - Param.Vector.w) > 1e-5f)
                    {
                        return false;
                    }
                    break;
                case EMaterialParameterType::Texture:
                    if (Override->Texture.Get() != Param.Texture)
                    {
                        return false;
                    }
                    break;
                }
            }
            return true;
        }
    }

    namespace Import::Materials
    {
        using namespace Import::Mesh;

        TVector<CMaterialInstance*> GenerateMaterials(
            TSpan<const FMeshImportMaterial>    SourceMaterials,
            TSpan<CTexture* const>              ImageAssets,
            const FFixedString&                 MaterialsDir,
            const FFixedString&                 BaseName,
            TVector<TObjectPtr<CObject>>&       OutCreated,
            bool                                bSourceHasVertexColors)
        {
            TVector<CMaterialInstance*> Instances;
            if (SourceMaterials.empty())
            {
                return Instances;
            }

            // Shared by every import into MaterialsDir, so a folder of meshes carries one pair rather than one per file.
            TObjectPtr<CTexture> White;
            TObjectPtr<CTexture> FlatNormal;
            bool bDefaultsResolved = false;
            auto ResolveDefaults = [&]()
            {
                if (bDefaultsResolved)
                {
                    return;
                }
                bDefaultsResolved = true;

                auto FindOrCreate = [&](const char* Name, uint8 R, uint8 G, uint8 B) -> TObjectPtr<CTexture>
                {
                    FFixedString Path = MaterialsDir;
                    Path.append(Name);
                    if (CTexture* Existing = FindExistingAsset<CTexture>(Path))
                    {
                        return Existing;
                    }
                    TObjectPtr<CTexture> Created = CTextureFactory::CreateSolidColorTexture(EnsureUniquePath(Path), R, G, B, 255, ETextureColorSpace::Linear);
                    // Pushed ahead of any master, so reverse-order teardown frees masters before their textures.
                    if (Created)
                    {
                        OutCreated.push_back(Created.Get());
                    }
                    return Created;
                };
                White = FindOrCreate("T_ImportDefaultWhite", 255, 255, 255);
                FlatNormal = FindOrCreate("T_ImportDefaultFlatNormal", 128, 128, 255);
            };

            // One master per distinct render state (instances can only diverge in parameters, not blend/two-sided).
            struct FMasterGroup
            {
                EBlendMode              Blend;
                EMaterialShadingModel   Shading;
                bool                    bTwoSided;
                float                   Cutoff;
                uint32                  UVSignature;
                uint32                  SamplerSignature;
                uint32                  FeatureSignature;
                CMaterial*              Master;
            };
            TVector<FMasterGroup> Groups;

            auto GetMaster = [&](const FMeshImportMaterial& Src) -> CMaterial*
            {
                const EBlendMode            Blend    = ToBlendMode(Src.AlphaMode);
                const EMaterialShadingModel Shading  = Src.bUnlit               ? EMaterialShadingModel::Unlit
                                                     : (Src.ClearcoatFactor > 0.0f) ? EMaterialShadingModel::Clearcoat
                                                                                    : EMaterialShadingModel::Lit;
                const bool                  bTwoSided = Src.bTwoSided;
                const float                 Cutoff   = (Blend == EBlendMode::Masked) ? Src.AlphaCutoff : 0.0f;
                const uint32                UVSignature      = BuildUVSignature(Src);
                const uint32                SamplerSignature = BuildSamplerSignature(Src);
                const uint32                FeatureSignature = BuildFeatureSignature(Src, bSourceHasVertexColors);

                for (const FMasterGroup& Group : Groups)
                {
                    if (Group.Blend == Blend && Group.Shading == Shading && Group.bTwoSided == bTwoSided
                        && Group.UVSignature == UVSignature
                        && Group.SamplerSignature == SamplerSignature
                        && Group.FeatureSignature == FeatureSignature
                        && Math::Abs(Group.Cutoff - Cutoff) < 0.001f)
                    {
                        return Group.Master;
                    }
                }

                // The name spells out everything the graph is built from, so it alone identifies a reusable master.
                FFixedString Variant;
                if (Blend == EBlendMode::Masked)           { Variant.append("_Masked"); }
                else if (Blend == EBlendMode::Translucent) { Variant.append("_Translucent"); }
                else if (Blend == EBlendMode::Additive)    { Variant.append("_Additive"); }
                if (Blend == EBlendMode::Masked && Math::Abs(Cutoff - 0.5f) > 0.001f)
                {
                    Variant.append("_C");
                    Variant.append(Format("{}", (int32)Math::Round(Cutoff * 1000.0f)));
                }
                if (Shading == EMaterialShadingModel::Unlit)     { Variant.append("_Unlit"); }
                if (Shading == EMaterialShadingModel::Clearcoat) { Variant.append("_Clearcoat"); }
                if (bTwoSided)                               { Variant.append("_TwoSided"); }
                if (UVSignature != 0)
                {
                    Variant.append("_UV");
                    Variant.append(Format("{}", UVSignature));
                }
                if (SamplerSignature != 0)
                {
                    Variant.append("_S");
                    Variant.append(Format("{}", SamplerSignature));
                }
                if (FeatureSignature != 0)
                {
                    Variant.append("_F");
                    Variant.append(Format("{}", FeatureSignature));
                }

                FFixedString MasterPath = MaterialsDir;
                MasterPath.append("M_Imported");
                MasterPath.append(Variant.c_str());

                CMaterial* Existing = FindExistingAsset<CMaterial>(MasterPath);
                if (Existing != nullptr && Existing->MaterialType == EMaterialType::PBR && Existing->BlendMode == Blend
                    && Existing->ShadingModel == Shading && Existing->bTwoSided == bTwoSided
                    && (Blend != EBlendMode::Masked || Math::Abs(Existing->OpacityMaskClipValue - Cutoff) < 0.001f))
                {
                    Groups.push_back({ Blend, Shading, bTwoSided, Cutoff, UVSignature, SamplerSignature, FeatureSignature, Existing });
                    return Existing;
                }

                CMaterial* Master = CFactory::CreateNewOf<CMaterial>(EnsureUniquePath(MasterPath));
                if (Master == nullptr)
                {
                    return nullptr;
                }

                // glTF BLEND legitimately means blend, so cutout foliage silently pays for forward shading.
                if (Blend == EBlendMode::Translucent)
                {
                    LOG_WARN("[Import] '{}' imported as translucent (source alpha mode is BLEND). Translucency "
                             "is forward shaded through OIT and cannot be occlusion-culled. If "
                             "this is cutout geometry (foliage, fences, decals on cards), set Blend Mode to "
                             "Masked and tick Two Sided, which Translucent gets implicitly and Masked does not.",
                             Master->GetName());
                }

                Master->MaterialType  = EMaterialType::PBR;
                Master->BlendMode     = Blend;
                Master->ShadingModel  = Shading;
                Master->bTwoSided     = bTwoSided;
                if (Blend == EBlendMode::Masked)
                {
                    Master->OpacityMaskClipValue = Cutoff;
                }

                ResolveDefaults();
                CMaterialNodeGraph* Graph = MaterialGraphBuilder::CreateHeadlessGraph(Master);
                BuildPBRGraph(Graph, White.Get(), FlatNormal.Get(), Blend != EBlendMode::Opaque,
                              UVSignature, Src.Samplers, FeatureSignature);

                const FMaterialGraphCompileResult CompileResult = CompileMaterialGraph(Master, Graph);
                if (!CompileResult.bSuccess)
                {
                    // Named even with no errors, since an empty list is exactly what used to drop a master silently.
                    LOG_ERROR("[MaterialImport] Generated PBR material '{}' failed to compile ({} error(s)); "
                              "every surface using it will import without a material.",
                              Master->GetName(), (uint32)CompileResult.Errors.size());
                    for (const EdNodeGraph::FError& Error : CompileResult.Errors)
                    {
                        LOG_ERROR("[MaterialImport] Generated PBR material '{}' failed to compile [{}]: {}", Master->GetName(), Error.Name, Error.Description);
                    }
                    // A failed compile leaves the master parameterless, so emitting it would only spam errors.
                    Graph->SetMaterial(nullptr);
                    Graph->ConditionalBeginDestroy();
                    Master->ConditionalBeginDestroy();
                    return nullptr;
                }

                // Master before graph, so reverse-order teardown releases the back-ref before the master is freed.
                OutCreated.push_back(Master);
                OutCreated.push_back(Graph);
                Groups.push_back({ Blend, Shading, bTwoSided, Cutoff, UVSignature, SamplerSignature,
                                   FeatureSignature, Master });
                return Master;
            };

            Instances.resize(SourceMaterials.size(), nullptr);
            uint32 Reused = 0;

            for (size_t i = 0; i < SourceMaterials.size(); ++i)
            {
                const FMeshImportMaterial& Src = SourceMaterials[i];

                CMaterial* Master = GetMaster(Src);
                if (Master == nullptr)
                {
                    LOG_ERROR("[MaterialImport] source material '{}' produced no master; meshes using it "
                              "import with an empty material slot.", Src.Name);
                    continue;
                }

                const uint32 FeatureSignature = BuildFeatureSignature(Src, bSourceHasVertexColors);
                const TVector<FInstanceParam> Params = BuildInstanceParams(Src, ImageAssets, FeatureSignature);

                // The folder already scopes the instance, so the name only needs cleaning.
                FFixedString BasePath = MaterialsDir;
                BasePath.append(Import::MakeAssetName("MI_", Src.Name.c_str(), "Material").c_str());

                // An identical instance from an earlier import, or from earlier in this one, is shared rather than copied.
                CMaterialInstance* Instance = nullptr;
                for (uint32 N = 0; N < kMaxReuseCandidates && Instance == nullptr; ++N)
                {
                    FFixedString Candidate = BasePath;
                    if (N > 0)
                    {
                        Candidate.append("_").append(Format("{}", N).c_str());
                    }
                    CMaterialInstance* Existing = FindExistingAsset<CMaterialInstance>(Candidate);
                    if (Existing != nullptr && InstanceMatches(*Existing, Master, Params))
                    {
                        Instance = Existing;
                    }
                }
                if (Instance != nullptr)
                {
                    ++Reused;
                    Instances[i] = Instance;
                    continue;
                }

                const FFixedString InstPath = EnsureUniquePath(BasePath);
                Instance = CFactory::CreateNewOf<CMaterialInstance>(InstPath);
                if (Instance == nullptr)
                {
                    LOG_ERROR("[MaterialImport] could not create instance '{}' for source material '{}'; "
                              "meshes using it import with an empty material slot.", InstPath, Src.Name);
                    continue;
                }

                Instance->Material = Master;
                ApplyInstanceParams(Instance, Params);
                Instance->PostLoad();

                OutCreated.push_back(Instance);
                Instances[i] = Instance;
            }

            if (Reused > 0)
            {
                LOG_INFO("[MaterialImport] '{}' reused {} of {} material instance(s) already in {}.", BaseName, Reused, (uint32)SourceMaterials.size(), MaterialsDir);
            }

            return Instances;
        }
    }
}
