#pragma once
#include <clang-c/Index.h>
#include <string>
#include "Reflector/Public/PropertyFlags.h"


namespace Lumina
{
    struct FFieldInfo
    {
        CXType                              Type;
        // Carried through sub-field creation so inner property diagnostics point at the user-facing source line.
        CXCursor                            OwningCursor = clang_getNullCursor();
        EPropertyTypeFlags                  Flags;
        EPropertyFlags                      PropertyFlags = EPropertyFlags::None;
        std::string                       Name;
        std::string                       TypeName;
        std::string                       RawFieldType;
        // A function argument's C++ default expression, verbatim, empty when it has none.
        std::string                       DefaultValue;
    };
}
