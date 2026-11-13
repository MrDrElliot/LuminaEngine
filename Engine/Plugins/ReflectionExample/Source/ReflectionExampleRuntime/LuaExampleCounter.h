#pragma once

#include "Containers/String.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectMacros.h"
#include "LuaExampleCounter.generated.h"

namespace Lumina
{
    // LuaExport is the specifier the plugin's generator claims, so this class gets Lua binding tables.
    REFLECT(LuaExport)
    class REFLECTIONEXAMPLERUNTIME_API CLuaExampleCounter : public CObject
    {
        GENERATED_BODY()

    public:

        FUNCTION()
        int32 Add(int32 Amount) { return Count += Amount; }

        PROPERTY()
        int32 Count = 0;

        PROPERTY()
        FString Label;
    };
}
