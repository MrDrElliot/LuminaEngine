#pragma once

#include "Containers/Array.h"
#include "Scripting/ScriptExports.h"

namespace Lumina
{
    class CStruct;
}

namespace Lumina::Scripting
{
    // Transcodes a CScriptStruct value buffer to/from FScriptPropertyEntry by walking the struct's FProperties.

    RUNTIME_API void ReadStructToValues(const CStruct* Layout, const void* Buffer, TVector<FScriptPropertyEntry>& OutValues);
    RUNTIME_API void WriteValuesToStruct(const CStruct* Layout, void* Buffer, const TVector<FScriptPropertyEntry>& Values);
}
