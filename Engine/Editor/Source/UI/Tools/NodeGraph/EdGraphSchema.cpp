#include "EdGraphSchema.h"
#include "EdGraphNode.h"
#include "EdNodeGraphPin.h"

namespace Lumina
{
    bool FEdGraphSchema::CanCreateConnection(CEdNodeGraphPin* From, CEdNodeGraphPin* To) const
    {
        if (From == nullptr || To == nullptr)
        {
            return false;
        }

        if (From == To || From->GetOwningNode() == To->GetOwningNode())
        {
            return false;
        }

        return From->IsExecutionPin() == To->IsExecutionPin();
    }

    bool FEdGraphSchema::AllowsMultipleConnections(CEdNodeGraphPin* InputPin) const
    {
        return false;
    }

    const FEdGraphSchema& GetDefaultEdGraphSchema()
    {
        static FEdGraphSchema DefaultSchema;
        return DefaultSchema;
    }
}
