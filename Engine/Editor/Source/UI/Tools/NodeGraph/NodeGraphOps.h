#pragma once

#include "Containers/String.h"
#include "Containers/StringView.h"
#include "Containers/Vector.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"

namespace Lumina
{
    class CClass;
    class CEdNodeGraph;
    class CEdNodeGraphPin;
}

namespace Lumina::NodeGraphOps
{
    // Node types a canvas of this class accepts, discovered from reflection rather than a list.
    NODISCARD EDITOR_API TVector<CClass*> GetPlaceableNodeTypes(CClass* GraphClass);

    NODISCARD EDITOR_API CClass* ResolveNodeType(CClass* GraphClass, FStringView TypeName);

    // Never calls BuildNode, which appends pins rather than rebuilding them.
    EDITOR_API void NotifyNodeValuesChanged(CEdNodeGraph* Graph);

    // Every mutation here leaves the serialized connection list rebuilt, which a raw pin edit does not.
    NODISCARD EDITOR_API CEdGraphNode* FindNode(CEdNodeGraph* Graph, int64 NodeId);

    NODISCARD EDITOR_API CEdNodeGraphPin* FindPin(CEdGraphNode* Node, FStringView PinName,
        ENodePinDirection Direction);

    // Names every pin on Node in the given direction, for an error that can be acted on.
    NODISCARD EDITOR_API FString DescribePinNames(CEdGraphNode* Node, ENodePinDirection Direction);

    // Applies the schema and the single-link input rule the interactive editor applies.
    NODISCARD EDITOR_API bool ConnectPins(CEdNodeGraph* Graph, CEdNodeGraphPin* Output,
        CEdNodeGraphPin* Input, FString& OutError);

    NODISCARD EDITOR_API bool DisconnectPin(CEdNodeGraph* Graph, CEdNodeGraphPin* Pin, FString& OutError);

    NODISCARD EDITOR_API CEdGraphNode* AddNode(CEdNodeGraph* Graph, CClass* NodeClass, float X, float Y);

    // Refuses a node the graph keeps for itself, such as the output node a graph compiles from.
    NODISCARD EDITOR_API bool RemoveNode(CEdNodeGraph* Graph, CEdGraphNode* Node, FString& OutError);

    // Name of the tool with this asset open, empty when nothing has it, so a caller outside the editor can refuse.
    NODISCARD EDITOR_API FString FindOpenEditorName(CObject* Asset);
}
