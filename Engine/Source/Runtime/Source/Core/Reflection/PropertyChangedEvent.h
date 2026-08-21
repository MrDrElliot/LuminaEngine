#pragma once
#include "Containers/Name.h"


namespace Lumina
{
    class CStruct;
    class FProperty;
}

namespace Lumina
{

    struct FPropertyChangedEvent
    {
        CStruct*    OuterType;
        FProperty*  Property;
        FName       PropertyName;

        // True only on the dispatch the struct edit hooks fire for; interim drag and keystroke ops are false.
        bool        bIsCommit = false;

        // Byte offset of the edited value inside OuterType; -1 when it sits in a separate heap block.
        int64       ValueOffset = -1;
    };

    
}
