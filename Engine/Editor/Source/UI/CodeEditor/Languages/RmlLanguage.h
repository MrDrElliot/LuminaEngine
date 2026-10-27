#pragma once

#include "Containers/Function.h"
#include "UI/CodeEditor/CodeLanguage.h"

namespace Lumina
{
    namespace RmlKnowledge
    {
        struct FRmlProperty;
        struct FRmlFunction;
    }

    // RML markup with its <style> blocks and style attributes, or a bare RCSS stylesheet.
    class EDITOR_API FRmlLanguage : public FCodeLanguage
    {
    public:

        explicit FRmlLanguage(bool bInStylesheet);

        FStringView GetName() const override { return bStylesheet ? "RCSS" : "RML"; }
        uint32 GetInitialState() const override;
        uint32 Tokenize(FStringView Line, uint32 State, ECodeToken* OutTokens) const override;

        bool IsWordChar(char C) const override;
        FCodeCommentSyntax GetCommentSyntax(uint32 State) const override;
        bool ShouldIndentAfter(FStringView LineBeforeCaret, uint32 CaretState) const override;
        bool ShouldOutdentOnType(FStringView LineThroughCaret, char Typed) const override;
        FString GetAutoInsert(const FCodeDocument& Document, FCodePosition Caret, uint32 CaretState, char Typed) const override;

        bool IsCompletionTrigger(char Typed, FStringView LinePrefix, uint32 CaretState) const override;
        void GetCompletions(const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out) const override;
        bool GetHover(const FCodeDocument& Document, FCodePosition Position, FCodeHover& Out) const override;
        bool GetDiagnostics(const FCodeDocument& Document, TVector<FCodeDiagnostic>& Out) const override;
        void GetColorSpans(FStringView Line, const ECodeToken* Tokens, TVector<FCodeColorSpan>& Out) const override;

        // Reads a stylesheet a <link> names, so the classes and ids it declares complete in this document.
        void SetStylesheetResolver(TFunction<bool(FStringView Href, FString& OutText)> Resolver) { StylesheetResolver = Move(Resolver); }

        struct FOpenElement
        {
            FString       Name;
            FCodePosition Position;
        };

        // The elements still open at Limit, outermost first, reporting misnested and stray closing tags to Problems.
        void BuildElementStack(const FCodeDocument& Document, FCodePosition Limit, TVector<FOpenElement>& OutStack, TVector<FCodeDiagnostic>* Problems) const;

    protected:

        bool FindSignature(FStringView Function, FCodeSignature& Out) const override;

    private:

        int32 TokenizeText(FStringView Line, int32 i, uint32& State, ECodeToken* Out) const;
        int32 TokenizeTag(FStringView Line, int32 i, uint32& State, ECodeToken* Out) const;
        int32 TokenizeAttributeValue(FStringView Line, int32 i, uint32& State, ECodeToken* Out) const;
        int32 TokenizeStylesheet(FStringView Line, int32 i, uint32& State, ECodeToken* Out) const;
        int32 TokenizeDeclaration(FStringView Line, int32 i, int32 End, uint32& State, ECodeToken* Out) const;

        void AddCssCompletions(const FCodeCompletionRequest& Request, bool bSelector, TVector<FCodeCompletionItem>& Out) const;
        void AddValueCompletions(const FCodeCompletionRequest& Request, FStringView Property, TVector<FCodeCompletionItem>& Out) const;
        void AddAttributeCompletions(const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out) const;
        void AddAttributeValueCompletions(const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out) const;
        void CollectSelectors(const FCodeDocument& Document, TVector<FString>& OutClasses, TVector<FString>& OutIds) const;
        void CollectAtRuleNames(const FCodeDocument& Document, FStringView AtRule, TVector<FString>& Out) const;
        FString FindTagName(const FCodeDocument& Document, FCodePosition Position) const;

        const RmlKnowledge::FRmlProperty* FindProperty(FStringView Name) const;

        bool bStylesheet = false;
        THashMap<FStringView, const RmlKnowledge::FRmlProperty*> PropertyMap;
        THashMap<FStringView, const RmlKnowledge::FRmlFunction*> FunctionMap;
        THashMap<FStringView, uint32>                         ColorMap;
        TFunction<bool(FStringView Href, FString& OutText)>  StylesheetResolver;
    };
}
