#pragma once

#include "Containers/String.h"
#include "Containers/StringView.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina::Agent
{
    // Where a dotted path landed, which is one property and the instance it belongs to.
    struct FResolvedProperty
    {
        FProperty* Property = nullptr;
        void*      ValuePtr = nullptr;

        NODISCARD bool IsValid() const { return Property != nullptr && ValuePtr != nullptr; }
    };

    // Walks a path such as "LightColor.X" or "Materials[2].Slot" from a struct instance to one value.
    NODISCARD EDITOR_API bool ResolvePropertyPath(CStruct* Root, void* RootData, FStringView Path,
        FResolvedProperty& Out, FString& OutError);
}
