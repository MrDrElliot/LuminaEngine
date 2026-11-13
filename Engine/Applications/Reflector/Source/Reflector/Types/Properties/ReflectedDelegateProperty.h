#pragma once

#include <string>
#include <vector>
#include "ReflectedProperty.h"

namespace Lumina
{
    // A TScriptDelegate<Args...> event member.
    class FReflectedDelegateProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() const override { return "Delegate"; }

        // Argument type spellings in declaration order, for the C# accessor.
        std::vector<std::string> ArgTypeNames;
    };
}
