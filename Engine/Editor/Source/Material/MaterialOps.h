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
    class CMaterial;
    class CMaterialNodeGraph;
}

namespace Lumina::MaterialOps
{
    // Node types a material graph will accept, discovered from reflection rather than a list.
    NODISCARD EDITOR_API TVector<CClass*> GetPlaceableNodeTypes();

    NODISCARD EDITOR_API CClass* ResolveNodeType(FStringView TypeName);

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

    NODISCARD EDITOR_API CEdGraphNode* FindOutputNode(CMaterialNodeGraph* Graph);

    // Name of the tool with this asset open, empty when nothing has it. A mutation underneath an
    // open tool desyncs the view it already built, so callers outside the editor refuse instead.
    NODISCARD EDITOR_API FString FindOpenEditorName(CObject* Asset);

    // The graph the material editor would open, created if the material has none yet.
    NODISCARD EDITOR_API CMaterialNodeGraph* FindOrCreateGraph(CMaterial* Material);
}
