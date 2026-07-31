#pragma once

#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CStruct;
}

namespace Lumina::DataTableUI
{
    /** Searchable combo over every reflected struct deriving from SDataTableRowBase. Returns the newly
     *  picked struct and sets bOutChanged, or returns Current when untouched.
     *
     *  SDataTableRowBase itself is excluded: it has no fields, so a table of it would be a list of
     *  names and nothing else. */
    CStruct* DrawRowStructPicker(const char* StrId, CStruct* Current, bool& bOutChanged);
}
