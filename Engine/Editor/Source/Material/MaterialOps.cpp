#include "EditorPCH.h"
#include "Material/MaterialOps.h"

#include "Assets/AssetTypes/Material/Material.h"
#include "LuminaEditor.h"
#include "UI/EditorUI.h"
#include "UI/Tools/EditorTool.h"
#include "Containers/StringFormat.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/Package/Package.h"
#include "UI/Tools/NodeGraph/EdGraphSchema.h"
#include "UI/Tools/NodeGraph/GraphNodeRegistry.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "UI/Tools/NodeGraph/Material/MaterialNodeGraph.h"
#include "UI/Tools/NodeGraph/Material/Nodes/MaterialOutputNode.h"

namespace Lumina::MaterialOps
{
    namespace
    {
        void DisconnectEverything(CEdNodeGraphPin* Pin)
        {
            if (Pin == nullptr || !Pin->HasConnection())
            {
                return;
            }

            const TVector<CEdNodeGraphPin*> Existing = Pin->GetConnections();
            for (CEdNodeGraphPin* Other : Existing)
            {
                Other->DisconnectFrom(Pin);
            }

            Pin->ClearConnections();
        }
    }

    TVector<CClass*> GetPlaceableNodeTypes()
    {
        const THashSet<CClass*>& Classes =
            FGraphNodeRegistry::Get().GetNodesForGraphClass(CMaterialNodeGraph::StaticClass());

        TVector<CClass*> Types;
        Types.reserve(Classes.size());

        for (CClass* Class : Classes)
        {
            if (Class != nullptr)
            {
                Types.push_back(Class);
            }
        }

        return Types;
    }

    CClass* ResolveNodeType(FStringView TypeName)
    {
        if (TypeName.empty())
        {
            return nullptr;
        }

        // Bound to a local because the getter returns by value, so end() must come from the same vector.
        const TVector<CClass*> Types = GetPlaceableNodeTypes();
        const auto It = Algo::FindIf(Types,
            [TypeName](CClass* Class) { return FStringView(Class->GetName().ToString()) == TypeName; });

        return It != Types.end() ? *It : nullptr;
    }

    void NotifyNodeValuesChanged(CMaterialNodeGraph* Graph)
    {
        if (Graph != nullptr)
        {
            Graph->ValidateGraph();
        }
    }

    CEdGraphNode* FindNode(CMaterialNodeGraph* Graph, int64 NodeId)
    {
        return Graph != nullptr ? Graph->FindNode(NodeId) : nullptr;
    }

    CEdNodeGraphPin* FindPin(CEdGraphNode* Node, FStringView PinName, ENodePinDirection Direction)
    {
        if (Node == nullptr)
        {
            return nullptr;
        }

        const TVector<TObjectPtr<CEdNodeGraphPin>>& Pins = Direction == ENodePinDirection::Input
            ? Node->GetInputPins()
            : Node->GetOutputPins();

        for (const TObjectPtr<CEdNodeGraphPin>& Pin : Pins)
        {
            if (Pin.IsValid() && FStringView(Pin->GetPinName()) == PinName)
            {
                return Pin.Get();
            }
        }

        return nullptr;
    }

    FString DescribePinNames(CEdGraphNode* Node, ENodePinDirection Direction)
    {
        if (Node == nullptr)
        {
            return FString();
        }

        const TVector<TObjectPtr<CEdNodeGraphPin>>& Pins = Direction == ENodePinDirection::Input
            ? Node->GetInputPins()
            : Node->GetOutputPins();

        FString Names;
        for (const TObjectPtr<CEdNodeGraphPin>& Pin : Pins)
        {
            if (!Pin.IsValid())
            {
                continue;
            }

            if (!Names.empty())
            {
                Names.append(", ");
            }

            Names.append(Pin->GetPinName());
        }

        return Names.empty() ? FString("none") : Names;
    }

    bool ConnectPins(CMaterialNodeGraph* Graph, CEdNodeGraphPin* Output, CEdNodeGraphPin* Input, FString& OutError)
    {
        if (Graph == nullptr || Output == nullptr || Input == nullptr)
        {
            OutError = "A connection needs a graph and both pins.";
            return false;
        }

        if (Output->bInputPin || !Input->bInputPin)
        {
            OutError = "A connection runs from an output pin to an input pin.";
            return false;
        }

        const FEdGraphSchema& Schema = Graph->GetSchema();

        if (!Schema.CanCreateConnection(Output, Input))
        {
            OutError = Lumina::Format("'{}' and '{}' cannot be connected, since the graph refuses that pairing.",
                Output->GetPinName(), Input->GetPinName());
            return false;
        }

        // An input holding a second link is unrepresentable in the editor and loses one on reload.
        if (Input->HasConnection() && !Schema.AllowsMultipleConnections(Input))
        {
            DisconnectEverything(Input);
        }

        Output->AddConnection(Input);
        Input->AddConnection(Output);

        Graph->ValidateGraph();

        return true;
    }

    bool DisconnectPin(CMaterialNodeGraph* Graph, CEdNodeGraphPin* Pin, FString& OutError)
    {
        if (Graph == nullptr || Pin == nullptr)
        {
            OutError = "A disconnect needs a graph and a pin.";
            return false;
        }

        if (!Pin->HasConnection())
        {
            OutError = Lumina::Format("'{}' is not connected to anything.", Pin->GetPinName());
            return false;
        }

        DisconnectEverything(Pin);

        Graph->ValidateGraph();

        return true;
    }

    CEdGraphNode* AddNode(CMaterialNodeGraph* Graph, CClass* NodeClass, float X, float Y)
    {
        if (Graph == nullptr || NodeClass == nullptr)
        {
            return nullptr;
        }

        CEdGraphNode* Node = Graph->CreateNode(NodeClass);
        if (Node != nullptr)
        {
            Node->SetGridPos(X, Y);
            Graph->ValidateGraph();
        }

        return Node;
    }

    bool RemoveNode(CMaterialNodeGraph* Graph, CEdGraphNode* Node, FString& OutError)
    {
        if (Graph == nullptr || Node == nullptr)
        {
            OutError = "A removal needs a graph and a node.";
            return false;
        }

        if (Node->IsA<CMaterialOutputNode>())
        {
            OutError = "The output node is what the graph compiles from, so it cannot be removed.";
            return false;
        }

        auto Found = Graph->Nodes.end();
        for (auto It = Graph->Nodes.begin(); It != Graph->Nodes.end(); ++It)
        {
            if (It->Get() == Node)
            {
                Found = It;
                break;
            }
        }

        if (Found == Graph->Nodes.end())
        {
            OutError = "That node is not in this graph.";
            return false;
        }

        for (CEdNodeGraphPin* Pin : Node->GetInputPins())
        {
            DisconnectEverything(Pin);
        }

        for (CEdNodeGraphPin* Pin : Node->GetOutputPins())
        {
            DisconnectEverything(Pin);
        }

        Graph->Nodes.erase(Found);
        Node->ConditionalBeginDestroy();

        Graph->ValidateGraph();

        return true;
    }

    CEdGraphNode* FindOutputNode(CMaterialNodeGraph* Graph)
    {
        if (Graph == nullptr)
        {
            return nullptr;
        }

        for (const TObjectPtr<CEdGraphNode>& Node : Graph->Nodes)
        {
            if (Node.IsValid() && Node->IsA<CMaterialOutputNode>())
            {
                return Node.Get();
            }
        }

        return nullptr;
    }

    FString FindOpenEditorName(CObject* Asset)
    {
        if (GEditorEngine == nullptr || Asset == nullptr)
        {
            return FString();
        }

        FEditorUI* UI = static_cast<FEditorUI*>(GEditorEngine->GetDevelopmentToolsUI());
        if (UI == nullptr)
        {
            return FString();
        }

        FEditorTool* Tool = UI->FindAssetEditor(Asset);
        return Tool != nullptr ? FString(Tool->GetToolName().c_str()) : FString();
    }

    CMaterialNodeGraph* FindOrCreateGraph(CMaterial* Material)
    {
        if (Material == nullptr)
        {
            return nullptr;
        }

        CPackage* Package = Material->GetPackage();
        if (Package == nullptr)
        {
            return nullptr;
        }

        // The name the material editor looks for, so an agent-built graph opens as the same graph.
        const FString GraphName = "AssetMaterialGraph";

        if (CMaterialNodeGraph* Existing = Cast<CMaterialNodeGraph>(Package->LoadObjectByName(GraphName)))
        {
            return Existing;
        }

        CMaterialNodeGraph* Graph = NewObject<CMaterialNodeGraph>(Package, GraphName);
        Graph->SetMaterial(Material);
        Graph->CreateNode(CMaterialOutputNode::StaticClass());

        return Graph;
    }
}
