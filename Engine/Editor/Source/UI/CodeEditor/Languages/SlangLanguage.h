#pragma once

#include "UI/CodeEditor/CodeLanguage.h"

namespace Lumina
{
    // Slang as written in a material's Custom Slang node, with the locals the material pixel pass puts in scope.
    class EDITOR_API FSlangLanguage : public FCLikeLanguage
    {
    public:

        FSlangLanguage();

        FStringView GetName() const override { return "Slang"; }
    };
}
