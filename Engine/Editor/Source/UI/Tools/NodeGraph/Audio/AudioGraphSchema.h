#pragma once

#include "UI/Tools/NodeGraph/EdGraphSchema.h"

namespace Lumina
{
    /** Audio graph wires connect matching value kinds only, with the converter nodes as the bridge. */
    struct FAudioGraphSchema : public FEdGraphSchema
    {
        bool CanCreateConnection(CEdNodeGraphPin* From, CEdNodeGraphPin* To) const override;
    };

    const FAudioGraphSchema& GetAudioGraphSchema();
}
