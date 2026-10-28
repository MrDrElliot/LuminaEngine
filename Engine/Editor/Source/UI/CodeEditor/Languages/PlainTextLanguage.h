#pragma once

#include "UI/CodeEditor/CodeLanguage.h"

namespace Lumina
{
    // Text with no grammar behind it, such as JSON, Markdown or config files, colored only for strings, numbers and brackets.
    class EDITOR_API FPlainTextLanguage : public FCodeLanguage
    {
    public:

        explicit FPlainTextLanguage(FStringView InName, FStringView InLineComment = {}) : Name(InName), LineComment(InLineComment) {}

        FStringView GetName() const override { return Name; }
        uint32 Tokenize(FStringView Line, uint32 State, ECodeToken* OutTokens) const override;
        FCodeCommentSyntax GetCommentSyntax(uint32 State) const override { return { LineComment, {}, {} }; }

    private:

        FString Name;
        FStringView LineComment;
    };
}
