#pragma once
#include <clang-c/Index.h>

#include "ReflectedHeader.h"
#include "Reflector/ReflectionConfig.h"


namespace Lumina::Reflection
{
    class FReflectionMacro
    {
    public:

        FReflectionMacro() = default;
        FReflectionMacro(const std::string& HeaderPath, const CXCursor& Cursor, const CXSourceRange& Range, EReflectionMacro InType);


        EReflectionMacro        Type;
        std::string           HeaderID;
        uint32_t                LineNumber = 0;
        int32_t                 Position = -1;

        std::string           MacroContents;
    };
}
