#include "MaterialNodeGraph.h"
#include "MaterialCompiler.h"
#include "MaterialGraphSchema.h"
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

    CClass* CMaterialNodeGraph::GetRerouteNodeClass() const
    {
        return CMaterialReroute::StaticClass();
    }

    const FEdGraphSchema& CMaterialNodeGraph::GetSchema() const
    {
        return GetMaterialGraphSchema();
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

    // Emission order for one stage: depth-first POST-ORDER from that stage's root pins, so a node is
    // emitted just before its first consumer and live ranges stay as short as the graph allows.
    static void CollectEmitOrderDepthFirst(CEdGraphNode* Node,
                                           const THashSet<CEdGraphNode*>& StageSet,
                                           THashSet<CEdGraphNode*>& Emitted,
                                           THashSet<CEdGraphNode*>& InProgress,
                                           TVector<CEdGraphNode*>& OutOrder,
                                           int32 Depth)
    {
        constexpr int32 MaxDepth = 256;
        if (Node == nullptr || Depth > MaxDepth)
        {
            return;
        }

        // Reroutes emit nothing -- GetTypedInputValue resolves through them -- but their source still has
        // to be walked, and a named reroute can point at a declaration downstream of itself.
        if (Node->IsRerouteNode())
        {
            if (!InProgress.insert(Node).second)
            {
                return;
            }
            if (CEdNodeGraphPin* Source = Node->GetRerouteSourcePin())
            {
                for (CEdNodeGraphPin* Conn : Source->GetConnections())
                {
                    CollectEmitOrderDepthFirst(Conn->GetOwningNode(), StageSet, Emitted, InProgress, OutOrder, Depth + 1);
                }
            }
            return;
        }

        if (StageSet.find(Node) == StageSet.end() || Emitted.find(Node) != Emitted.end())
        {
            return;
        }
        if (!InProgress.insert(Node).second)
        {
            return;
        }

        for (CEdNodeGraphPin* InputPin : Node->GetInputPins())
        {
            for (CEdNodeGraphPin* Conn : InputPin->GetConnections())
            {
                CollectEmitOrderDepthFirst(Conn->GetOwningNode(), StageSet, Emitted, InProgress, OutOrder, Depth + 1);
            }
        }

        Emitted.insert(Node);
        OutOrder.push_back(Node);
    }

    // Kept as a thin alias so all reroute resolution lives in one place; see
    // FMaterialCompiler::ResolveThroughReroutes.
    static CMaterialOutput* ResolveOutputThroughReroutes(CMaterialOutput* OutputPin)
    {
        return FMaterialCompiler::ResolveThroughReroutes(OutputPin);
    }


    // Infers the promoted output type after binary-op promotion without running full emit.
    static EMaterialInputType InferOutputType(CEdNodeGraphPin* InputPin, int32 Depth = 0);

    /**
     * A math node's output pin still carries its BuildNode default (Float) at validation time -- the
     * promoted width is only stamped during emit, which runs AFTER this check. Reading the pin directly
     * therefore reports every math chain as a float, no matter how wide it actually is, and rejects it on
     * any pin that does not allow broadcast.
     *
     * So recompute the promotion here, by the same rule FMaterialCompiler::DetermineResultType uses for
     * component-wise ops: a one-component operand takes the other side's width, otherwise the wider wins.
     * An unconnected input is the node's scalar ConstA/ConstB, which is a float.
     */
    static EMaterialInputType InferMathOutputType(CMaterialExpression_Math* Node, int32 Depth)
    {
        const EMaterialInputType AType = InferOutputType(Node->A, Depth + 1);
        if (Node->B == nullptr || !Node->B->HasConnection())
        {
            // Unary, or binary against a scalar constant: the width passes straight through.
            return AType;
        }

        const EMaterialInputType BType = InferOutputType(Node->B, Depth + 1);

        const int32 CountA = FMaterialCompiler::GetComponentCount(AType);
        const int32 CountB = FMaterialCompiler::GetComponentCount(BType);
        if (CountA == 1)
        {
            return BType;
        }
        if (CountB == 1)
        {
            return AType;
        }
        return CountA >= CountB ? AType : BType;
    }

    static EMaterialInputType InferOutputType(CEdNodeGraphPin* InputPin, int32 Depth)
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

        // Bounded like ResolveThroughReroutes: a cyclic graph must not recurse forever.
        constexpr int32 MaxDepth = 64;
        if (Depth < MaxDepth)
        {
            if (CMaterialExpression_Math* Math = Cast<CMaterialExpression_Math>(SourcePin->GetOwningNode()))
            {
                return InferMathOutputType(Math, Depth);
            }
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
        // Both the closure and the emit roots below come from this one list, and it comes from the output
        // node itself: a pin the node assigns from but that is missing here emits an assignment referencing
        // a variable nothing ever declared, which fails the whole material's compile.
        TVector<CEdNodeGraphPin*> PixelPins;
        if (OutputNode)
        {
            // Vertex stage: only WPO contributes for now.
            CollectInputClosure(OutputNode->WorldPositionOffsetPin, VertexSet);

            OutputNode->GetPixelStagePins(PixelPins);
            for (CEdNodeGraphPin* P : PixelPins)
            {
                CollectInputClosure(P, PixelSet);
            }
        }

        Compiler.NewLine();
        Compiler.NewLine();

        // Falls back to the global topological order if the depth-first walk did not reach the whole set.
        // The fallback is the previous behaviour: still correct, just with the longer live ranges.
        auto BuildEmitOrder = [&SortedNodes](const THashSet<CEdGraphNode*>& StageSet,
                                             const TVector<CEdNodeGraphPin*>& RootPins) -> TVector<CEdGraphNode*>
        {
            TVector<CEdGraphNode*> Order;
            Order.reserve(StageSet.size());

            THashSet<CEdGraphNode*> Emitted;
            THashSet<CEdGraphNode*> InProgress;
            for (CEdNodeGraphPin* Pin : RootPins)
            {
                if (Pin == nullptr || !Pin->HasConnection())
                {
                    continue;
                }
                for (CEdNodeGraphPin* Conn : Pin->GetConnections())
                {
                    CollectEmitOrderDepthFirst(Conn->GetOwningNode(), StageSet, Emitted, InProgress, Order, 0);
                }
            }

            if (Order.size() != StageSet.size())
            {
                Order.clear();
                for (CEdGraphNode* Node : SortedNodes)
                {
                    if (StageSet.find(Node) != StageSet.end() && !Node->IsRerouteNode())
                    {
                        Order.push_back(Node);
                    }
                }
            }
            return Order;
        };

        // Same list the closure was built from, so a node that was collected always has a root to be
        // ordered from and can never end up emitted after the assignment that reads it.
        const TVector<CEdNodeGraphPin*>& PixelRoots = PixelPins;

        Compiler.SetStage(EMaterialCompileStage::Pixel);
        const TVector<CEdGraphNode*> PixelOrder = BuildEmitOrder(PixelSet, PixelRoots);
        for (size_t i = 0; i < PixelOrder.size(); ++i)
        {
            CEdGraphNode* Node = PixelOrder[i];
            Node->SetDebugExecutionOrder((uint32)i);

            if (Node->GetClass() == CMaterialOutputNode::StaticClass() || Node->IsRerouteNode())
            {
                continue;
            }

            static_cast<CMaterialGraphNode*>(Node)->GenerateDefinition(Compiler);
        }

        // Skipped when VertexSet empty (WPO unconnected); unmodified materials pay zero compile-time cost.
        if (!VertexSet.empty())
        {
            Compiler.SetStage(EMaterialCompileStage::Vertex);

            TVector<CEdNodeGraphPin*> VertexRoots;
            VertexRoots.push_back(OutputNode ? OutputNode->WorldPositionOffsetPin : nullptr);

            for (CEdGraphNode* Node : BuildEmitOrder(VertexSet, VertexRoots))
            {
                if (Node->GetClass() == CMaterialOutputNode::StaticClass() || Node->IsRerouteNode())
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
            // Arrays are checked BEFORE plain textures: CTextureArray derives from CTexture, so the accept
            // below would claim one and spawn a node sampling slice 0 as if it were a 2D texture.
            if (CTextureArray* DroppedArray = DragDrop::AcceptAsset<CTextureArray>())
            {
                SpawnAssetNode(CMaterialExpression_TextureSampleArray::StaticClass(), DroppedArray, ImGui::GetMousePos());
            }
            else if (CTexture* DroppedTexture = DragDrop::AcceptAsset<CTexture>())
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
