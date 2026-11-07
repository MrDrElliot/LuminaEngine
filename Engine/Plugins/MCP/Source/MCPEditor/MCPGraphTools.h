#pragma once

#include "Containers/String.h"
#include "Containers/Vector.h"
#include "MCPToolTypes.h"

namespace Lumina
{
    class CEdGraphNode;
    class CEdNodeGraph;

    namespace MCP
    {
        MCPEDITOR_API void CollectPins(CEdGraphNode* Node, TVector<SGraphPinInfo>& Out);
        bool RemoveGraphNode(CEdNodeGraph* Graph, int64 NodeId, FString& OutError);
        bool ConnectGraphPins(CEdNodeGraph* Graph, int64 FromNode, const FString& FromPin, int64 ToNode, const FString& ToPin, FString& OutError);
        bool DisconnectGraphPin(CEdNodeGraph* Graph, int64 NodeId, const FString& PinName, FString& OutError);
    }
}
