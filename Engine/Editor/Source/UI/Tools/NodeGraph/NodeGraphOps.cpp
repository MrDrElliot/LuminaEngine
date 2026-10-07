#include "EditorPCH.h"
#include "UI/Tools/NodeGraph/NodeGraphOps.h"

#include "LuminaEditor.h"
#include "UI/EditorUI.h"
#include "UI/Tools/EditorTool.h"
#include "Containers/StringFormat.h"
#include "Core/Object/Class.h"
#include "UI/Tools/NodeGraph/EdGraphSchema.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "UI/Tools/NodeGraph/EdNodeGraphPin.h"
#include "UI/Tools/NodeGraph/GraphNodeRegistry.h"

namespace Lumina::NodeGraphOps
{
    TVector<CClass*> GetPlaceableNodeTypes(CClass* GraphClass)
    {
        TVector<CClass*> Types;
        if (GraphClass == nullptr)
        {
            return Types;
        }

        const THashSet<CClass*>& Classes = FGraphNodeRegistry::Get().GetNodesForGraphClass(GraphClass);
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

    CClass* ResolveNodeType(CClass* GraphClass, FStringView TypeName)
    {
        if (TypeName.empty())
        {
            return nullptr;
        }

        // Bound to a local because the getter returns by value, so end() must come from the same vector.
        const TVector<CClass*> Types = GetPlaceableNodeTypes(GraphClass);
        const auto It = Algo::FindIf(Types,
            [TypeName](CClass* Class) { return FStringView(Class->GetName().ToString()) == TypeName; });

        return It != Types.end() ? *It : nullptr;
    }

    void NotifyNodeValuesChanged(CEdNodeGraph* Graph)
    {
        if (Graph != nullptr)
        {
            Graph->CommitExternalNodeEdit("Set Node Property");
        }
    }

    CEdGraphNode* FindNode(CEdNodeGraph* Graph, int64 NodeId)
    {
        return Graph != nullptr ? Graph->FindNode(NodeId) : nullptr;
    }

    CEdNodeGraphPin* FindPin(CEdGraphNode* Node, FStringView PinName, ENodePinDirection Direction)
    {
        if (Node == nullptr)
        {
            return nullptr;
        }

        const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& Pins = Direction == ENodePinDirection::Input
            ? Node->GetInputPins()
            : Node->GetOutputPins();

        for (const TStrongObjectPtr<CEdNodeGraphPin>& Pin : Pins)
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

        const TVector<TStrongObjectPtr<CEdNodeGraphPin>>& Pins = Direction == ENodePinDirection::Input
            ? Node->GetInputPins()
            : Node->GetOutputPins();

        FString Names;
        for (const TStrongObjectPtr<CEdNodeGraphPin>& Pin : Pins)
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

    bool ConnectPins(CEdNodeGraph* Graph, CEdNodeGraphPin* Output, CEdNodeGraphPin* Input, FString& OutError)
    {
        if (Graph == nullptr)
        {
            OutError = "A connection needs a graph.";
            return false;
        }

        FNodeGraphEditScope Edit(Graph, "Connect Pins");
        return Graph->ConnectPins(Output, Input, &OutError);
    }

    bool DisconnectPin(CEdNodeGraph* Graph, CEdNodeGraphPin* Pin, FString& OutError)
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

        FNodeGraphEditScope Edit(Graph, "Break Links");
        Graph->BreakPinLinks(Pin);
        return true;
    }

    CEdGraphNode* AddNode(CEdNodeGraph* Graph, CClass* NodeClass, float X, float Y)
    {
        if (Graph == nullptr)
        {
            return nullptr;
        }

        FNodeGraphEditScope Edit(Graph, "Add Node");
        return Graph->SpawnNode(NodeClass, X, Y);
    }

    bool RemoveNode(CEdNodeGraph* Graph, CEdGraphNode* Node, FString& OutError)
    {
        if (Graph == nullptr || Node == nullptr)
        {
            OutError = "A removal needs a graph and a node.";
            return false;
        }

        FNodeGraphEditScope Edit(Graph, "Delete Node");
        return Graph->RemoveNodes({ Node }, &OutError);
    }

    bool MoveNode(CEdNodeGraph* Graph, CEdGraphNode* Node, float X, float Y)
    {
        if (Graph == nullptr || Node == nullptr)
        {
            return false;
        }

        FNodeGraphEditScope Edit(Graph, "Move Node");
        Graph->MoveNode(Node, X, Y);
        return true;
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
        if (Tool == nullptr)
        {
            return FString();
        }

        // The window name leads with an icon glyph and ends in an ImGui id, so only the id between is retypeable.
        const FString WindowName(Tool->GetToolName().c_str());
        const size_t Hash = WindowName.find("###");
        const FString Label = WindowName.substr(0, Hash);
        size_t Start = 0;
        while (Start < Label.size() && ((uint8)Label[Start] >= 0x80 || Label[Start] == ' '))
        {
            ++Start;
        }
        const FString Id = Hash == FString::npos ? WindowName : WindowName.substr(Hash + 3);
        return Lumina::Format("the {} editor (tab {})", Label.substr(Start), Id);
    }
}
