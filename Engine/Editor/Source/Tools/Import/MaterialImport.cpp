#include "pch.h"
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
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "UI/Tools/NodeGraph/Material/MaterialGraphCompile.h"
#include "UI/Tools/NodeGraph/Material/MaterialNodeGraph.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNodeExpression.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_Constants.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_Math.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialNode_TextureSample.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialOutputNode.h"

namespace Lumina
{
    namespace MaterialGraphBuilder
    {
        CMaterialNodeGraph* CreateHeadlessGraph(CMaterial* Material)
        {
            CMaterialNodeGraph* Graph = NewObject<CMaterialNodeGraph>(Material->GetPackage(), "AssetMaterialGraph");
            Graph->SetMaterial(Material);

            // Seed the always-present output node directly. We deliberately skip CMaterialNodeGraph::Initialize()
            // (it creates an ImGui/ax editor context, which we don't need and which isn't safe off the main
            // thread); the editor recreates that context lazily when the user later opens the asset.
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
            // Links are stored on both pins (AddConnection is a one-sided push_back, see EdNodeGraph draw loop).
            OutputPin->AddConnection(InputPin);
            InputPin->AddConnection(OutputPin);
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

        // Builds the standard PBR template graph: texture-sample-times-factor for base color / metallic /
        // roughness / emissive, occlusion, and a flat-normal-defaulted normal map. Every texture and factor is
        // exposed as a named parameter so per-source-material CMaterialInstances override them. Opacity is only
        // wired for masked/translucent masters (opaque leaves the pin unconnected -> Opacity = 1).
        void BuildPBRGraph(CMaterialNodeGraph* Graph, CTexture* White, CTexture* FlatNormal, bool bNeedsOpacity)
        {
            using namespace MaterialGraphBuilder;

            CMaterialOutputNode* Output = GetOutputNode(Graph);
            if (Output == nullptr)
            {
                return;
            }

            // Layout: named columns + a vertical scale, sized so the (tall) texture-sample nodes aren't crowded.
            // Bump these to spread the auto-generated graph further apart.
            constexpr float ColTex = 0.0f;      // texture + factor inputs
            constexpr float ColMul = 620.0f;    // multiply nodes
            constexpr float ColOut = 1320.0f;   // output node
            constexpr float VS     = 1.7f;      // vertical spacing scale

            Output->SetGridPos(ColOut, 360.0f * VS);

            // Base color = BaseColorTexture.rgb * BaseColorFactor.rgb.
            auto* TexBase = AddNode<CMaterialExpression_TextureSample>(Graph, ColTex, 0.0f * VS);
            TexBase->bDynamic = true;
            TexBase->ParameterName = "BaseColorTexture";
            TexBase->Texture = White;

            auto* FacBase = AddNode<CMaterialExpression_ConstantFloat4>(Graph, ColTex, 190.0f * VS);
            FacBase->bDynamic = true;
            FacBase->ParameterName = "BaseColorFactor";
            FacBase->Value = FVector4(1.0f, 1.0f, 1.0f, 1.0f);

            auto* MulBase = AddNode<CMaterialExpression_Multiplication>(Graph, ColMul, 60.0f * VS);
            Connect(TexBase->GetOutputPins()[0].Get(), MulBase->A);   // RGBA
            Connect(FacBase->GetOutputPins()[0].Get(), MulBase->B);
            Connect(MulBase->Output, Output->BaseColorPin);

            // Metallic-roughness (glTF packing: G = roughness, B = metallic), each scaled by its factor.
            auto* TexMR = AddNode<CMaterialExpression_TextureSample>(Graph, ColTex, 380.0f * VS);
            TexMR->bDynamic = true;
            TexMR->ParameterName = "MetallicRoughnessTexture";
            TexMR->Texture = White;

            auto* FacMetal = AddNode<CMaterialExpression_ConstantFloat>(Graph, ColTex, 570.0f * VS);
            FacMetal->bDynamic = true;
            FacMetal->ParameterName = "MetallicFactor";
            FacMetal->Value = FVector4(1.0f, 0.0f, 0.0f, 0.0f);

            auto* FacRough = AddNode<CMaterialExpression_ConstantFloat>(Graph, ColTex, 650.0f * VS);
            FacRough->bDynamic = true;
            FacRough->ParameterName = "RoughnessFactor";
            FacRough->Value = FVector4(1.0f, 0.0f, 0.0f, 0.0f);

            auto* MulMetal = AddNode<CMaterialExpression_Multiplication>(Graph, ColMul, 380.0f * VS);
            Connect(TexMR->GetOutputPins()[3].Get(), MulMetal->A);    // B channel
            Connect(FacMetal->GetOutputPins()[0].Get(), MulMetal->B);
            Connect(MulMetal->Output, Output->MetallicPin);

            auto* MulRough = AddNode<CMaterialExpression_Multiplication>(Graph, ColMul, 490.0f * VS);
            Connect(TexMR->GetOutputPins()[2].Get(), MulRough->A);    // G channel
            Connect(FacRough->GetOutputPins()[0].Get(), MulRough->B);
            Connect(MulRough->Output, Output->RoughnessPin);

            // Normal map: feed the raw RGB; the output node decodes (xy*2-1) and reconstructs z.
            auto* TexNormal = AddNode<CMaterialExpression_TextureSample>(Graph, ColTex, 730.0f * VS);
            TexNormal->bDynamic = true;
            TexNormal->ParameterName = "NormalTexture";
            TexNormal->Texture = FlatNormal;
            Connect(TexNormal->GetOutputPins()[0].Get(), Output->NormalPin);

            // Emissive = EmissiveTexture.rgb * EmissiveColor.rgb.
            auto* TexEmissive = AddNode<CMaterialExpression_TextureSample>(Graph, ColTex, 920.0f * VS);
            TexEmissive->bDynamic = true;
            TexEmissive->ParameterName = "EmissiveTexture";
            TexEmissive->Texture = White;

            auto* FacEmissive = AddNode<CMaterialExpression_ConstantFloat4>(Graph, ColTex, 1110.0f * VS);
            FacEmissive->bDynamic = true;
            FacEmissive->ParameterName = "EmissiveColor";
            FacEmissive->Value = FVector4(0.0f, 0.0f, 0.0f, 1.0f);

            auto* MulEmissive = AddNode<CMaterialExpression_Multiplication>(Graph, ColMul, 960.0f * VS);
            Connect(TexEmissive->GetOutputPins()[0].Get(), MulEmissive->A);
            Connect(FacEmissive->GetOutputPins()[0].Get(), MulEmissive->B);
            Connect(MulEmissive->Output, Output->EmissivePin);

            // Ambient occlusion = OcclusionTexture.r.
            auto* TexOcclusion = AddNode<CMaterialExpression_TextureSample>(Graph, ColTex, 1300.0f * VS);
            TexOcclusion->bDynamic = true;
            TexOcclusion->ParameterName = "OcclusionTexture";
            TexOcclusion->Texture = White;
            Connect(TexOcclusion->GetOutputPins()[1].Get(), Output->AOPin);   // R channel

            if (bNeedsOpacity)
            {
                auto* FacOpacity = AddNode<CMaterialExpression_ConstantFloat>(Graph, ColTex, 1490.0f * VS);
                FacOpacity->bDynamic = true;
                FacOpacity->ParameterName = "OpacityFactor";
                FacOpacity->Value = FVector4(1.0f, 0.0f, 0.0f, 0.0f);

                auto* MulOpacity = AddNode<CMaterialExpression_Multiplication>(Graph, ColMul, 1300.0f * VS);
                Connect(TexBase->GetOutputPins()[4].Get(), MulOpacity->A);    // base color A
                Connect(FacOpacity->GetOutputPins()[0].Get(), MulOpacity->B);
                Connect(MulOpacity->Output, Output->OpacityPin);
            }

            FinalizeGraph(Graph);
        }

        FFixedString EnsureUniquePath(FFixedString Path)
        {
            if (FindObject<CPackage>(Path) == nullptr)
            {
                return Path;
            }
            for (uint32 N = 1; N < 10000; ++N)
            {
                FFixedString Candidate = Path;
                Candidate.append("_");
                Candidate.append_convert(eastl::to_string(N));
                if (FindObject<CPackage>(Candidate) == nullptr)
                {
                    return Candidate;
                }
            }
            return Path;
        }
    }

    namespace Import::Materials
    {
        using namespace Import::Mesh;

        TVector<CMaterialInstance*> GenerateMaterials(
            const FMeshImportData&                      Data,
            const FFixedString&                         MaterialsDir,
            const FFixedString&                         BaseName,
            const THashMap<FFixedString, CTexture*>&    TextureMap,
            TVector<CObject*>&                          OutCreated)
        {
            TVector<CMaterialInstance*> Instances;
            if (Data.Materials.empty())
            {
                return Instances;
            }

            // <Dir>/<Prefix><cleaned BaseName><Variant>. Prefix follows the asset convention (M_ materials,
            // T_ textures); Variant is an already-clean tag like "_Masked" appended after the sanitized core.
            auto MakeAssetPath = [&](FStringView Prefix, FStringView Variant) -> FFixedString
            {
                FFixedString Path = MaterialsDir;
                Path.append(Import::MakeAssetName(Prefix, BaseName.c_str()).c_str());
                Path.append_convert(Variant.data(), Variant.length());
                return Path;
            };

            // Neutral defaults shared by every master's texture parameters: white for color/MR/AO/emissive,
            // 128,128,255 (decodes to a flat tangent normal) for the normal channel.
            //
            // These are referenced only by each master's graph nodes (TObjectPtr<CTexture>), so hold strong
            // pins for the whole generation pass: a mid-loop master teardown (a failed compile destroys its
            // graph below) would otherwise release the last ref and free them, leaving every later master
            // building its graph against dangling White/FlatNormal pointers -> use-after-free when the next
            // compile's FMaterialCompiler releases its texture refs. Registered into OutCreated lazily (on the
            // first master that actually keeps them) so an all-fail pass frees them cleanly at function return
            // instead of leaving dangling raw pointers in the caller's reverse-order teardown list.
            TObjectPtr<CTexture> White = CTextureFactory::CreateSolidColorTexture(
                EnsureUniquePath(MakeAssetPath("T_", "_DefaultWhite")), 255, 255, 255, 255, ETextureColorSpace::Linear);
            TObjectPtr<CTexture> FlatNormal = CTextureFactory::CreateSolidColorTexture(
                EnsureUniquePath(MakeAssetPath("T_", "_DefaultFlatNormal")), 128, 128, 255, 255, ETextureColorSpace::Linear);

            bool bDefaultsRegistered = false;
            auto RegisterSharedDefaults = [&]()
            {
                if (bDefaultsRegistered)
                {
                    return;
                }
                bDefaultsRegistered = true;
                if (White)
                {
                    OutCreated.push_back(White.Get());
                }
                if (FlatNormal)
                {
                    OutCreated.push_back(FlatNormal.Get());
                }
            };

            // One master per distinct render state (instances can only diverge in parameters, not blend/two-sided).
            struct FMasterGroup
            {
                EBlendMode              Blend;
                EMaterialShadingModel   Shading;
                bool                    bTwoSided;
                float                   Cutoff;
                CMaterial*              Master;
            };
            TVector<FMasterGroup> Groups;

            auto GetMaster = [&](const FMeshImportMaterial& Src) -> CMaterial*
            {
                const EBlendMode            Blend    = ToBlendMode(Src.AlphaMode);
                const EMaterialShadingModel Shading  = Src.bUnlit ? EMaterialShadingModel::Unlit : EMaterialShadingModel::Lit;
                const bool                  bTwoSided = Src.bTwoSided;
                const float                 Cutoff   = (Blend == EBlendMode::Masked) ? Src.AlphaCutoff : 0.0f;

                for (const FMasterGroup& Group : Groups)
                {
                    if (Group.Blend == Blend && Group.Shading == Shading && Group.bTwoSided == bTwoSided
                        && Math::Abs(Group.Cutoff - Cutoff) < 0.001f)
                    {
                        return Group.Master;
                    }
                }

                // Master name: M_<BaseName>[_Masked/_Translucent/_Additive][_Unlit][_TwoSided] -- one master per
                // distinct render state. The "M_" prefix replaces the old "_Material" tag.
                FFixedString Variant;
                if (Blend == EBlendMode::Masked)           { Variant.append("_Masked"); }
                else if (Blend == EBlendMode::Translucent) { Variant.append("_Translucent"); }
                else if (Blend == EBlendMode::Additive)    { Variant.append("_Additive"); }
                if (Shading == EMaterialShadingModel::Unlit) { Variant.append("_Unlit"); }
                if (bTwoSided)                               { Variant.append("_TwoSided"); }

                CMaterial* Master = CFactory::CreateNewOf<CMaterial>(EnsureUniquePath(MakeAssetPath("M_", Variant)));
                if (Master == nullptr)
                {
                    return nullptr;
                }

                Master->MaterialType  = EMaterialType::PBR;
                Master->BlendMode     = Blend;
                Master->ShadingModel  = Shading;
                Master->bTwoSided     = bTwoSided;
                if (Blend == EBlendMode::Masked)
                {
                    Master->OpacityMaskClipValue = Cutoff;
                }

                CMaterialNodeGraph* Graph = MaterialGraphBuilder::CreateHeadlessGraph(Master);
                BuildPBRGraph(Graph, White.Get(), FlatNormal.Get(), Blend != EBlendMode::Opaque);

                const FMaterialGraphCompileResult CompileResult = CompileMaterialGraph(Master, Graph);
                if (!CompileResult.bSuccess)
                {
                    for (const EdNodeGraph::FError& Error : CompileResult.Errors)
                    {
                        LOG_ERROR("[MaterialImport] Generated PBR material '{}' failed to compile [{}]: {}", Master->GetName(), Error.Name, Error.Description);
                    }
                    // A failed compile leaves the master parameterless and not-ready; don't emit it or any
                    // instances against it (they'd just spam missing-parameter errors). Meshes in this group
                    // fall back to the engine default material. Tear down the temporaries we created.
                    Graph->SetMaterial(nullptr);
                    Graph->ConditionalBeginDestroy();
                    Master->ConditionalBeginDestroy();
                    return nullptr;
                }

                // Register the shared default textures ahead of the first master that keeps them, so the
                // caller's reverse-order teardown destroys masters/graphs before the textures they reference.
                RegisterSharedDefaults();

                // Push the master, THEN its graph: the importer's reverse-order teardown then destroys the graph
                // first, releasing its TObjectPtr<CMaterial> back-ref so the master (and its default textures)
                // can actually reach refcount 0 and free -- otherwise the graph pins them resident forever.
                OutCreated.push_back(Master);
                OutCreated.push_back(Graph);
                Groups.push_back({ Blend, Shading, bTwoSided, Cutoff, Master });
                return Master;
            };

            auto ResolveTexture = [&](const FFixedString& Key) -> CTexture*
            {
                if (Key.empty())
                {
                    return nullptr;
                }
                auto It = TextureMap.find(Key);
                return (It != TextureMap.end()) ? It->second : nullptr;
            };

            Instances.resize(Data.Materials.size(), nullptr);

            for (size_t i = 0; i < Data.Materials.size(); ++i)
            {
                const FMeshImportMaterial& Src = Data.Materials[i];

                CMaterial* Master = GetMaster(Src);
                if (Master == nullptr)
                {
                    continue;
                }

                // Instance name: MI_<source material name>, cleaned. The folder already scopes it to this import.
                FFixedString InstPath = MaterialsDir;
                InstPath.append(Import::MakeAssetName("MI_", Src.Name.c_str(), "Material").c_str());
                InstPath = EnsureUniquePath(InstPath);

                CMaterialInstance* Instance = CFactory::CreateNewOf<CMaterialInstance>(InstPath);
                if (Instance == nullptr)
                {
                    continue;
                }

                Instance->Material = Master;

                Instance->SetVectorValue("BaseColorFactor", Src.BaseColorFactor);
                Instance->SetScalarValue("MetallicFactor", Src.MetallicFactor);
                Instance->SetScalarValue("RoughnessFactor", Src.RoughnessFactor);
                Instance->SetVectorValue("EmissiveColor", FVector4(Src.EmissiveColor.x, Src.EmissiveColor.y, Src.EmissiveColor.z, 1.0f));

                // Binds a channel's texture; a non-empty key that fails to resolve warns (the usual cause of a
                // flat/untextured import). Empty key = the source material has no such map (uses neutral default).
                auto BindTexture = [&](const FName& Param, const FFixedString& Key)
                {
                    if (Key.empty())
                    {
                        return;
                    }
                    if (CTexture* Tex = ResolveTexture(Key))
                    {
                        Instance->SetTextureValue(Param, Tex);
                    }
                    else
                    {
                        LOG_WARN("[MaterialImport] '{}' texture '{}' -> '{}' did not resolve; using neutral default.", Src.Name, Param, Key);
                    }
                };

                BindTexture("BaseColorTexture", Src.BaseColorTexture);
                BindTexture("MetallicRoughnessTexture", Src.MetallicRoughnessTexture);
                BindTexture("NormalTexture", Src.NormalTexture);
                BindTexture("EmissiveTexture", Src.EmissiveTexture);
                BindTexture("OcclusionTexture", Src.OcclusionTexture);

                if (ToBlendMode(Src.AlphaMode) != EBlendMode::Opaque)
                {
                    Instance->SetScalarValue("OpacityFactor", Src.BaseColorFactor.w);
                }

                Instance->PostLoad();

                OutCreated.push_back(Instance);
                Instances[i] = Instance;
            }

            return Instances;
        }
    }
}
