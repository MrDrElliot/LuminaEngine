#include "MaterialNodeGraph.h"
#include "MaterialCompiler.h"
#include "MaterialNamedReroute.h"
#include "MaterialReroute.h"
#include "imgui_internal.h"
#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "Assets/AssetTypes/Textures/Texture.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "Nodes/MaterialNodes.h"
#include "Tools/UI/ImGui/ImGuiDragDrop.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "UI/Tools/NodeGraph/EdNode_Reroute.h"
#include "UI/Tools/NodeGraph/GraphAlgorithms.h"


namespace Lumina
{
    void CMaterialNodeGraph::Initialize()
    {
        Super::Initialize();

        EnsureRootNodes();
        RegisterCommonMaterialNodes();
        RegisterGraphTypeNodes();

        ValidateGraph();
    }

    void CMaterialNodeGraph::EnsureRootNodes()
    {
        bool bHasOutputNode = false;
        for (const TObjectPtr<CEdGraphNode>& Node : Nodes)
        {
            if (Node.IsValid() && Node->IsA<CMaterialOutputNode>())
            {
                bHasOutputNode = true;
                break;
            }
        }

        if (!bHasOutputNode)
        {
            CreateNode(CMaterialOutputNode::StaticClass());
        }
    }

    void CMaterialNodeGraph::RegisterCommonMaterialNodes()
    {
        RegisterGraphNode(CMaterialExpression_Addition::StaticClass());
        RegisterGraphNode(CMaterialExpression_Subtraction::StaticClass());
        RegisterGraphNode(CMaterialExpression_Multiplication::StaticClass());
        RegisterGraphNode(CMaterialExpression_Division::StaticClass());
        RegisterGraphNode(CMaterialExpression_Power::StaticClass());
        RegisterGraphNode(CMaterialExpression_Mod::StaticClass());
        RegisterGraphNode(CMaterialExpression_Min::StaticClass());
        RegisterGraphNode(CMaterialExpression_Max::StaticClass());
        RegisterGraphNode(CMaterialExpression_Step::StaticClass());
        RegisterGraphNode(CMaterialExpression_Atan2::StaticClass());

        RegisterGraphNode(CMaterialExpression_Sin::StaticClass());
        RegisterGraphNode(CMaterialExpression_Cosin::StaticClass());
        RegisterGraphNode(CMaterialExpression_Tan::StaticClass());
        RegisterGraphNode(CMaterialExpression_Asin::StaticClass());
        RegisterGraphNode(CMaterialExpression_Acos::StaticClass());
        RegisterGraphNode(CMaterialExpression_Atan::StaticClass());
        RegisterGraphNode(CMaterialExpression_Sinh::StaticClass());
        RegisterGraphNode(CMaterialExpression_Cosh::StaticClass());
        RegisterGraphNode(CMaterialExpression_Tanh::StaticClass());
        RegisterGraphNode(CMaterialExpression_Sqrt::StaticClass());
        RegisterGraphNode(CMaterialExpression_Rsqrt::StaticClass());
        RegisterGraphNode(CMaterialExpression_Log::StaticClass());
        RegisterGraphNode(CMaterialExpression_Log2::StaticClass());
        RegisterGraphNode(CMaterialExpression_Log10::StaticClass());
        RegisterGraphNode(CMaterialExpression_Exp::StaticClass());
        RegisterGraphNode(CMaterialExpression_Exp2::StaticClass());
        RegisterGraphNode(CMaterialExpression_Sign::StaticClass());
        RegisterGraphNode(CMaterialExpression_OneMinus::StaticClass());
        RegisterGraphNode(CMaterialExpression_Reciprocal::StaticClass());
        RegisterGraphNode(CMaterialExpression_Round::StaticClass());
        RegisterGraphNode(CMaterialExpression_Truncate::StaticClass());
        RegisterGraphNode(CMaterialExpression_Negate::StaticClass());
        RegisterGraphNode(CMaterialExpression_Square::StaticClass());
        RegisterGraphNode(CMaterialExpression_DegreesToRadians::StaticClass());
        RegisterGraphNode(CMaterialExpression_RadiansToDegrees::StaticClass());
        RegisterGraphNode(CMaterialExpression_Floor::StaticClass());
        RegisterGraphNode(CMaterialExpression_Fract::StaticClass());
        RegisterGraphNode(CMaterialExpression_Ceil::StaticClass());
        RegisterGraphNode(CMaterialExpression_Abs::StaticClass());
        RegisterGraphNode(CMaterialExpression_Saturate::StaticClass());

        RegisterGraphNode(CMaterialExpression_Lerp::StaticClass());
        RegisterGraphNode(CMaterialExpression_Clamp::StaticClass());
        RegisterGraphNode(CMaterialExpression_SmoothStep::StaticClass());
        RegisterGraphNode(CMaterialExpression_Remap::StaticClass());

        RegisterGraphNode(CMaterialExpression_ComponentMask::StaticClass());
        RegisterGraphNode(CMaterialExpression_Append::StaticClass());
        RegisterGraphNode(CMaterialExpression_MakeFloat2::StaticClass());
        RegisterGraphNode(CMaterialExpression_MakeFloat3::StaticClass());
        RegisterGraphNode(CMaterialExpression_MakeFloat4::StaticClass());
        RegisterGraphNode(CMaterialExpression_BreakFloat2::StaticClass());
        RegisterGraphNode(CMaterialExpression_BreakFloat3::StaticClass());
        RegisterGraphNode(CMaterialExpression_BreakFloat4::StaticClass());
        RegisterGraphNode(CMaterialExpression_Normalize::StaticClass());
        RegisterGraphNode(CMaterialExpression_Distance::StaticClass());
        RegisterGraphNode(CMaterialExpression_Length::StaticClass());
        RegisterGraphNode(CMaterialExpression_Dot::StaticClass());
        RegisterGraphNode(CMaterialExpression_Cross::StaticClass());
        RegisterGraphNode(CMaterialExpression_Reflect::StaticClass());
        RegisterGraphNode(CMaterialExpression_Refract::StaticClass());
        RegisterGraphNode(CMaterialExpression_RotateAboutAxis::StaticClass());

        RegisterGraphNode(CMaterialExpression_TexCoords::StaticClass());
        RegisterGraphNode(CMaterialExpression_Panner::StaticClass());
        RegisterGraphNode(CMaterialExpression_VertexNormal::StaticClass());
        RegisterGraphNode(CMaterialExpression_VertexTangent::StaticClass());
        RegisterGraphNode(CMaterialExpression_VertexBitangent::StaticClass());
        RegisterGraphNode(CMaterialExpression_VertexColor::StaticClass());
        RegisterGraphNode(CMaterialExpression_WorldPos::StaticClass());
        RegisterGraphNode(CMaterialExpression_CameraPos::StaticClass());
        RegisterGraphNode(CMaterialExpression_ObjectScale::StaticClass());
        RegisterGraphNode(CMaterialExpression_ObjectPosition::StaticClass());
        RegisterGraphNode(CMaterialExpression_EntityID::StaticClass());
        RegisterGraphNode(CMaterialNodeGetTime::StaticClass());
        RegisterGraphNode(CMaterialExpression_CustomPrimitiveData::StaticClass());

        RegisterGraphNode(CMaterialExpression_ConstantFloat::StaticClass());
        RegisterGraphNode(CMaterialExpression_ConstantFloat2::StaticClass());
        RegisterGraphNode(CMaterialExpression_ConstantFloat3::StaticClass());
        RegisterGraphNode(CMaterialExpression_ConstantFloat4::StaticClass());
        RegisterGraphNode(CMaterialExpression_NumericConstant::StaticClass());

        RegisterGraphNode(CMaterialExpression_TextureSample::StaticClass());
        RegisterGraphNode(CMaterialExpression_CurveSample::StaticClass());

        RegisterGraphNode(CMaterialExpression_Luminance::StaticClass());
        RegisterGraphNode(CMaterialExpression_Desaturate::StaticClass());
        RegisterGraphNode(CMaterialExpression_RGBToHSV::StaticClass());
        RegisterGraphNode(CMaterialExpression_HSVToRGB::StaticClass());
        RegisterGraphNode(CMaterialExpression_Posterize::StaticClass());
        RegisterGraphNode(CMaterialExpression_GammaCorrection::StaticClass());
        RegisterGraphNode(CMaterialExpression_Contrast::StaticClass());
        RegisterGraphNode(CMaterialExpression_Brightness::StaticClass());
        RegisterGraphNode(CMaterialExpression_Tint::StaticClass());
        RegisterGraphNode(CMaterialExpression_LinearToSRGB::StaticClass());
        RegisterGraphNode(CMaterialExpression_SRGBToLinear::StaticClass());

        RegisterGraphNode(CMaterialExpression_Hash11::StaticClass());
        RegisterGraphNode(CMaterialExpression_Hash21::StaticClass());
        RegisterGraphNode(CMaterialExpression_Hash22::StaticClass());
        RegisterGraphNode(CMaterialExpression_Hash33::StaticClass());
        RegisterGraphNode(CMaterialExpression_ValueNoise::StaticClass());
        RegisterGraphNode(CMaterialExpression_GradientNoise::StaticClass());
        RegisterGraphNode(CMaterialExpression_PerlinNoise::StaticClass());
        RegisterGraphNode(CMaterialExpression_VoronoiNoise::StaticClass());
        RegisterGraphNode(CMaterialExpression_SimpleNoise::StaticClass());
        RegisterGraphNode(CMaterialExpression_Checkerboard::StaticClass());

        RegisterGraphNode(CMaterialExpression_RotateUV::StaticClass());
        RegisterGraphNode(CMaterialExpression_TilingAndOffset::StaticClass());
        RegisterGraphNode(CMaterialExpression_FlipBook::StaticClass());
        RegisterGraphNode(CMaterialExpression_PolarCoordinates::StaticClass());
        RegisterGraphNode(CMaterialExpression_TwirlUV::StaticClass());
        RegisterGraphNode(CMaterialExpression_ParallaxOcclusionMapping::StaticClass());

        RegisterGraphNode(CMaterialExpression_ScreenPosition::StaticClass());
        RegisterGraphNode(CMaterialExpression_ViewDirection::StaticClass());
        RegisterGraphNode(CMaterialExpression_ReflectionVector::StaticClass());
        RegisterGraphNode(CMaterialExpression_FragmentDepth::StaticClass());
        RegisterGraphNode(CMaterialExpression_ViewportSize::StaticClass());
        RegisterGraphNode(CMaterialExpression_AspectRatio::StaticClass());
        RegisterGraphNode(CMaterialExpression_SceneColor::StaticClass());
        RegisterGraphNode(CMaterialExpression_SceneDepth::StaticClass());
        RegisterGraphNode(CMaterialExpression_SceneHDRColor::StaticClass());

        RegisterGraphNode(CMaterialExpression_If::StaticClass());
        RegisterGraphNode(CMaterialExpression_Compare::StaticClass());

        RegisterGraphNode(CMaterialExpression_Fresnel::StaticClass());
        RegisterGraphNode(CMaterialExpression_DepthFade::StaticClass());
        RegisterGraphNode(CMaterialExpression_NormalFromHeight::StaticClass());
        RegisterGraphNode(CMaterialExpression_DeriveNormalZ::StaticClass());
        RegisterGraphNode(CMaterialExpression_BlendNormals::StaticClass());

        RegisterGraphNode(CMaterialExpression_TerrainLayerWeight::StaticClass());
        RegisterGraphNode(CMaterialExpression_TerrainLayerWeights::StaticClass());
        RegisterGraphNode(CMaterialExpression_TerrainLayerBlend::StaticClass());

        RegisterGraphNode(CMaterialExpression_MaterialFunctionCall::StaticClass());
        RegisterGraphNode(CMaterialExpression_CustomSlang::StaticClass());

        RegisterGraphNode(CMaterialReroute::StaticClass());
        RegisterGraphNode(CMaterialNamedRerouteDeclaration::StaticClass());
        RegisterGraphNode(CMaterialNamedRerouteUsage::StaticClass());
    }

    CClass* CMaterialNodeGraph::GetRerouteNodeClass() const
    {
        return CMaterialReroute::StaticClass();
    }

    bool CMaterialNodeGraph::IsGraphRootNode(CEdGraphNode* Node) const
    {
        return Cast<CMaterialOutputNode>(Node) != nullptr;
    }

    void CMaterialNodeGraph::Shutdown()
    {
        CEdNodeGraph::Shutdown();
    }

    // Reverse-BFS from a pin to partition nodes into pixel vs. vertex (WPO) sets.
    // Reroutes are traversed but not added to the set; the compile loop skips them anyway.
    static void CollectInputClosure(CEdNodeGraphPin* StartPin, THashSet<CEdGraphNode*>& OutSet)
    {
        if (StartPin == nullptr || !StartPin->HasConnection())
        {
            return;
        }

        TQueue<CEdGraphNode*> Q;
        for (CEdNodeGraphPin* Conn : StartPin->GetConnections())
        {
            CEdGraphNode* N = Conn->GetOwningNode();
            if (N != nullptr && !N->IsRerouteNode())
            {
                if (OutSet.insert(N).second)
                {
                    Q.push(N);
                }
            }
            else if (N != nullptr)
            {
                Q.push(N);
            }
        }

        // Reroutes are not added to OutSet, so they need their own visited set. Without it a named
        // reroute pointing at a declaration downstream of itself would spin here forever.
        THashSet<CEdGraphNode*> VisitedPassthrough;

        auto Enqueue = [&](CEdGraphNode* Up)
        {
            if (Up == nullptr)
            {
                return;
            }

            if (Up->IsRerouteNode())
            {
                if (VisitedPassthrough.insert(Up).second)
                {
                    Q.push(Up);
                }
            }
            else if (OutSet.insert(Up).second)
            {
                Q.push(Up);
            }
        };

        while (!Q.empty())
        {
            CEdGraphNode* Node = Q.front();
            Q.pop();

            // A named reroute usage carries no input pins; its source is the declaration's input.
            if (Node->IsRerouteNode())
            {
                if (CEdNodeGraphPin* Source = Node->GetRerouteSourcePin())
                {
                    for (CEdNodeGraphPin* Conn : Source->GetConnections())
                    {
                        Enqueue(Conn->GetOwningNode());
                    }
                }
                continue;
            }

            for (CEdNodeGraphPin* InputPin : Node->GetInputPins())
            {
                for (CEdNodeGraphPin* Conn : InputPin->GetConnections())
                {
                    Enqueue(Conn->GetOwningNode());
                }
            }
        }
    }

    // Kept as a thin alias so all reroute resolution lives in one place; see
    // FMaterialCompiler::ResolveThroughReroutes.
    static CMaterialOutput* ResolveOutputThroughReroutes(CMaterialOutput* OutputPin)
    {
        return FMaterialCompiler::ResolveThroughReroutes(OutputPin);
    }


    // Infers the promoted output type after binary-op promotion without running full emit.
    static EMaterialInputType InferOutputType(CEdNodeGraphPin* InputPin)
    {
        if (InputPin == nullptr || !InputPin->HasConnection())
        {
            return EMaterialInputType::Float;
        }

        CMaterialOutput* SourcePin = InputPin->GetConnection<CMaterialOutput>(0);
        SourcePin = ResolveOutputThroughReroutes(SourcePin);
        if (SourcePin == nullptr)
        {
            return EMaterialInputType::Float;
        }

        return SourcePin->InputType;
    }

    // Pre-emit type check: catches mismatches where no math op fires so no promotion happens.
    static void ValidateOutputConnections(CMaterialOutputNode* OutputNode, FMaterialCompiler& Compiler)
    {
        if (OutputNode == nullptr)
        {
            return;
        }

        struct FPinSpec
        {
            CEdNodeGraphPin* Pin;
            const char*      AttributeName;
            int32            RequiredComponents;
            bool             bAllowBroadcast;
        };

        const FPinSpec Specs[] =
        {
            { OutputNode->BaseColorPin,           "Base Color",            3, true },
            { OutputNode->MetallicPin,            "Metallic",              1, false },
            { OutputNode->RoughnessPin,           "Roughness",             1, false },
            { OutputNode->SpecularPin,            "Specular",              1, false },
            { OutputNode->EmissivePin,            "Emissive",              3, true },
            { OutputNode->AOPin,                  "Ambient Occlusion",     1, false },
            { OutputNode->NormalPin,              "Normal",                3, false },
            { OutputNode->OpacityPin,             "Opacity",               1, false },
            { OutputNode->WorldPositionOffsetPin, "World Position Offset", 3, false },
        };

        for (const FPinSpec& Spec : Specs)
        {
            if (Spec.Pin == nullptr || !Spec.Pin->HasConnection() || Spec.Pin->IsDisabled())
            {
                continue;
            }

            CMaterialOutput* SourcePin = Spec.Pin->GetConnection<CMaterialOutput>(0);
            SourcePin = ResolveOutputThroughReroutes(SourcePin);
            if (SourcePin == nullptr)
            {
                continue;
            }

            const EMaterialInputType SourceType = InferOutputType(Spec.Pin);
            const int32 SourceComponents = FMaterialCompiler::GetComponentCount(SourceType);
            

            if (SourceComponents == Spec.RequiredComponents)
            {
                continue;
            }

            // Single-component sources broadcast cleanly into any width.
            if (SourceComponents == 1 && Spec.bAllowBroadcast)
            {
                continue;
            }

            // 4 -> 3 swizzle is supported by EmitMaterialAssignment (.rgb).
            if (SourceComponents == 4 && Spec.RequiredComponents == 3)
            {
                continue;
            }

            // 2 -> 3 padding is supported by EmitMaterialAssignment (float3(xy, 0)).
            if (SourceComponents == 2 && Spec.RequiredComponents == 3)
            {
                continue;
            }

            // Truncating wider->1 silently is dangerous; require explicit channel extraction.
            EdNodeGraph::FError Error;
            Error.Name = "Type Mismatch";

            const char* SourceTypeName = "Float";
            switch (SourceType)
            {
                case EMaterialInputType::Float:   SourceTypeName = "float";  break;
                case EMaterialInputType::Float2:  SourceTypeName = "float2"; break;
                case EMaterialInputType::Float3:  SourceTypeName = "float3"; break;
                case EMaterialInputType::Float4:  SourceTypeName = "float4"; break;
                default: break;
            }

            const char* TargetTypeName = "float";
            switch (Spec.RequiredComponents)
            {
                case 1: TargetTypeName = "float";  break;
                case 2: TargetTypeName = "float2"; break;
                case 3: TargetTypeName = "float3"; break;
                case 4: TargetTypeName = "float4"; break;
                default: break;
            }

            Error.Description = FString("'") + Spec.AttributeName + "' expects " + TargetTypeName
                              + " but the connected pin produces " + SourceTypeName
                              + ". Insert a Mask or Append node to convert the value.";
            Error.Node        = SourcePin->GetOwningNode();
            Compiler.AddError(Error);
        }
    }

    void CMaterialNodeGraph::CompileGraph(FMaterialCompiler& Compiler)
    {
        if (Nodes.empty())
        {
            return;
        }

        for (CEdGraphNode* Node : Nodes)
        {
            Node->ClearError();
        }

        TVector<CEdGraphNode*> SortedNodes;
        CEdGraphNode* CyclicNode = GraphAlgorithms::TopologicalSortFromRoot(Nodes, SortedNodes, [](CEdGraphNode* Node)
        {
            return Cast<CMaterialOutputNode>(Node) != nullptr;
        });

        if (CyclicNode != nullptr)
        {
            EdNodeGraph::FError Error;
            Error.Name          = "Cyclic";
            Error.Description   = "Cycle detected in material node graph! Graph must be acyclic!";
            Error.Node          = CyclicNode;
            Compiler.AddError(Error);

            return;
        }

        // Nodes in both sets are walked twice (once per stage) to emit into both chunk buffers.
        CMaterialOutputNode* OutputNode = nullptr;
        for (const TObjectPtr<CEdGraphNode>& N : Nodes)
        {
            if (N.IsValid() && N->IsA<CMaterialOutputNode>())
            {
                OutputNode = static_cast<CMaterialOutputNode*>(N.Get());
                break;
            }
        }

        // Pre-emit validation catches output-boundary mismatches; compile proceeds to accumulate further errors.
        ValidateOutputConnections(OutputNode, Compiler);

        THashSet<CEdGraphNode*> VertexSet;
        THashSet<CEdGraphNode*> PixelSet;
        if (OutputNode)
        {
            // Vertex stage: only WPO contributes for now.
            CollectInputClosure(OutputNode->WorldPositionOffsetPin, VertexSet);

            // Pixel stage: every other output pin contributes.
            CEdNodeGraphPin* PixelPins[] = {
                OutputNode->BaseColorPin, OutputNode->MetallicPin,  OutputNode->RoughnessPin,
                OutputNode->SpecularPin,  OutputNode->EmissivePin,  OutputNode->AOPin,
                OutputNode->NormalPin,    OutputNode->OpacityPin
            };
            for (CEdNodeGraphPin* P : PixelPins)
            {
                CollectInputClosure(P, PixelSet);
            }
        }

        Compiler.NewLine();
        Compiler.NewLine();

        // Walk pixel set in topo order with stage = Pixel.
        Compiler.SetStage(EMaterialCompileStage::Pixel);
        for (size_t i = 0; i < SortedNodes.size(); ++i)
        {
            CEdGraphNode* Node = SortedNodes[i];
            Node->SetDebugExecutionOrder((uint32)i);

            if (Node->GetClass() == CMaterialOutputNode::StaticClass())
            {
                continue;
            }

            if (PixelSet.find(Node) == PixelSet.end())
            {
                continue;
            }

            // Reroute nodes are resolved by GetTypedInputValue; skip them in the emit pass.
            if (Node->IsRerouteNode())
            {
                continue;
            }

            static_cast<CMaterialGraphNode*>(Node)->GenerateDefinition(Compiler);
        }

        // Skipped when VertexSet empty (WPO unconnected); unmodified materials pay zero compile-time cost.
        if (!VertexSet.empty())
        {
            Compiler.SetStage(EMaterialCompileStage::Vertex);
            for (CEdGraphNode* Node : SortedNodes)
            {
                if (Node->GetClass() == CMaterialOutputNode::StaticClass())
                {
                    continue;
                }
                if (VertexSet.find(Node) == VertexSet.end())
                {
                    continue;
                }
                if (Node->IsRerouteNode())
                {
                    continue;
                }
                static_cast<CMaterialGraphNode*>(Node)->GenerateDefinition(Compiler);
            }
        }

        // Output node: emits per-stage assignment chunks via AddPixelOutput /
        // AddVertexOutput regardless of cursor.
        if (OutputNode)
        {
            OutputNode->GenerateDefinition(Compiler);
        }

        // Restore default cursor.
        Compiler.SetStage(EMaterialCompileStage::Pixel);

        for (auto& Error : Compiler.GetErrors())
        {
            if (Error.Node)
            {
                Error.Node->SetError(Error);
            }
        }
    }

    void CMaterialNodeGraph::ValidateGraph()
    {
        Connections.clear();
        Connections.reserve(16);

        for (CEdGraphNode* Node : Nodes)
        {
            for (CEdNodeGraphPin* InputPin : Node->GetInputPins())
            {
                for (CEdNodeGraphPin* Connection : InputPin->GetConnections())
                {
                    Connections.push_back(InputPin->PinID);
                    Connections.push_back(Connection->PinID);
                }
            }
        }
    }

    void CMaterialNodeGraph::SetMaterial(CMaterial* InMaterial)
    {
        Material = InMaterial;
    }

    void CMaterialNodeGraph::DrawCanvasDropTarget()
    {
        // The node editor consumes the canvas region itself and leaves no item behind, so the target is
        // registered over the whole host window rect (same pattern as the anim graph's clip drops).
        ImGuiWindow* Window = ImGui::GetCurrentWindow();
        if (Window == nullptr)
        {
            return;
        }

        if (ImGui::BeginDragDropTargetCustom(Window->Rect(), Window->ID))
        {
            // Each accept is class-checked and only fires on release, so a non-matching drag simply
            // falls through to the next candidate.
            if (CTexture* DroppedTexture = DragDrop::AcceptAsset<CTexture>())
            {
                SpawnAssetNode(CMaterialExpression_TextureSample::StaticClass(), DroppedTexture, ImGui::GetMousePos());
            }
            else if (CCurveAsset* DroppedCurve = DragDrop::AcceptAsset<CCurveAsset>())
            {
                SpawnAssetNode(CMaterialExpression_CurveSample::StaticClass(), DroppedCurve, ImGui::GetMousePos());
            }
            ImGui::EndDragDropTarget();
        }
    }

    CEdGraphNode* CMaterialNodeGraph::SpawnAssetNode(CClass* NodeClass, CObject* Asset, ImVec2 ScreenPos)
    {
        if (NodeClass == nullptr || Asset == nullptr)
        {
            return nullptr;
        }

        CEdGraphNode* Node = CreateNode(NodeClass);
        if (CMaterialExpression* Expression = Cast<CMaterialExpression>(Node))
        {
            Expression->SetNodeValue(Asset);
        }

        // Screen->canvas needs the node-editor context, which is only current inside DrawGraph.
        QueueNodePlacement(Node, ScreenPos);
        ValidateGraph();

        if (CPackage* Package = GetPackage())
        {
            Package->MarkDirty();
        }

        return Node;
    }

    void CMaterialNodeGraph::HandleQuickPlace(int Digit, ImVec2 CanvasPos)
    {
        CClass* NodeClass = nullptr;
        switch (Digit)
        {
            case 1: NodeClass = CMaterialExpression_ConstantFloat::StaticClass();   break;
            case 2: NodeClass = CMaterialExpression_ConstantFloat2::StaticClass();  break;
            case 3: NodeClass = CMaterialExpression_ConstantFloat3::StaticClass();  break;
            case 4: NodeClass = CMaterialExpression_ConstantFloat4::StaticClass();  break;
            case 5: NodeClass = CMaterialNodeGetTime::StaticClass();                break;
            case 6: NodeClass = CMaterialExpression_WorldPos::StaticClass();        break;
            case 7: NodeClass = CMaterialExpression_TexCoords::StaticClass();       break;
            case 8: NodeClass = CMaterialExpression_VertexNormal::StaticClass();    break;
            case 9: NodeClass = CMaterialExpression_Multiplication::StaticClass();  break;
            case 0: NodeClass = CMaterialExpression_Addition::StaticClass();        break;
            default: return;
        }

        QuickPlaceNode(NodeClass, CanvasPos);
    }

    void CMaterialNodeGraph::HandleQuickPlace(char Key, ImVec2 CanvasPos)
    {
        CClass* NodeClass = nullptr;
        switch (Key)
        {
            case 'T': NodeClass = CMaterialExpression_TextureSample::StaticClass(); break;
            case 'C': NodeClass = CMaterialExpression_CurveSample::StaticClass();   break;
            default: return;
        }

        QuickPlaceNode(NodeClass, CanvasPos);
    }

    void CMaterialNodeGraph::QuickPlaceNode(CClass* NodeClass, ImVec2 CanvasPos)
    {
        if (NodeClass == nullptr)
        {
            return;
        }

        if (CEdGraphNode* NewNode = CreateNode(NodeClass))
        {
            ax::NodeEditor::SetNodePosition(NewNode->GetNodeID(), CanvasPos);
            ax::NodeEditor::SelectNode(NewNode->GetNodeID(), false);
        }
    }
}
