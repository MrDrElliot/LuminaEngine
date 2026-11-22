#pragma once

#include "UI/Tools/NodeGraph/EdGraphSchema.h"

namespace Lumina
{
    // Connection rules that let a pin connect only to another of the same EAnimPinType.
    struct FAnimGraphSchema : public FEdGraphSchema
    {
        bool CanCreateConnection(CEdNodeGraphPin* From, CEdNodeGraphPin* To) const override;

        // Honors bAllowMultipleConnections so the State Machine's States pin can collect every state.
        bool AllowsMultipleConnections(CEdNodeGraphPin* InputPin) const override;
    };

    const FAnimGraphSchema& GetAnimGraphSchema();
}
