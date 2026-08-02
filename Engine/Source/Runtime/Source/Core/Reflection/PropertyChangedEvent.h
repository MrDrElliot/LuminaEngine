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
    };

    
}
