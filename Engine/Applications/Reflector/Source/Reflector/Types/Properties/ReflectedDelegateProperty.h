#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ReflectedProperty.h"
#include "Reflector/Clang/Utils.h"
#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"

namespace Lumina
{
    // A TScriptDelegate<Args...> event member; the arguments follow it as reflected sub-properties.
    class FReflectedDelegateProperty : public FReflectedProperty
    {
    public:

        const char* GetTypeName() override { return "Delegate"; }
        const char* GetPropertyParamType() const override { return "FDelegatePropertyParams"; }
        std::string_view GetLuaType() override { return "function"; }

        void AppendDefinition(Reflection::FCodeWriter& Writer) const override;

        uint16_t NumArgs = 0;

        // Argument type spellings in declaration order, for the C# accessor.
        std::vector<std::string> ArgTypeNames;
    };
}
