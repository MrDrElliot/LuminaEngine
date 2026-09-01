#pragma once

#include <cstdint>

namespace Lumina::Reflection
{
    enum class EReflectionMacro : uint8_t
    {
        Property,
        Function,
        Reflect,
        GeneratedBody,
        ScriptExport,
        Size,
    };

    inline constexpr const char* ReflectionEnumToString(EReflectionMacro Macro)
    {
        switch (Macro)
        {
            case EReflectionMacro::Property:            return "PROPERTY";
            case EReflectionMacro::Function:            return "FUNCTION";
            case EReflectionMacro::Reflect:             return "REFLECT";
            case EReflectionMacro::GeneratedBody:       return "GENERATED_BODY";
            case EReflectionMacro::ScriptExport:        return "SCRIPT_EXPORT";
            default:                                    return "NONE";
        }
    }


    constexpr static const char* GEngineNamespace = "Lumina";
    
    
}
