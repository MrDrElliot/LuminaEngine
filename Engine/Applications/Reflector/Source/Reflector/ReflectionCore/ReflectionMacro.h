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
        FReflectionMacro(const FReflectedHeader* InHeader, const CXCursor& Cursor, const CXSourceRange& Range, EReflectionMacro InType);


        EReflectionMacro        Type = EReflectionMacro::Size;
        const FReflectedHeader* Header = nullptr;
        uint32_t                LineNumber = 0;

        // Where the invocation closes, which is a later line whenever the argument list wraps.
        uint32_t                EndLineNumber = 0;

        int32_t                 Position = -1;

        // A macro binds to one declaration, so a consumed entry stays in the pool but stops matching.
        bool                    bConsumed = false;

        std::string             MacroContents;
    };
}
