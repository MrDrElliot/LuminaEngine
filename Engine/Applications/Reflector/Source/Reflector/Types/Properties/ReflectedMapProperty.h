#pragma once

#include "ReflectedProperty.h"

namespace Lumina
{
    // A THashMap<K, V>, or any map that normalizes onto one, such as TInlineHashMap.
    class FReflectedMapProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() override { return nullptr; }

        std::string KeyTypeName;
        std::string ValueTypeName;
    };
}
