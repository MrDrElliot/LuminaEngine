#pragma once

#include "CodeDocument.h"
#include "Containers/HashTable.h"
#include "Containers/Span.h"

namespace Lumina
{
    enum class ECodeCompletionKind : uint8
    {
        Text,
        Keyword,
        Type,
        Function,
        Variable,
        Field,
        Property,
        Value,
        Unit,
        Color,
        Element,
        Attribute,
        Selector,
        Event,
        Snippet,
        Module,
    };

    struct FCodeCompletionItem
    {
        FString             Label;
        // Snippet syntax with numbered tab stops, placeholders in braces, and $0 for the final caret. Empty inserts the label.
        FString             InsertText;
        FString             Detail;
        FString             Documentation;
        ECodeCompletionKind Kind = ECodeCompletionKind::Text;
        int32               SortBoost = 0;
        // An ImU32 shown beside a color value, zero for none.
        uint32              Swatch = 0;
        // Opens completion again after inserting, for a property whose value comes next.
        bool                bRetrigger = false;
    };

    struct FCodeCompletionRequest
    {
        const FCodeDocument* Document = nullptr;
        FCodePosition        Caret;
        FCodePosition        WordStart;
        FStringView          Word;
        FStringView          LinePrefix;
        uint32               CaretState = 0;
        char                 TriggerChar = 0;
        bool                 bExplicit = false;
    };

    enum class ECodeSeverity : uint8
    {
        Error,
        Warning,
        Info,
    };

    struct FCodeDiagnostic
    {
        FCodeRange    Range;
        FString       Message;
        ECodeSeverity Severity = ECodeSeverity::Error;
    };

    struct FCodeHover
    {
        FCodeRange Range;
        FString    Title;
        FString    Body;
        uint32     Swatch = 0;
    };

    struct FCodeSignature
    {
        struct FSpan
        {
            int32 Start = 0;
            int32 End = 0;
        };

        FString        Label;
        FString        Documentation;
        TVector<FSpan> Parameters;
        int32          ActiveParameter = 0;
        // CSS functions such as gradient() take space separated arguments rather than a comma list.
        bool           bSpaceSeparated = false;
    };

    struct FCodeColorSpan
    {
        int32  Start = 0;
        int32  End = 0;
        uint32 Color = 0;
    };

    struct FCodeLocation
    {
        // A disk path, or empty for the document being edited.
        FString       Path;
        FCodePosition Position;
    };

    struct FCodeCommentSyntax
    {
        FStringView Line;
        FStringView BlockOpen;
        FStringView BlockClose;
    };

    class EDITOR_API FCodeLanguage
    {
    public:

        virtual ~FCodeLanguage() = default;

        virtual FStringView GetName() const = 0;
        virtual uint32 GetInitialState() const { return 0; }

        // Colors one line from the state the previous line ended in, one token per byte, and returns the state it ends in.
        virtual uint32 Tokenize(FStringView Line, uint32 State, ECodeToken* OutTokens) const = 0;

        virtual bool IsWordChar(char C) const;
        virtual FCodeCommentSyntax GetCommentSyntax(uint32 State) const;
        virtual FStringView GetAutoClosePairs() const { return "()[]{}\"\"''"; }

        virtual bool ShouldIndentAfter(FStringView LineBeforeCaret, uint32 CaretState) const;
        virtual bool ShouldOutdentOnType(FStringView LineThroughCaret, char Typed) const;

        // A snippet to insert at the caret once Typed has gone in, empty for none.
        virtual FString GetAutoInsert(const FCodeDocument& Document, FCodePosition Caret, uint32 CaretState, char Typed) const { return {}; }

        virtual bool IsCompletionTrigger(char Typed, FStringView LinePrefix, uint32 CaretState) const { return false; }
        virtual void GetCompletions(const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out) const {}
        virtual bool GetHover(const FCodeDocument& Document, FCodePosition Position, FCodeHover& Out) const { return false; }
        // False when the answer is not ready yet, so the editor asks again shortly.
        virtual bool GetDiagnostics(const FCodeDocument& Document, TVector<FCodeDiagnostic>& Out) const { return true; }
        virtual void GetColorSpans(FStringView Line, const ECodeToken* Tokens, TVector<FCodeColorSpan>& Out) const {}
        virtual bool FindDefinition(const FCodeDocument& Document, FCodePosition Position, FCodeLocation& Out) const { return false; }

        // Finds the call around Caret and the argument the caret sits in.
        virtual bool GetSignature(const FCodeDocument& Document, FCodePosition Caret, FCodeSignature& Out) const;

    protected:

        virtual bool FindSignature(FStringView Function, FCodeSignature& Out) const { return false; }
    };

    namespace CodeLanguageUtil
    {
        EDITOR_API void ParseSignatureParameters(FCodeSignature& Signature);
        EDITOR_API FCodeRange WordRangeAt(const FCodeDocument& Document, const FCodeLanguage& Language, FCodePosition Position);
        EDITOR_API void CollectDocumentWords(const FCodeDocument& Document, const FCodeLanguage& Language, FCodePosition Exclude, int32 MaxWords, TVector<FString>& Out);
        EDITOR_API bool ParseHexColor(FStringView Hex, uint32& OutColor);
        EDITOR_API FString FormatHexColor(uint32 Color, bool bWithAlpha);
    }

    // A symbol the C family languages know, with a call signature when it is a function.
    struct FCodeSymbol
    {
        const char* Name = "";
        const char* Signature = "";
        const char* Documentation = "";
    };

    // The C family tokenizer and completion, configured by keyword and symbol tables, which Slang and C# build on.
    class EDITOR_API FCLikeLanguage : public FCodeLanguage
    {
    public:

        uint32 Tokenize(FStringView Line, uint32 State, ECodeToken* OutTokens) const override;
        FCodeCommentSyntax GetCommentSyntax(uint32 State) const override { return { "//", "/*", "*/" }; }
        void GetCompletions(const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out) const override;
        bool GetHover(const FCodeDocument& Document, FCodePosition Position, FCodeHover& Out) const override;
        bool GetDiagnostics(const FCodeDocument& Document, TVector<FCodeDiagnostic>& Out) const override;

        // Names the host puts in scope, such as a material node's pins, colored and completed like built-ins.
        struct FContextSymbol
        {
            FString Name;
            FString Detail;
        };
        void SetContextSymbols(TVector<FContextSymbol> Symbols) { ContextSymbols = Move(Symbols); }

    protected:

        bool FindSignature(FStringView Function, FCodeSignature& Out) const override;

        // False inside comments, strings and preprocessor lines, and for a number nothing could complete.
        bool CanCompleteAt(const FCodeCompletionRequest& Request) const;
        void AddKeywordsAndSnippets(TVector<FCodeCompletionItem>& Out, THashSet<FStringView>& Added) const;

        void AddKeywords(TSpan<const char* const> Words);
        void AddTypes(TSpan<const char* const> Words);
        void AddFunctions(TSpan<const FCodeSymbol> Symbols);
        void AddKnownIdentifiers(TSpan<const FCodeSymbol> Symbols);
        void AddSnippet(const char* Label, const char* Body, const char* Detail);

        const FContextSymbol* FindContextSymbol(FStringView Name) const;

        THashSet<FStringView>                     Keywords;
        THashSet<FStringView>                     Types;
        THashMap<FStringView, const FCodeSymbol*> Functions;
        THashMap<FStringView, const FCodeSymbol*> KnownIdentifiers;
        TVector<FCodeCompletionItem>              Snippets;
        TVector<FContextSymbol>                   ContextSymbols;

        bool bPreprocessor = true;
        // C# verbatim strings, which may span lines and escape a quote by doubling it.
        bool bVerbatimStrings = false;
    };
}
