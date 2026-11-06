#include "MCPAnimGraphTools.h"

#include "Agent/AgentPropertyPath.h"
#include "Agent/AgentToolMarshal.h"
#include "Agent/AgentToolRegistry.h"
#include "Animation/AnimGraphOps.h"
#include "Assets/AssetTypes/Animation/AnimationGraph/AnimationGraph.h"
#include "Core/Object/Class.h"
#include "MCPGraphTools.h"
#include "MCPServer.h"
#include "MCPTextMatch.h"
#include "Session/SessionOps.h"
#include "UI/Tools/NodeGraph/NodeGraphOps.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphNodeGraph.h"
#include "UI/Tools/NodeGraph/Animation/AnimStateTransition.h"

namespace Lumina::MCP
{
    namespace MCPAnimGraphToolsInternal
    {
        struct FAnimGraphTarget
        {
            CAnimationGraph* Asset  = nullptr;
            CEdNodeGraph*    Canvas = nullptr;
        };

        // A read never allocates a sub-graph canvas, while a write creates one it passes through.
        enum class EAnimGraphAccess : uint8
        {
            Read,
            Write,
        };

        bool ResolveAnimGraphCanvas(const FString& Guid, const TVector<int64>& GraphPath, EAnimGraphAccess Access,
            FAnimGraphTarget& Out, FString& OutError)
        {
            if (!ResolveEditableAsset(FStringView(Guid), Out.Asset, OutError))
            {
                return false;
            }

            CEdNodeGraph* Canvas = AnimGraphOps::FindOrCreateGraph(Out.Asset);
            if (Canvas == nullptr)
            {
                OutError = "That animation graph has no node graph and one could not be created.";
                return false;
            }

            for (const int64 NodeId : GraphPath)
            {
                CEdGraphNode* Node = NodeGraphOps::FindNode(Canvas, NodeId);
                if (Node == nullptr)
                {
                    OutError = Lumina::Format("GraphPath names node {}, which is not on that canvas.", NodeId);
                    return false;
                }

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

            // A canvas the open editor has not visited yet would otherwise take this edit with no undo.
            if (Access == EAnimGraphAccess::Write)
            {
                SessionOps::AdoptNodeGraph(Out.Asset, Canvas);
            }
            return true;
        }

        bool SetAnimGraphPropertyFromJson(CObject* Object, const FString& Path, const FString& ValueJson,
            SAnimGraphPropertyResult& Out, FString& OutError)
        {
            Agent::FResolvedProperty Property;
            if (!Agent::ResolvePropertyPath(Object->GetClass(), Object, FStringView(Path), Property, OutError))
            {
                return false;
            }

            FJsonValue Value;
            if (!FJsonValue::Parse(ValueJson, Value))
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

            FJsonValue Before;
            Agent::WriteProperty(Property.Property, Property.ValuePtr, Before);
            Out.Previous = Before.ToString();

            const Agent::FMarshalResult Applied =
                Agent::ReadProperty(Value, Property.Property, Property.ValuePtr, FStringView(Path));

            if (!Applied.IsValid())
            {
                OutError = Applied.Error;
                return false;
            }

            FJsonValue After;
            Agent::WriteProperty(Property.Property, Property.ValuePtr, After);
            Out.Current = After.ToString();

            return true;
        }

        FString DescribeAnimGraphCompileError(const EdNodeGraph::FError& Error)
        {
            if (Error.Node == nullptr)
            {
                return Lumina::Format("[{}] {}", Error.Name, Error.Description);
            }

            return Lumina::Format("[{}] {} (node {})", Error.Name, Error.Description, Error.Node->GetNodeID());
        }

        void RegisterAnimGraphListNodeTypes(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SListAnimGraphNodeTypesParams, SListAnimGraphNodeTypesResult>(
                Owner, "animgraph.list_node_types",
                "List the node types one animation graph canvas accepts; blend trees and state canvases differ.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SListAnimGraphNodeTypesParams& In, SListAnimGraphNodeTypesResult& Out)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveAnimGraphCanvas(In.AnimGraph, In.GraphPath, EAnimGraphAccess::Read, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    for (CClass* Class : NodeGraphOps::GetPlaceableNodeTypes(Target.Canvas->GetClass()))
                    {
                        CEdGraphNode* CDO = Class->GetDefaultObject<CEdGraphNode>();
                        if (CDO == nullptr)
                        {
                            continue;
                        }

                        SAnimGraphNodeTypeInfo Info;
                        Info.Name        = FString(*Class->GetName().ToString());
                        Info.DisplayName = FString(CDO->GetNodeDisplayName());
                        Info.Category    = FString(*CDO->GetNodeCategory());
                        Info.Description = FString(CDO->GetNodeTooltip());

                        if (!ContainsText(FStringView(Info.Name), In.Contains)
                            && !ContainsText(FStringView(Info.Category), In.Contains))
                        {
                            continue;
                        }

                        Out.Types.Add(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("{} node type(s).", Out.Types.Num()));
                });
        }

        void RegisterAnimGraphDescribe(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAnimGraphCanvasParams, SDescribeAnimGraphResult>(
                Owner, "animgraph.describe",
                "Report one animation graph canvas: every node with its pins, links and field values, and on a "
                "state machine canvas every transition.",
                Agent::EToolEffect::ReadOnly, Agent::EToolThread::GameThread,
                [](const SAnimGraphCanvasParams& In, SDescribeAnimGraphResult& Out)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveAnimGraphCanvas(In.AnimGraph, In.GraphPath, EAnimGraphAccess::Read, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    Out.Name        = FString(*Target.Asset->GetName().ToString());
                    Out.CanvasClass = FString(*Target.Canvas->GetClass()->GetName().ToString());

                    for (const TStrongObjectPtr<CEdGraphNode>& Node : Target.Canvas->Nodes)
                    {
                        if (!Node.IsValid())
                        {
                            continue;
                        }

                        SAnimGraphNodeInfo Info;
                        Info.Id         = Node->GetNodeID();
                        Info.Type       = FString(*Node->GetClass()->GetName().ToString());
                        Info.Title      = Node->GetNodeTitleText();
                        Info.bHasCanvas = Node->GetOwnedSubGraph() != nullptr;
                        Info.X          = Node->GetNodeX();
                        Info.Y          = Node->GetNodeY();

                        CollectPins(Node.Get(), Info.Pins);

                        FJsonValue Values;
                        if (Agent::WriteStruct(Node->GetClass(), Node.Get(), Values).IsValid())
                        {
                            Info.Values = Values.ToString();
                        }

                        Out.Nodes.Add(Move(Info));
                    }

                    for (CAnimStateTransition* Transition : AnimGraphOps::GetTransitions(Target.Canvas))
                    {
                        SAnimGraphTransitionInfo Info;
                        Info.FromNode = Transition->FromStateNodeID;
                        Info.ToNode   = Transition->ToStateNodeID;

                        FJsonValue Values;
                        if (Agent::WriteStruct(Transition->GetClass(), Transition, Values).IsValid())
                        {
                            Info.Values = Values.ToString();
                        }

                        Out.Transitions.Add(Move(Info));
                    }

                    return Agent::FToolResult::Ok(Lumina::Format("'{}' canvas has {} node(s), {} transition(s).",
                        Out.Name, Out.Nodes.Num(), Out.Transitions.Num()));
                });
        }

        Agent::FToolResult AddAnimGraphNode(const SAnimGraphNodeParams& In, const FAnimGraphTarget& Target, SAnimGraphNodeResult& Out)
        {
            CClass* NodeClass = NodeGraphOps::ResolveNodeType(Target.Canvas->GetClass(), FStringView(In.NodeType));
            if (NodeClass == nullptr)
            {
                return Agent::FToolResult::Error(Lumina::Format(
                    "This canvas takes no node type named '{}'. Use animgraph.list_node_types.", In.NodeType));
            }

            CEdGraphNode* Node = NodeGraphOps::AddNode(Target.Canvas, NodeClass, In.X, In.Y);
            if (Node == nullptr)
            {
                return Agent::FToolResult::Error("The graph refused to create that node.");
            }

            Out.Id = Node->GetNodeID();
            CollectPins(Node, Out.Pins);

            MarkAssetDirty(Target.Asset);

            return Agent::FToolResult::Ok(Lumina::Format("Added {} as node {}.", In.NodeType, Out.Id));
        }

        void RegisterAnimGraphNode(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAnimGraphNodeParams, SAnimGraphNodeResult>(
                Owner, "animgraph.node",
                "Add or remove a node on one animation graph canvas. Add returns the node's id and pins; X grows right "
                "and Y down. Remove takes its links, transitions and any canvas it owns.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAnimGraphNodeParams& In, SAnimGraphNodeResult& Out)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveAnimGraphCanvas(In.AnimGraph, In.GraphPath, EAnimGraphAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (In.Action == EAnimGraphNodeAction::Add)
                    {
                        return AddAnimGraphNode(In, Target, Out);
                    }

                    if (!RemoveGraphNode(Target.Canvas, In.Node, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    MarkAssetDirty(Target.Asset);

                    return Agent::FToolResult::Ok(Lumina::Format("Removed node {}.", In.Node));
                });
        }

        void RegisterAnimGraphLink(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SAnimGraphLinkParams>(
                Owner, "animgraph.link",
                "Connect wires an output pin into an input pin; between two states it creates a transition. "
                "Disconnect clears every link on Node.Pin, deleting any transitions it carried.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SAnimGraphLinkParams& In)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveAnimGraphCanvas(In.AnimGraph, In.GraphPath, EAnimGraphAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    if (In.Action == EAnimGraphLinkAction::Connect)
                    {
                        if (!ConnectGraphPins(Target.Canvas, In.FromNode, In.FromPin, In.ToNode, In.ToPin, Error))
                        {
                            return Agent::FToolResult::Error(Error);
                        }

                        MarkAssetDirty(Target.Asset);

                        return Agent::FToolResult::Ok(Lumina::Format("Connected {}.{} to {}.{}.",
                            In.FromNode, In.FromPin, In.ToNode, In.ToPin));
                    }

                    if (!DisconnectGraphPin(Target.Canvas, In.Node, In.Pin, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    MarkAssetDirty(Target.Asset);

                    return Agent::FToolResult::Ok(Lumina::Format("Disconnected {}.{}.", In.Node, In.Pin));
                });
        }

        void RegisterAnimGraphSetNodeProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetAnimGraphNodePropertyParams, SAnimGraphPropertyResult>(
                Owner, "animgraph.set_node_property",
                "Set one field on one node, such as a clip or a state's name. An unwired value pin's default lives "
                "in PinDefaults, as [{\"PinName\": \"Alpha\", \"Value\": 0.5}].",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetAnimGraphNodePropertyParams& In, SAnimGraphPropertyResult& Out)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveAnimGraphCanvas(In.AnimGraph, In.GraphPath, EAnimGraphAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    CEdGraphNode* Node = NodeGraphOps::FindNode(Target.Canvas, In.Node);
                    if (Node == nullptr)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("No node {} is on this canvas.", In.Node));
                    }

                    if (!SetAnimGraphPropertyFromJson(Node, In.Path, In.Value, Out, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    NodeGraphOps::NotifyNodeValuesChanged(Target.Canvas);
                    MarkAssetDirty(Target.Asset);

                    return Agent::FToolResult::Ok(Lumina::Format("Node {} {} is now {}.",
                        In.Node, In.Path, Out.Current));
                });
        }

        void RegisterAnimGraphSetTransitionProperty(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SSetAnimGraphTransitionPropertyParams, SAnimGraphPropertyResult>(
                Owner, "animgraph.set_transition_property",
                "Set one field on the transition between two states. Conditions takes a list of "
                "{ConditionSource, ParameterName, CurveName, Compare, CompareValue}.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SSetAnimGraphTransitionPropertyParams& In, SAnimGraphPropertyResult& Out)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveAnimGraphCanvas(In.AnimGraph, In.GraphPath, EAnimGraphAccess::Write, Target, Error))
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
                            "animgraph.link the two states first.", In.FromNode, In.ToNode));
                    }

                    if (!SetAnimGraphPropertyFromJson(Found, In.Path, In.Value, Out, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    MarkAssetDirty(Target.Asset);

                    return Agent::FToolResult::Ok(Lumina::Format("Transition {} -> {} {} is now {}.",
                        In.FromNode, In.ToNode, In.Path, Out.Current));
                });
        }

        void RegisterAnimGraphCompile(FStringView Owner)
        {
            Agent::FToolRegistry::Get().Register<SCompileAnimGraphParams, SAnimGraphCompileResult>(
                Owner, "animgraph.compile",
                "Compile an animation graph into the bytecode its entities run and report its errors. "
                "Save with assets.save afterwards.",
                Agent::EToolEffect::Mutating, Agent::EToolThread::GameThread,
                [](const SCompileAnimGraphParams& In, SAnimGraphCompileResult& Out)
                {
                    FAnimGraphTarget Target;
                    FString Error;
                    if (!ResolveAnimGraphCanvas(In.AnimGraph, {}, EAnimGraphAccess::Write, Target, Error))
                    {
                        return Agent::FToolResult::Error(Error);
                    }

                    FAnimationGraphCompiler Compiler;
                    Out.bSucceeded = AnimGraphOps::Compile(Target.Asset,
                        AnimGraphOps::FindOrCreateGraph(Target.Asset), Compiler);

                    for (const EdNodeGraph::FError& Item : Compiler.GetErrors())
                    {
                        Out.Errors.Add(DescribeAnimGraphCompileError(Item));
                    }

                    for (const EdNodeGraph::FError& Item : Compiler.GetWarnings())
                    {
                        Out.Warnings.Add(DescribeAnimGraphCompileError(Item));
                    }

                    if (!Out.bSucceeded)
                    {
                        return Agent::FToolResult::Error(Lumina::Format("Compile failed with {} error(s). {}",
                            Out.Errors.Num(), Out.Errors.IsEmpty() ? FString() : Out.Errors[0]));
                    }

                    MarkAssetDirty(Target.Asset);

                    return Agent::FToolResult::Ok(Lumina::Format("Compiled with {} warning(s).",
                        Out.Warnings.Num()));
                });
        }
    }

    using namespace MCPAnimGraphToolsInternal;

    void RegisterAnimGraphTools(FStringView Owner)
    {
        RegisterAnimGraphListNodeTypes(Owner);
        RegisterAnimGraphDescribe(Owner);
        RegisterAnimGraphNode(Owner);
        RegisterAnimGraphLink(Owner);
        RegisterAnimGraphSetNodeProperty(Owner);
        RegisterAnimGraphSetTransitionProperty(Owner);
        RegisterAnimGraphCompile(Owner);
    }
}
