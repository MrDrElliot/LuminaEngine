#include "MCPAnimGraphTools.h"

#include "Agent/AgentAssetResolve.h"
#include "Agent/AgentPropertyPath.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "Animation/AnimGraphOps.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Core/Object/Class.h"
#include "Core/Object/Package/Package.h"
#include "MCPTextMatch.h"
#include "Material/MaterialOps.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphNodeGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateTransition.h"

namespace Lumina::MCP
{
    // Named because MCPMaterialTools.cpp shares its Register* names and the two can land in one unity blob.
    namespace AnimGraphTools
    {
        struct FAnimGraphTarget
        {
            CAnimationGraph* Asset  = nullptr;
            CEdNodeGraph*    Canvas = nullptr;
        };

        // Read-only tools are fine against an open editor; only a write would desync its view.
        enum class EAnimGraphAccess : uint8
        {
            Read,
            Write,
        };

        bool ResolveCanvas(const FString& Guid, const TVector<int64>& GraphPath, EAnimGraphAccess Access,
            FAnimGraphTarget& Out, FString& OutError)
        {
            if (!Agent::ResolveAsset<CAnimationGraph>(FStringView(Guid), Out.Asset, OutError))
            {
                return false;
            }

            if (Access == EAnimGraphAccess::Write)
            {
                const FString OpenIn = MaterialOps::FindOpenEditorName(Out.Asset);
                if (!OpenIn.empty())
                {
                    OutError = Lumina::Format(
                        "'{}' is open in {}, which would not see this change. Close it and try again.",
                        Out.Asset->GetName(), OpenIn);
                    return false;
                }
            }

            CEdNodeGraph* Canvas = AnimGraphOps::FindOrCreateGraph(Out.Asset);
            if (Canvas == nullptr)
            {
                OutError = "That animation graph has no node graph and one could not be created.";
                return false;
            }

            for (const int64 NodeId : GraphPath)
            {
                CEdGraphNode* Node = MaterialOps::FindNode(Canvas, NodeId);
                if (Node == nullptr)
                {
                    OutError = Lumina::Format("GraphPath names node {}, which is not on that canvas.", NodeId);
                    return false;
                }

                // A read must not allocate a canvas, so it only follows ones that already exist.
                CEdNodeGraph* SubGraph = Access == EAnimGraphAccess::Write
                    ? Node->GetEnterableSubGraph()
                    : Node->GetOwnedSubGraph();

                if (SubGraph == nullptr)
                {
                    OutError = Lumina::Format("Node {} ({}) has no canvas. State Machine and State nodes "
                        "create theirs on the first write through them.", NodeId, Node->GetNodeDisplayName());
                    return false;
                }

                Canvas = SubGraph;
            }

            Out.Canvas = Canvas;
            return true;
        }

        void MarkAnimGraphDirty(const FAnimGraphTarget& Target)
        {
            if (CPackage* Package = Target.Asset->GetPackage())
            {
                Package->MarkDirty();
            }
        }

        bool SetPropertyFromJson(CObject* Object, const FString& Path, const FString& ValueJson,
            SAnimGraphPropertyResult& Out, FString& OutError)
        {
            Agent::FResolvedProperty Property;
            if (!Agent::ResolvePropertyPath(Object->GetClass(), Object, FStringView(Path), Property, OutError))
            {
                return false;
            }

            const nlohmann::json Value = nlohmann::json::parse(
                ValueJson.c_str(), ValueJson.c_str() + ValueJson.size(), nullptr, false);

            if (Value.is_discarded())
            {
                OutError = "Value is not JSON. Strings need quotes, so a quoted name rather than a bare word.";
                return false;
            }

            if (const Agent::FMarshalResult Check =
                    Agent::ValidatePropertyValue(Value, Property.Property, FStringView(Path));
                !Check.IsValid())
            {
                OutError = Check.Error;
                return false;
            }

            nlohmann::json Before;
            Agent::WriteProperty(Property.Property, Property.ValuePtr, Before);
            Out.Previous = FString(Before.dump().c_str());

            const Agent::FMarshalResult Applied =
                Agent::ReadProperty(Value, Property.Property, Property.ValuePtr, FStringView(Path));

            if (!Applied.IsValid())
            {
                OutError = Applied.Error;
                return false;
            }

            nlohmann::json After;
            Agent::WriteProperty(Property.Property, Property.ValuePtr, After);
            Out.Current = FString(After.dump().c_str());

            return true;
        }

        FString DescribeCompileError(const EdNodeGraph::FError& Error)
        {
            if (Error.Node == nullptr)
            {
                return Lumina::Format("[{}] {}", Error.Name, Error.Description);
            }

            return Lumina::Format("[{}] {} (node {})", Error.Name, Error.Description, Error.Node->GetNodeID());
        }

        void RegisterListNodeTypes(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListAnimGraphNodeTypesParams, SListAnimGraphNodeTypesResult>(
                Owner, "animgraph.list_node_types",
                "List the node types one animation graph canvas accepts. A blend tree and a state machine "
                "canvas take different ones.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListAnimGraphNodeTypesParams& In, SListAnimGraphNodeTypesResult& Out)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveCanvas(In.AnimGraph, In.GraphPath, EAnimGraphAccess::Read, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    for (CClass* Class : AnimGraphOps::GetPlaceableNodeTypes(Target.Canvas))
                    {
                        CEdGraphNode* CDO = Class->GetDefaultObject<CEdGraphNode>();
                        if (CDO == nullptr)
                        {
                            continue;
                        }

                        SAnimGraphNodeTypeInfo Info;
                        Info.Name        = FString(Class->GetName().ToString().c_str());
                        Info.DisplayName = FString(CDO->GetNodeDisplayName());
                        Info.Category    = FString(CDO->GetNodeCategory().c_str());
                        Info.Description = FString(CDO->GetNodeTooltip());

                        if (!ContainsText(FStringView(Info.Name), In.Contains)
                            && !ContainsText(FStringView(Info.Category), In.Contains))
                        {
                            continue;
                        }

                        Out.Types.push_back(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("{} node type(s).", Out.Types.size()));
                });
        }

        void RegisterDescribe(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAnimGraphCanvasParams, SDescribeAnimGraphResult>(
                Owner, "animgraph.describe",
                "Report one canvas of an animation graph: every node, its pins and links, its field values, "
                "and on a state machine canvas every transition.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SAnimGraphCanvasParams& In, SDescribeAnimGraphResult& Out)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveCanvas(In.AnimGraph, In.GraphPath, EAnimGraphAccess::Read, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.Name        = FString(Target.Asset->GetName().ToString().c_str());
                    Out.CanvasClass = FString(Target.Canvas->GetClass()->GetName().ToString().c_str());

                    for (const TObjectPtr<CEdGraphNode>& Node : Target.Canvas->Nodes)
                    {
                        if (!Node.IsValid())
                        {
                            continue;
                        }

                        SAnimGraphNodeInfo Info;
                        Info.Id         = Node->GetNodeID();
                        Info.Type       = FString(Node->GetClass()->GetName().ToString().c_str());
                        Info.Title      = Node->GetNodeTitleText();
                        Info.bHasCanvas = Node->GetOwnedSubGraph() != nullptr;

                        CollectPins(Node.Get(), Info.Pins);

                        nlohmann::json Values;
                        if (Agent::WriteStruct(Node->GetClass(), Node.Get(), Values).IsValid())
                        {
                            Info.Values = FString(Values.dump().c_str());
                        }

                        Out.Nodes.push_back(Move(Info));
                    }

                    for (CAnimStateTransition* Transition : AnimGraphOps::GetTransitions(Target.Canvas))
                    {
                        SAnimGraphTransitionInfo Info;
                        Info.FromNode = Transition->FromStateNodeID;
                        Info.ToNode   = Transition->ToStateNodeID;

                        nlohmann::json Values;
                        if (Agent::WriteStruct(Transition->GetClass(), Transition, Values).IsValid())
                        {
                            Info.Values = FString(Values.dump().c_str());
                        }

                        Out.Transitions.push_back(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("'{}' canvas has {} node(s), {} transition(s).",
                        Out.Name, Out.Nodes.size(), Out.Transitions.size()));
                });
        }

        void RegisterAddNode(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAddAnimGraphNodeParams, SAddAnimGraphNodeResult>(
                Owner, "animgraph.add_node",
                "Add a node to one animation graph canvas and report the pins it came with.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAddAnimGraphNodeParams& In, SAddAnimGraphNodeResult& Out)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveCanvas(In.AnimGraph, In.GraphPath, EAnimGraphAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CClass* NodeClass = AnimGraphOps::ResolveNodeType(Target.Canvas, FStringView(In.NodeType));
                    if (NodeClass == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "This canvas takes no node type named '{}'. Use animgraph.list_node_types.",
                            In.NodeType));
                    }

                    CEdGraphNode* Node = MaterialOps::AddNode(Target.Canvas, NodeClass, In.X, In.Y);
                    if (Node == nullptr)
                    {
                        return Agent::FToolResult::Error("The graph refused to create that node.");
                    }

                    Out.Id = Node->GetNodeID();
                    CollectPins(Node, Out.Pins);

                    MarkAnimGraphDirty(Target);

                    return Agent::FToolResult::Ok(Lumina::Format("Added {} as node {}.", In.NodeType, Out.Id));
                });
        }

        void RegisterRemoveNode(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAnimGraphNodeParams, SAnimGraphEditResult>(
                Owner, "animgraph.remove_node",
                "Remove a node from an animation graph canvas, with its links, transitions and any canvas it owns.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAnimGraphNodeParams& In, SAnimGraphEditResult& Out)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveCanvas(In.AnimGraph, In.GraphPath, EAnimGraphAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CEdGraphNode* Node = MaterialOps::FindNode(Target.Canvas, In.Node);
                    if (Node == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No node {} is on this canvas.", In.Node));
                    }

                    if (!MaterialOps::RemoveNode(Target.Canvas, Node, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.bSucceeded = true;
                    MarkAnimGraphDirty(Target);

                    return Agent::FToolResult::Ok(Lumina::Format("Removed node {}.", In.Node));
                });
        }

        void RegisterConnect(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SConnectAnimGraphParams, SAnimGraphEditResult>(
                Owner, "animgraph.connect",
                "Wire an output pin into an input pin. On a state machine canvas, wiring one state's output "
                "into another's input creates a transition; set its rule with animgraph.set_transition_property.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SConnectAnimGraphParams& In, SAnimGraphEditResult& Out)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveCanvas(In.AnimGraph, In.GraphPath, EAnimGraphAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CEdGraphNode* From = MaterialOps::FindNode(Target.Canvas, In.FromNode);
                    CEdGraphNode* To   = MaterialOps::FindNode(Target.Canvas, In.ToNode);

                    if (From == nullptr || To == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No node {} is on this canvas.",
                            From == nullptr ? In.FromNode : In.ToNode));
                    }

                    CEdNodeGraphPin* OutputPin =
                        MaterialOps::FindPin(From, FStringView(In.FromPin), ENodePinDirection::Output);

                    if (OutputPin == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Node {} has no output pin '{}'. It has {}.",
                            In.FromNode, In.FromPin,
                            MaterialOps::DescribePinNames(From, ENodePinDirection::Output)));
                    }

                    CEdNodeGraphPin* InputPin =
                        MaterialOps::FindPin(To, FStringView(In.ToPin), ENodePinDirection::Input);

                    if (InputPin == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Node {} has no input pin '{}'. It has {}.",
                            In.ToNode, In.ToPin,
                            MaterialOps::DescribePinNames(To, ENodePinDirection::Input)));
                    }

                    if (!MaterialOps::ConnectPins(Target.Canvas, OutputPin, InputPin, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.bSucceeded = true;
                    MarkAnimGraphDirty(Target);

                    return Agent::FToolResult::Ok(Lumina::Format("Connected {}.{} to {}.{}.",
                        In.FromNode, In.FromPin, In.ToNode, In.ToPin));
                });
        }

        void RegisterDisconnect(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SDisconnectAnimGraphParams, SAnimGraphEditResult>(
                Owner, "animgraph.disconnect",
                "Clear every link on one pin. On a state machine canvas this deletes the transitions it carried.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SDisconnectAnimGraphParams& In, SAnimGraphEditResult& Out)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveCanvas(In.AnimGraph, In.GraphPath, EAnimGraphAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CEdGraphNode* Node = MaterialOps::FindNode(Target.Canvas, In.Node);
                    if (Node == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No node {} is on this canvas.", In.Node));
                    }

                    CEdNodeGraphPin* Pin = MaterialOps::FindPin(Node, FStringView(In.Pin), ENodePinDirection::Input);
                    if (Pin == nullptr)
                    {
                        Pin = MaterialOps::FindPin(Node, FStringView(In.Pin), ENodePinDirection::Output);
                    }

                    if (Pin == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Node {} has no pin '{}'.", In.Node, In.Pin));
                    }

                    if (!MaterialOps::DisconnectPin(Target.Canvas, Pin, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.bSucceeded = true;
                    MarkAnimGraphDirty(Target);

                    return Agent::FToolResult::Ok(Lumina::Format("Disconnected {}.{}.", In.Node, In.Pin));
                });
        }

        void RegisterSetNodeProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetAnimGraphNodePropertyParams, SAnimGraphPropertyResult>(
                Owner, "animgraph.set_node_property",
                "Set one field on one node, such as a clip, a state's name or a parameter name. An unwired "
                "value pin's default lives in PinDefaults, as [{\"PinName\": \"Alpha\", \"Value\": 0.5}].",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetAnimGraphNodePropertyParams& In, SAnimGraphPropertyResult& Out)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveCanvas(In.AnimGraph, In.GraphPath, EAnimGraphAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CEdGraphNode* Node = MaterialOps::FindNode(Target.Canvas, In.Node);
                    if (Node == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No node {} is on this canvas.", In.Node));
                    }

                    if (!SetPropertyFromJson(Node, In.Path, In.Value, Out, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    MaterialOps::NotifyNodeValuesChanged(Target.Canvas);
                    MarkAnimGraphDirty(Target);

                    return Agent::FToolResult::Ok(Lumina::Format("Node {} {} is now {}.",
                        In.Node, In.Path, Out.Current));
                });
        }

        void RegisterSetTransitionProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetAnimGraphTransitionPropertyParams, SAnimGraphPropertyResult>(
                Owner, "animgraph.set_transition_property",
                "Set one field on the transition between two nodes of a state machine canvas. Conditions takes "
                "a list of {ConditionSource, ParameterName, CurveName, Compare, CompareValue}.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetAnimGraphTransitionPropertyParams& In, SAnimGraphPropertyResult& Out)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveCanvas(In.AnimGraph, In.GraphPath, EAnimGraphAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CAnimStateTransition* Found = nullptr;
                    for (CAnimStateTransition* Transition : AnimGraphOps::GetTransitions(Target.Canvas))
                    {
                        if (Transition->FromStateNodeID == In.FromNode && Transition->ToStateNodeID == In.ToNode)
                        {
                            Found = Transition;
                            break;
                        }
                    }

                    if (Found == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format(
                            "No transition runs from node {} to node {} on this canvas. "
                            "animgraph.connect the two states first.", In.FromNode, In.ToNode));
                    }

                    if (!SetPropertyFromJson(Found, In.Path, In.Value, Out, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    MarkAnimGraphDirty(Target);

                    return Agent::FToolResult::Ok(Lumina::Format("Transition {} -> {} {} is now {}.",
                        In.FromNode, In.ToNode, In.Path, Out.Current));
                });
        }

        void RegisterCompile(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCompileAnimGraphParams, SAnimGraphEditResult>(
                Owner, "animgraph.compile",
                "Compile an animation graph into the bytecode its entities run, and report what the graph got "
                "wrong. Save with assets.save afterwards.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCompileAnimGraphParams& In, SAnimGraphEditResult& Out)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveCanvas(In.AnimGraph, {}, EAnimGraphAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    FAnimationGraphCompiler Compiler;
                    Out.bSucceeded = AnimGraphOps::Compile(Target.Asset,
                        AnimGraphOps::FindOrCreateGraph(Target.Asset), Compiler);

                    for (const EdNodeGraph::FError& Item : Compiler.GetErrors())
                    {
                        Out.Errors.push_back(DescribeCompileError(Item));
                    }

                    for (const EdNodeGraph::FError& Item : Compiler.GetWarnings())
                    {
                        Out.Warnings.push_back(DescribeCompileError(Item));
                    }

                    if (!Out.bSucceeded)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Compile failed with {} error(s). {}",
                            Out.Errors.size(), Out.Errors.empty() ? FString() : Out.Errors[0]));
                    }

                    MarkAnimGraphDirty(Target);

                    return Agent::FToolResult::Ok(Lumina::Format("Compiled with {} warning(s).",
                        Out.Warnings.size()));
                });
        }
    }

    void RegisterAnimGraphTools(FStringView Owner)
    {
        AnimGraphTools::RegisterListNodeTypes(Owner);
        AnimGraphTools::RegisterDescribe(Owner);
        AnimGraphTools::RegisterAddNode(Owner);
        AnimGraphTools::RegisterRemoveNode(Owner);
        AnimGraphTools::RegisterConnect(Owner);
        AnimGraphTools::RegisterDisconnect(Owner);
        AnimGraphTools::RegisterSetNodeProperty(Owner);
        AnimGraphTools::RegisterSetTransitionProperty(Owner);
        AnimGraphTools::RegisterCompile(Owner);
    }
}
