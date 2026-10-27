#pragma once

#include "UI/CodeEditor/CodeLanguage.h"

namespace Lumina
{
    // C# keywords, types, strings and snippets, the base a script editor can layer project symbols onto.
    class EDITOR_API FCSharpLanguage : public FCLikeLanguage
    {
    public:

        FCSharpLanguage();

        FStringView GetName() const override { return "C#"; }
    };
}
