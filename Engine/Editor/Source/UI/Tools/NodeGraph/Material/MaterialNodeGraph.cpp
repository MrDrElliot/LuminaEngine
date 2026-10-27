#include "Containers/Queue.h"
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
#include "UI/Tools/NodeGraph/GraphNodeBuilder.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "Assets/AssetRegistry/AssetRegistry.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Assets/AssetTypes/MaterialFunction/MaterialFunction.h"
#include "Assets/Factories/Factory.h"
#include "FileSystem/FileSystem.h"
#include "Material/MaterialOps.h"
#include "Paths/Paths.h"
#include "Tools/UI/ImGui/ImGuiX.h"


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
        const bool bHasOutputNode = Algo::AnyOf(Nodes, [](const TStrongObjectPtr<CEdGraphNode>& Node)
        {
            return Node.IsValid() && Node->IsA<CMaterialOutputNode>();
        });

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

    // Reroutes are traversed but not added, since the compile loop skips them anyway.
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

        // Without its own visited set a named reroute pointing downstream of itself spins forever.
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

            for (const auto& InputPinRef : Node->GetInputPins())
            {
                CEdNodeGraphPin* InputPin = InputPinRef.Get();
                for (CEdNodeGraphPin* Conn : InputPin->GetConnections())
                {
                    Enqueue(Conn->GetOwningNode());
                }
            }
        }
    }

    // Post-order, so a node is emitted just before its first consumer and live ranges stay short.
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

        // Reroutes emit nothing, but their source still has to be walked, possibly downstream of itself.
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

        for (const auto& InputPinRef : Node->GetInputPins())
        {
            CEdNodeGraphPin* InputPin = InputPinRef.Get();
            for (CEdNodeGraphPin* Conn : InputPin->GetConnections())
            {
                CollectEmitOrderDepthFirst(Conn->GetOwningNode(), StageSet, Emitted, InProgress, OutOrder, Depth + 1);
            }
        }

        Emitted.insert(Node);
        OutOrder.push_back(Node);
    }

    // A thin alias so all reroute resolution lives in FMaterialCompiler::ResolveThroughReroutes.
    static CMaterialOutput* ResolveOutputThroughReroutes(CMaterialOutput* OutputPin)
    {
        return FMaterialCompiler::ResolveThroughReroutes(OutputPin);
    }


    // Infers the promoted output type after binary-op promotion without running full emit.
    static EMaterialInputType InferOutputType(CEdNodeGraphPin* InputPin, int32 Depth = 0);

    // The promoted width is stamped during emit, so the pin still reads Float at validation time.
    static EMaterialInputType InferMathOutputType(CMaterialExpression_Math* Node, int32 Depth)
    {
        const EMaterialInputType AType = InferOutputType(Node->A, Depth + 1);
        if (Node->B == nullptr || !Node->B->HasConnection())
        {
            // Unary, or binary against a scalar constant, so the width passes straight through.
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

    // What an output pin actually carries, which is not its declared type until emit stamps the promotion.
    static EMaterialInputType InferOutputPinType(CMaterialOutput* OutputPin, int32 Depth = 0)
    {
        if (OutputPin == nullptr)
        {
            return EMaterialInputType::Float;
        }

        // Bounded like ResolveThroughReroutes, since a cyclic graph must not recurse forever.
        constexpr int32 MaxDepth = 64;
        if (Depth < MaxDepth)
        {
            if (CMaterialExpression_Math* Math = Cast<CMaterialExpression_Math>(OutputPin->GetOwningNode()))
            {
                return InferMathOutputType(Math, Depth);
            }
        }

        return OutputPin->InputType;
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

        return InferOutputPinType(SourcePin, Depth);
    }

    // A pre-emit type check, catching mismatches where no math op fires and no promotion happens.
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

            // Named by width, since a texture sample's pin type is Texture yet it hands over a float4.
            const char* SourceTypeName = "float";
            switch (SourceComponents)
            {
                case 2: SourceTypeName = "float2"; break;
                case 3: SourceTypeName = "float3"; break;
                case 4: SourceTypeName = "float4"; break;
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

        // A preview compile leaves every node's status as the last real compile set it.
        CMaterialOutput* const PreviewPin = Compiler.GetPreviewOutput();
        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            if (PreviewPin == nullptr)
            {
                Node->ClearError();
                Node->ClearWarning();
            }
        }

        // Every walk below resolves a switch through whichever branch is stamped here, so this runs first.
        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            if (CMaterialExpression_StaticSwitch* Switch = Cast<CMaterialExpression_StaticSwitch>(Node))
            {
                const FName Param = Switch->bDynamic ? Switch->ParameterName : NAME_None;
                Switch->SetResolvedValue(Compiler.ResolveStaticSwitch(Param, Switch->bDefaultValue, Switch));
            }
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

            ApplyDiagnosticsToNodes(Compiler);
            return;
        }

        if (PreviewPin != nullptr)
        {
            CompilePreview(Compiler, PreviewPin);
            return;
        }

        // Nodes in both sets are walked twice (once per stage) to emit into both chunk buffers.
        CMaterialOutputNode* OutputNode = nullptr;
        for (const TStrongObjectPtr<CEdGraphNode>& N : Nodes)
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
        // A pin the output assigns from but that is missing here emits a reference nothing declared.
        TVector<CEdNodeGraphPin*> PixelPins;
        if (OutputNode)
        {
            // Only World Position Offset contributes to the vertex stage for now.
            CollectInputClosure(OutputNode->WorldPositionOffsetPin, VertexSet);

            OutputNode->GetPixelStagePins(PixelPins);
            for (CEdNodeGraphPin* P : PixelPins)
            {
                CollectInputClosure(P, PixelSet);
            }
        }

        Compiler.NewLine();
        Compiler.NewLine();

        // The fallback is the previous behavior, still correct but with longer live ranges.
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

        // The same list the closure came from, so a collected node always has a root to be ordered from.
        const TVector<CEdNodeGraphPin*>& PixelRoots = PixelPins;

        // Each node's share of the chunks is kept, for its cost and its code in the graph.
        THashMap<int64, FNodeInsight> Insights;
        auto EmitNode = [&Compiler, &Insights](CEdGraphNode* Node, bool bVertex)
        {
            const size_t Start = Compiler.GetActiveChunk().size();
            static_cast<CMaterialGraphNode*>(Node)->GenerateDefinition(Compiler);
            FNodeInsight& Insight = Insights[Node->GetNodeID()];
            (bVertex ? Insight.VertexCode : Insight.PixelCode) += Compiler.GetActiveChunk().substr(Start);
        };

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

            EmitNode(Node, false);
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
                EmitNode(Node, true);
            }
        }

        // The output node emits per-stage assignment chunks regardless of the cursor.
        if (OutputNode)
        {
            OutputNode->GenerateDefinition(Compiler);
        }

        // Restore default cursor.
        Compiler.SetStage(EMaterialCompileStage::Pixel);

        MaxNodeCost = 0;
        for (auto& [NodeID, Insight] : Insights)
        {
            Insight.Cost       = FMaterialCompiler::EstimateCost(Insight.PixelCode, Insight.VertexCode);
            Insight.bReadsTime = Insight.PixelCode.find("GetTime()") != FString::npos || Insight.VertexCode.find("GetTime()") != FString::npos;
            MaxNodeCost        = Math::Max(MaxNodeCost, Insight.Cost);
        }
        NodeInsights = Move(Insights);
        ++InsightGeneration;

        ApplyDiagnosticsToNodes(Compiler);
    }

    void CMaterialNodeGraph::CompilePreview(FMaterialCompiler& Compiler, CMaterialOutput* Pin)
    {
        CMaterialOutput* Source = FMaterialCompiler::ResolveThroughReroutes(Pin);
        CEdGraphNode*    Node   = Source != nullptr ? Source->GetOwningNode() : nullptr;
        if (Node == nullptr || Node->IsA<CMaterialOutputNode>())
        {
            EdNodeGraph::FError Error;
            Error.Name        = "Preview";
            Error.Description = "That pin has no value to show.";
            Compiler.AddError(Error);
            return;
        }

        THashSet<CEdGraphNode*> Closure;
        Closure.insert(Node);
        for (const TStrongObjectPtr<CEdNodeGraphPin>& Input : Node->GetInputPins())
        {
            CollectInputClosure(Input.Get(), Closure);
        }

        TVector<CEdGraphNode*>  Order;
        THashSet<CEdGraphNode*> Emitted;
        THashSet<CEdGraphNode*> InProgress;
        CollectEmitOrderDepthFirst(Node, Closure, Emitted, InProgress, Order, 0);

        Compiler.SetStage(EMaterialCompileStage::Pixel);
        Compiler.NewLine();
        for (CEdGraphNode* Ordered : Order)
        {
            if (!Ordered->IsA<CMaterialOutputNode>() && !Ordered->IsRerouteNode())
            {
                static_cast<CMaterialGraphNode*>(Ordered)->GenerateDefinition(Compiler);
            }
        }
        CMaterialOutputNode::GeneratePreviewDefinition(Compiler, Source);
    }

    void CMaterialNodeGraph::DrawPinContextMenu(CEdNodeGraphPin* Pin)
    {
        Super::DrawPinContextMenu(Pin);
        if (Cast<CMaterialOutput>(Pin) == nullptr)
        {
            return;
        }

        const bool bPreviewing = PreviewPinID == Pin->GetPinGUID();
        if (ImGui::MenuItem(LE_ICON_EYE " Preview in Viewport", nullptr, bPreviewing))
        {
            PreviewPinID = bPreviewing ? 0 : Pin->GetPinGUID();
        }
    }

    bool CMaterialNodeGraph::CollapseSelectionToFunction(FString& OutPath, FString& OutError)
    {
        const FAssetData* HostData = Material.IsValid() ? FAssetRegistry::Get().GetAssetByGUID(Material->GetGUID()) : nullptr;
        if (HostData == nullptr)
        {
            OutError = "Save the material once first, so the function has a folder to go in.";
            return false;
        }

        TVector<CEdGraphNode*> Selected;
        ImVec2 Pivot;
        CollectSelectedNodesForClone(Selected, Pivot);
        erase_if(Selected, [](const CEdGraphNode* Node)
        {
            return Node->IsCommentNode() || Node->IsA<CMaterialOutputNode>()
                || Node->IsA<CMaterialExpression_FunctionInput>() || Node->IsA<CMaterialFunctionOutput>();
        });
        if (Selected.empty())
        {
            OutError = "Select the nodes to collapse first.";
            return false;
        }

        THashSet<CEdGraphNode*> Inside;
        for (CEdGraphNode* Node : Selected)
        {
            Inside.insert(Node);
        }

        // One outside source becomes one function input, and one inside result one function output.
        struct FBoundary
        {
            CEdNodeGraphPin*          Source;
            TVector<CEdNodeGraphPin*> Targets;
        };
        TVector<FBoundary> Incoming;
        TVector<FBoundary> Outgoing;
        TVector<TPair<CEdNodeGraphPin*, CEdNodeGraphPin*>> Links;
        CollectLinks(Links);
        for (const auto& [InputPin, OutputPin] : Links)
        {
            const bool bInputInside  = Inside.contains(InputPin->GetOwningNode());
            const bool bOutputInside = Inside.contains(OutputPin->GetOwningNode());
            if (bInputInside == bOutputInside)
            {
                continue;
            }

            TVector<FBoundary>& Side = bInputInside ? Incoming : Outgoing;
            CEdNodeGraphPin* Source = OutputPin;
            auto Found = Algo::FindIf(Side, [Source](const FBoundary& Boundary) { return Boundary.Source == Source; });
            if (Found == Side.end())
            {
                Side.push_back(FBoundary{ OutputPin, { InputPin } });
            }
            else
            {
                Found->Targets.push_back(InputPin);
            }
        }

        const FString Folder = Paths::Parent(FStringView(HostData->Path.c_str()));
        FFixedString AssetPath;
        for (int32 Attempt = 1; AssetPath.empty() || VFS::Exists(AssetPath); ++Attempt)
        {
            const FString Name = Attempt == 1 ? Lumina::Format("MF_{}", Material->GetName().c_str()) : Lumina::Format("MF_{}_{}", Material->GetName().c_str(), Attempt);
            AssetPath = Paths::Combine(FStringView(Folder), FStringView(Name));
            CPackage::AddPackageExt(AssetPath);
        }

        CMaterialFunction* Function = CFactory::CreateNewOf<CMaterialFunction>(FStringView(AssetPath.c_str(), AssetPath.size()));
        CMaterialNodeGraph* FunctionGraph = Function != nullptr ? MaterialOps::FindOrCreateFunctionGraph(Function) : nullptr;
        if (FunctionGraph == nullptr)
        {
            OutError = Lumina::Format("Could not create {}.", AssetPath.c_str());
            return false;
        }

        const THashMap<CEdGraphNode*, CEdGraphNode*> Clones = FunctionGraph->CloneNodes(Selected, ImVec2(-Pivot.x, -Pivot.y));
        auto CopiedPin = [&Clones](CEdNodeGraphPin* Pin, bool bInput) -> CEdNodeGraphPin*
        {
            auto Found = Clones.find(Pin->GetOwningNode());
            if (Found == Clones.end())
            {
                return nullptr;
            }
            const auto& Original = bInput ? Pin->GetOwningNode()->GetInputPins() : Pin->GetOwningNode()->GetOutputPins();
            const auto& Copied   = bInput ? Found->second->GetInputPins() : Found->second->GetOutputPins();
            for (size_t Index = 0; Index < Original.size() && Index < Copied.size(); ++Index)
            {
                if (Original[Index].Get() == Pin)
                {
                    return Copied[Index].Get();
                }
            }
            return nullptr;
        };

        // Named after what they carry, so the call node's pins say where each one came from.
        TVector<FName> TakenNames;
        auto PinIdentifier = [&TakenNames](const CEdNodeGraphPin* Pin)
        {
            FString Base = Pin->GetOwningNode()->GetNodeTitleText() + (Pin->GetPinName().empty() ? FString() : "_" + Pin->GetPinName());
            for (char& Character : Base)
            {
                const bool bWord = (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') || (Character >= '0' && Character <= '9');
                Character = bWord ? Character : '_';
            }
            FName Name(Base.c_str());
            for (int32 Suffix = 2; Algo::Find(TakenNames, Name) != TakenNames.end(); ++Suffix)
            {
                Name = FName(Lumina::Format("{}_{}", Base, Suffix).c_str());
            }
            TakenNames.push_back(Name);
            return Name;
        };

        float MinX = FLT_MAX;
        float MaxX = -FLT_MAX;
        float MinY = FLT_MAX;
        for (const auto& [Source, Clone] : Clones)
        {
            MinX = Math::Min(MinX, Clone->GetNodeX());
            MaxX = Math::Max(MaxX, Clone->GetNodeX());
            MinY = Math::Min(MinY, Clone->GetNodeY());
        }

        constexpr float BoundaryGap     = 380.0f;
        constexpr float BoundarySpacing = 140.0f;
        for (int32 Index = 0; Index < (int32)Incoming.size(); ++Index)
        {
            CMaterialExpression_FunctionInput* Input = NewObject<CMaterialExpression_FunctionInput>(FunctionGraph->GetNodeOuter());
            Input->InputName    = PinIdentifier(Incoming[Index].Source);
            Input->InputType    = ToMaterialValueType(InferOutputPinType(Cast<CMaterialOutput>(Incoming[Index].Source)));
            Input->SortPriority = Index;
            Input->SetGridPos(MinX - BoundaryGap, MinY + BoundarySpacing * (float)Index);
            FunctionGraph->AddNode(Input);
            for (CEdNodeGraphPin* Target : Incoming[Index].Targets)
            {
                if (CEdNodeGraphPin* Copied = CopiedPin(Target, true))
                {
                    FunctionGraph->ConnectPins(Input->Output, Copied);
                }
            }
        }

        for (int32 Index = 0; Index < (int32)Outgoing.size(); ++Index)
        {
            CMaterialFunctionOutput* Output = NewObject<CMaterialFunctionOutput>(FunctionGraph->GetNodeOuter());
            Output->OutputName   = PinIdentifier(Outgoing[Index].Source);
            Output->OutputType   = ToMaterialValueType(InferOutputPinType(Cast<CMaterialOutput>(Outgoing[Index].Source)));
            Output->SortPriority = Index;
            Output->SetGridPos(MaxX + BoundaryGap, MinY + BoundarySpacing * (float)Index);
            FunctionGraph->AddNode(Output);
            if (CEdNodeGraphPin* Copied = CopiedPin(Outgoing[Index].Source, false))
            {
                FunctionGraph->ConnectPins(Copied, Output->Input);
            }
        }

        MaterialOps::SyncFunctionSignature(Function, FunctionGraph);
        if (!CPackage::SavePackage(Function->GetPackage(), FStringView(AssetPath.c_str(), AssetPath.size())))
        {
            OutError = Lumina::Format("Could not save {}.", AssetPath.c_str());
            return false;
        }
        FAssetRegistry::Get().AssetCreated(Function);

        SetPendingEditLabel("Collapse to Function");
        CMaterialExpression_MaterialFunctionCall* Call = Cast<CMaterialExpression_MaterialFunctionCall>(CreateNode(CMaterialExpression_MaterialFunctionCall::StaticClass()));
        Call->Function = TStrongObjectPtr<CMaterialFunction>(Function);
        Call->RebuildPins();
        Call->SetGridPos(Pivot.x, Pivot.y);
        RemoveNodes(Selected);

        for (int32 Index = 0; Index < (int32)Incoming.size() && Index < (int32)Call->FunctionInputPins.size(); ++Index)
        {
            ConnectPins(Incoming[Index].Source, Call->FunctionInputPins[Index]);
        }
        for (int32 Index = 0; Index < (int32)Outgoing.size() && Index < (int32)Call->FunctionOutputPins.size(); ++Index)
        {
            for (CEdNodeGraphPin* Target : Outgoing[Index].Targets)
            {
                ConnectPins(Call->FunctionOutputPins[Index], Target);
            }
        }

        GetEditor().ClearSelection();
        GetEditor().SelectNode(Call->GetNodeID(), false);
        OutPath = AssetPath.c_str();
        return true;
    }

    void CMaterialNodeGraph::DrawNodeMenuItems(CEdGraphNode* Node)
    {
        if (Material.IsValid() && ImGui::MenuItem(LE_ICON_FUNCTION_VARIANT " Collapse to Material Function"))
        {
            FString Path;
            FString Error;
            if (CollapseSelectionToFunction(Path, Error))
            {
                ImGuiX::Notifications::NotifySuccess("Collapsed the selection into {}.", Path);
            }
            else
            {
                ImGuiX::Notifications::NotifyWarning("{}", Error);
            }
        }

        CMaterialGraphNode* MaterialNode = Cast<CMaterialGraphNode>(Node);
        if (MaterialNode == nullptr || Node->GetOutputPins().empty())
        {
            return;
        }

        const bool bPreviewing = IsNodePreviewed(Node);
        if (ImGui::MenuItem(LE_ICON_EYE " Preview in Viewport", nullptr, bPreviewing))
        {
            PreviewPinID = bPreviewing ? 0 : Node->GetOutputPins()[0]->GetPinGUID();
        }
    }

    void CMaterialNodeGraph::DrawNodePreview(CEdGraphNode* Node, FGraphNodeBuilder& Builder)
    {
        constexpr float PreviewSize = 96.0f;

        CMaterialGraphNode* MaterialNode = Cast<CMaterialGraphNode>(Node);
        if (MaterialNode == nullptr || Node->GetOutputPins().empty() || !NodePreviewSource)
        {
            return;
        }

        Builder.Footer();
        if (MaterialNode->bShowPreview)
        {
            ImGui::BeginHorizontal("Preview");
            ImGui::Spring(1.0f, 0.0f);

            ImTextureRef Texture;
            const char*  Status = "Compiling";
            if (NodePreviewSource(Node->GetNodeID(), Texture, Status))
            {
                ImGui::Image(Texture, ImVec2(PreviewSize, PreviewSize));
            }
            else
            {
                // Held open at full size until the first capture lands, so the node does not jump when it does.
                const ImVec2 Min = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(PreviewSize, PreviewSize));
                ImDrawList* DrawList = ImGui::GetWindowDrawList();
                DrawList->AddRectFilled(Min, Min + ImVec2(PreviewSize, PreviewSize), IM_COL32(20, 21, 24, 255), 4.0f);
                const ImVec2 TextSize = ImGui::CalcTextSize(Status);
                DrawList->AddText(Min + (ImVec2(PreviewSize, PreviewSize) - TextSize) * 0.5f, IM_COL32(150, 155, 165, 255), Status);
            }

            ImGui::Spring(1.0f, 0.0f);
            ImGui::EndHorizontal();
        }

        if (Builder.ExpandToggle(MaterialNode->bShowPreview))
        {
            SetPendingEditLabel(MaterialNode->bShowPreview ? FName("Hide Node Preview") : FName("Show Node Preview"));
            MaterialNode->bShowPreview = !MaterialNode->bShowPreview;
            NotifyCosmeticChange();
        }
    }

    bool CMaterialNodeGraph::IsNodePreviewed(const CEdGraphNode* Node)
    {
        const CEdNodeGraphPin* Pin = PreviewPinID != 0 ? FindPinByGUID(PreviewPinID) : nullptr;
        return Pin != nullptr && Pin->GetOwningNode() == Node;
    }

    void CMaterialNodeGraph::GetLinkStyle(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin, ImU32& OutColor, float& OutThickness) const
    {
        constexpr float WidthPerComponent = 0.5f;

        Super::GetLinkStyle(InputPin, OutputPin, OutColor, OutThickness);
        const CMaterialOutput* Source = FMaterialCompiler::ResolveThroughReroutes(Cast<CMaterialOutput>(OutputPin));
        if (Source == nullptr)
        {
            return;
        }

        const int32 Masked     = Source->Mask != EComponentMask::None ? FMaterialCompiler::GetComponentCount(Source->Mask) : 0;
        const int32 Components = Masked > 0 ? Masked : FMaterialCompiler::GetComponentCount(Source->InputType);
        OutThickness += (float)(Math::Clamp(Components, 1, 4) - 1) * WidthPerComponent;
    }

    bool CMaterialNodeGraph::GetLinkBadge(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin, FString& OutText, ImU32& OutColor)
    {
        // Float inputs take any width component-wise, so only the vector-typed ones convert.
        CMaterialInput* Input = Cast<CMaterialInput>(InputPin);
        if (Input == nullptr || Input->GetInputType() < EMaterialInputType::Float2 || Input->GetInputType() > EMaterialInputType::Float4)
        {
            return false;
        }

        const int32 Wanted   = FMaterialCompiler::GetComponentCount(Input->GetInputType());
        const int32 Incoming = FMaterialCompiler::GetComponentCount(InferOutputType(Input));
        if (Incoming == Wanted || Incoming < 1 || Incoming > 4)
        {
            return false;
        }

        OutText  = Lumina::Format("{}" LE_ICON_ARROW_RIGHT_THIN "{}", Incoming, Wanted);
        OutColor = Incoming > Wanted ? IM_COL32(240, 184, 77, 255) : IM_COL32(150, 160, 175, 255);
        return true;
    }

    void CMaterialNodeGraph::RefreshTimeVaryingNodes()
    {
        const uint64 Key = (ContentVersion << 20) ^ InsightGeneration;
        if (Key == TimeVaryingKey)
        {
            return;
        }
        TimeVaryingKey = Key;
        TimeVaryingNodes.clear();

        // Everything downstream of a node reading the time changes every frame too.
        TVector<const CEdGraphNode*> Frontier;
        for (const TStrongObjectPtr<CEdGraphNode>& NodeRef : Nodes)
        {
            auto Found = NodeInsights.find(NodeRef->GetNodeID());
            if (Found != NodeInsights.end() && Found->second.bReadsTime && TimeVaryingNodes.insert(NodeRef.Get()).second)
            {
                Frontier.push_back(NodeRef.Get());
            }
        }

        while (!Frontier.empty())
        {
            const CEdGraphNode* Node = Frontier.back();
            Frontier.pop_back();
            for (const TStrongObjectPtr<CEdNodeGraphPin>& Output : Node->GetOutputPins())
            {
                for (CEdNodeGraphPin* Connection : Output->GetConnections())
                {
                    const CEdGraphNode* Downstream = Connection->GetOwningNode();
                    if (Downstream != nullptr && TimeVaryingNodes.insert(Downstream).second)
                    {
                        Frontier.push_back(Downstream);
                    }
                }
            }
        }
    }

    bool CMaterialNodeGraph::WantsLinkFlow(CEdNodeGraphPin* InputPin, CEdNodeGraphPin* OutputPin)
    {
        RefreshTimeVaryingNodes();
        return TimeVaryingNodes.contains(OutputPin->GetOwningNode());
    }

    bool CMaterialNodeGraph::GetNodeHeat(const CEdGraphNode* Node, float& OutHeat, FString& OutLabel)
    {
        auto Found = bShowCostHeat ? NodeInsights.find(Node->GetNodeID()) : NodeInsights.end();
        if (Found == NodeInsights.end() || MaxNodeCost == 0)
        {
            return false;
        }

        OutHeat  = (float)Found->second.Cost / (float)MaxNodeCost;
        OutLabel = Lumina::Format("{}", Found->second.Cost);
        return true;
    }

    bool CMaterialNodeGraph::GetNodeGeneratedCode(const CEdGraphNode* Node, FString& OutCode)
    {
        auto Found = NodeInsights.find(Node->GetNodeID());
        if (Found == NodeInsights.end() || (Found->second.PixelCode.empty() && Found->second.VertexCode.empty()))
        {
            return false;
        }

        const FNodeInsight& Insight = Found->second;
        OutCode = Insight.VertexCode.empty() ? Insight.PixelCode : "[Vertex]\n" + Insight.VertexCode + "\n[Pixel]\n" + Insight.PixelCode;
        return true;
    }

    void CMaterialNodeGraph::DrawPinTooltip(CEdNodeGraphPin* Pin)
    {
        constexpr ImVec4 TypeColor(0.60f, 0.80f, 1.00f, 1.0f);
        constexpr ImVec4 MismatchColor(1.00f, 0.72f, 0.30f, 1.0f);
        constexpr float  ValueColumn = 70.0f;

        if (CMaterialOutput* Output = Cast<CMaterialOutput>(Pin))
        {
            const EMaterialInputType Produced = InferOutputPinType(Output);
            ImGui::TextColored(TypeColor, "%s", FMaterialCompiler::GetHLSLTypeName(Produced).c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(Output->GetPinName().c_str());
            return;
        }

        CMaterialInput* Input = Cast<CMaterialInput>(Pin);
        if (Input == nullptr)
        {
            Super::DrawPinTooltip(Pin);
            return;
        }

        ImGui::TextUnformatted(Input->GetPinName().c_str());
        ImGui::Separator();

        const EMaterialInputType Expected = Input->GetInputType();
        ImGui::TextUnformatted("expects");
        ImGui::SameLine(ValueColumn);
        ImGui::TextColored(TypeColor, "%s", FMaterialCompiler::GetHLSLTypeName(Expected).c_str());

        if (!Input->HasConnection())
        {
            return;
        }

        const EMaterialInputType Incoming = InferOutputType(Input);
        const bool bWidthDiffers = FMaterialCompiler::GetComponentCount(Incoming)
                                != FMaterialCompiler::GetComponentCount(Expected);

        ImGui::TextUnformatted("receives");
        ImGui::SameLine(ValueColumn);
        ImGui::TextColored(bWidthDiffers ? MismatchColor : TypeColor, "%s",
            FMaterialCompiler::GetHLSLTypeName(Incoming).c_str());

        CMaterialOutput* SourcePin = ResolveOutputThroughReroutes(Input->GetConnection<CMaterialOutput>(0));
        if (SourcePin != nullptr && SourcePin->GetOwningNode() != nullptr)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("from %s", SourcePin->GetOwningNode()->GetNodeFullName().c_str());
        }
    }

    void CMaterialNodeGraph::ApplyDiagnosticsToNodes(const FMaterialCompiler& Compiler)
    {
        // Warnings first, so a node carrying both keeps the error as its status.
        for (const EdNodeGraph::FError& Warning : Compiler.GetWarnings())
        {
            if (Warning.Node)
            {
                Warning.Node->SetWarning(Warning);
            }
        }

        for (const EdNodeGraph::FError& Error : Compiler.GetErrors())
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

        for (const auto& NodeRef : Nodes)
        {
            CEdGraphNode* Node = NodeRef.Get();
            for (const auto& InputPinRef : Node->GetInputPins())
            {
                CEdNodeGraphPin* InputPin = InputPinRef.Get();
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
        // The node editor consumes the canvas and leaves no item, so the target covers the host rect.
        ImGuiWindow* Window = ImGui::GetCurrentWindow();
        if (Window == nullptr)
        {
            return;
        }

        if (ImGui::BeginDragDropTargetCustom(Window->Rect(), Window->ID))
        {
            // CTextureArray derives from CTexture, so the plain accept would sample slice 0 as a 2D texture.
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

        PlaceNodeAtScreen(Node, ScreenPos);
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
            NewNode->SetGridPos(CanvasPos.x, CanvasPos.y);
            GetEditor().SelectNode(NewNode->GetNodeID(), false);
        }
    }
}
