#pragma once

#include "Core/Object/ObjectMacros.h"
#include "MouseCodes.generated.h"

namespace Lumina
{
    REFLECT()
    enum class EMouseMode : uint8
    {
        Hidden,
        Normal,
        Captured,
    };
    
    REFLECT()
    enum class EMouseKey : uint16
    {
        Button0                = 0,
        Button1                = 1,
        Button2                = 2,
        Button3                = 3,
        Button4                = 4,
        Button5                = 5,
        Button6                = 6,
        Button7                = 7,
        Scroll                 = 8,

        ButtonLast             = Button7,
        ButtonLeft             = Button0,
        ButtonRight            = Button1,
        ButtonMiddle           = Button2,

        Num = 9,
    };
}
