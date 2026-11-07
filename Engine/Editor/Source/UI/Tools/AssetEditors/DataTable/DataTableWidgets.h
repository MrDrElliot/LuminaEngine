#pragma once

#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CStruct;
}

namespace Lumina::DataTableUI
{
    // Struct combo over structs deriving from the fieldless SDataTableRowBase, or a how-to hint while none exist.
    CStruct* DrawRowStructPicker(const char* StrId, CStruct* Current, bool& bOutChanged);
}
