#pragma once

namespace Lumina
{
    class CEdNodeGraphPin;
    class CEdGraphNode;
}

namespace Lumina
{
    // Per-graph connection rules that derived graphs customize by returning a subclass from CEdNodeGraph::GetSchema.
    struct EDITOR_API FEdGraphSchema
    {
        virtual ~FEdGraphSchema() = default;

        virtual bool CanCreateConnection(CEdNodeGraphPin* From, CEdNodeGraphPin* To) const;
        
        virtual bool AllowsMultipleConnections(CEdNodeGraphPin* InputPin) const;
    };

    // Default schema used by CEdNodeGraph when no derived schema is supplied.
    EDITOR_API const FEdGraphSchema& GetDefaultEdGraphSchema();
}
