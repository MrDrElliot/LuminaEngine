#include "CodeLanguage.h"

#include <imgui.h>
#include "Containers/StringFormat.h"

namespace Lumina
{
    namespace CodeLanguageInternal
    {
        bool IsStringOrComment(const TVector<ECodeToken>& Tokens, int32 Column)
        {
            if (Column < 0 || Column >= Tokens.Num())
            {
                return false;
            }
            return Tokens[Column] == ECodeToken::String || Tokens[Column] == ECodeToken::Comment;
        }

        bool IsIdentifierStart(char C)
        {
            return CodeText::IsAlpha(C) || C == '_' || (uint8)C >= 0x80;
        }

        // Counts the space separated arguments typed so far after an open parenthesis.
        int32 CountSpaceArguments(FStringView Text, int32 From, int32 Caret)
        {
            int32 Runs = 0;
            int32 Depth = 0;
            bool bInRun = false;
            for (int32 i = From; i < Caret; ++i)
            {
                const char C = Text[i];
                if (C == '(')
                {
                    ++Depth;
                }
                else if (C == ')')
                {
                    --Depth;
                }
                const bool bSpace = CodeText::IsSpace(C) || C == ',';
                if (Depth == 0 && !bSpace && !bInRun)
                {
                    ++Runs;
                }
                bInRun = Depth > 0 || !bSpace;
            }
            return bInRun ? Math::Max(0, Runs - 1) : Runs;
        }
    }

    bool FCodeLanguage::IsWordChar(char C) const
    {
        return CodeText::IsAlpha(C) || CodeText::IsDigit(C) || C == '_' || (uint8)C >= 0x80;
    }

    FCodeCommentSyntax FCodeLanguage::GetCommentSyntax(uint32 State) const
    {
        return { "//", "/*", "*/" };
    }

    bool FCodeLanguage::ShouldIndentAfter(FStringView LineBeforeCaret, uint32 CaretState) const
    {
        const int32 Last = LineBeforeCaret.FindLastNotOf(" \t");
        if (Last == Constants::kIndexNone)
        {
            return false;
        }
        const char C = LineBeforeCaret[Last];
        return C == '{' || C == '(' || C == '[';
    }

    bool FCodeLanguage::ShouldOutdentOnType(FStringView LineThroughCaret, char Typed) const
    {
        if (Typed != '}' && Typed != ')' && Typed != ']')
        {
            return false;
        }
        return CodeText::LeadingWhitespace(LineThroughCaret) == LineThroughCaret.Len() - 1;
    }

    void FCodeLanguage::GetFoldingRanges(const FCodeDocument& Document, TVector<FCodeFold>& Out) const
    {
        TVector<int32> Braces;
        TVector<int32> Regions;
        for (int32 Line = 0; Line < Document.GetLineCount(); ++Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            const TVector<ECodeToken>& Tokens = Document.GetTokens(Line);
            const FStringView Trimmed = Text.Mid((size_t)CodeText::LeadingWhitespace(Text));
            if (CodeText::StartsWithIgnoreCase(Trimmed, "#region"))
            {
                Regions.Add(Line);
            }
            else if (CodeText::StartsWithIgnoreCase(Trimmed, "#endregion") && !Regions.IsEmpty())
            {
                Out.Emplace(Regions.Last(), Line);
                Regions.Pop();
            }
            if (Tokens.Num() != Text.Len())
            {
                continue;
            }
            for (int32 Column = 0; Column < Text.Len(); ++Column)
            {
                if (Tokens[Column] != ECodeToken::Punctuation)
                {
                    continue;
                }
                if (Text[Column] == '{')
                {
                    Braces.Add(Line);
                }
                else if (Text[Column] == '}' && !Braces.IsEmpty())
                {
                    // A brace alone on the next line folds from the line that names the block, as editors usually show it.
                    int32 Start = Braces.Last();
                    Braces.Pop();
                    if (Start > 0 && CodeText::IsBlank(Document.GetLineView(Start).Mid(0, Document.GetLineView(Start).Find('{'))))
                    {
                        --Start;
                    }
                    if (Line - Start >= 2)
                    {
                        Out.Emplace(Start, Line);
                    }
                }
            }
        }
        Algo::Sort(Out, [](const FCodeFold& A, const FCodeFold& B) { return A.StartLine < B.StartLine || (A.StartLine == B.StartLine && A.EndLine > B.EndLine); });
        Out.SetNum(static_cast<int32>(Algo::Unique(Out.begin(), Out.end(), [](const FCodeFold& A, const FCodeFold& B) { return A.StartLine == B.StartLine; }) - Out.begin()));
    }

    bool FCodeLanguage::GetSignature(const FCodeDocument& Document, FCodePosition Caret, FCodeSignature& Out) const
    {
        constexpr int32 MaxLinesBack = 24;
        int32 Depth = 0;
        int32 Commas = 0;
        for (int32 Line = Caret.Line; Line >= Math::Max(0, Caret.Line - MaxLinesBack); --Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            const TVector<ECodeToken>& Tokens = Document.GetTokens(Line);
            int32 Column = Line == Caret.Line ? Caret.Column : Text.Len();
            while (Column > 0)
            {
                --Column;
                if (CodeLanguageInternal::IsStringOrComment(Tokens, Column))
                {
                    continue;
                }
                const char C = Text[Column];
                if (C == ')' || C == ']')
                {
                    ++Depth;
                    continue;
                }
                if (C == '(' || C == '[')
                {
                    if (Depth > 0)
                    {
                        --Depth;
                        continue;
                    }
                    if (C == '[')
                    {
                        return false;
                    }
                    int32 NameEnd = Column;
                    while (NameEnd > 0 && CodeText::IsSpace(Text[NameEnd - 1]))
                    {
                        --NameEnd;
                    }
                    int32 NameStart = NameEnd;
                    while (NameStart > 0 && (IsWordChar(Text[NameStart - 1]) || Text[NameStart - 1] == '-'))
                    {
                        --NameStart;
                    }
                    if (NameStart == NameEnd || !FindSignature(Text.Mid(NameStart, NameEnd - NameStart), Out))
                    {
                        return false;
                    }
                    if (Out.bSpaceSeparated)
                    {
                        Out.ActiveParameter = Line == Caret.Line ? CodeLanguageInternal::CountSpaceArguments(Text, Column + 1, Caret.Column) : 0;
                    }
                    else
                    {
                        Out.ActiveParameter = Commas;
                    }
                    return true;
                }
                if (Depth == 0)
                {
                    if (C == ',')
                    {
                        ++Commas;
                    }
                    else if (C == ';' || C == '{' || C == '}')
                    {
                        return false;
                    }
                }
            }
        }
        return false;
    }

    namespace CodeLanguageUtil
    {
        void ParseSignatureParameters(FCodeSignature& Signature)
        {
            Signature.Parameters.Reset();
            const FStringView Label = Signature.Label.View();
            const int32 Open = Label.Find('(');
            const int32 Close = Label.FindLast(')');
            if (Open == Constants::kIndexNone || Close == Constants::kIndexNone || Close <= Open + 1)
            {
                return;
            }

            int32 Depth = 0;
            bool bHasComma = false;
            for (int32 i = Open + 1; i < Close; ++i)
            {
                Depth += Label[i] == '(' ? 1 : Label[i] == ')' ? -1 : 0;
                bHasComma |= Depth == 0 && Label[i] == ',';
            }
            Signature.bSpaceSeparated = !bHasComma;

            auto IsSeparator = [&Signature](char C)
            {
                return Signature.bSpaceSeparated ? CodeText::IsSpace(C) : C == ',';
            };

            int32 Start = (int32)Open + 1;
            Depth = 0;
            for (int32 i = Start; i <= (int32)Close; ++i)
            {
                const char C = Label[i];
                if (i < (int32)Close && (C == '(' || C == '[' || C == '<'))
                {
                    ++Depth;
                    continue;
                }
                if (i < (int32)Close && (C == ')' || C == ']' || C == '>'))
                {
                    --Depth;
                    continue;
                }
                if (i == (int32)Close || (Depth == 0 && IsSeparator(C)))
                {
                    int32 SpanStart = Start;
                    int32 SpanEnd = i;
                    while (SpanStart < SpanEnd && CodeText::IsSpace(Label[SpanStart]))
                    {
                        ++SpanStart;
                    }
                    while (SpanEnd > SpanStart && CodeText::IsSpace(Label[SpanEnd - 1]))
                    {
                        --SpanEnd;
                    }
                    if (SpanEnd > SpanStart)
                    {
                        Signature.Parameters.Emplace(SpanStart, SpanEnd);
                    }
                    Start = i + 1;
                }
            }
        }

        FCodeRange WordRangeAt(const FCodeDocument& Document, const FCodeLanguage& Language, FCodePosition Position)
        {
            Position = Document.Clamp(Position);
            const FStringView Text = Document.GetLineView(Position.Line);
            int32 Start = Position.Column;
            int32 End = Position.Column;
            while (Start > 0 && Language.IsWordChar(Text[Start - 1]))
            {
                --Start;
            }
            while (End < Text.Len() && Language.IsWordChar(Text[End]))
            {
                ++End;
            }
            return { { Position.Line, Start }, { Position.Line, End } };
        }

        void CollectDocumentWords(const FCodeDocument& Document, const FCodeLanguage& Language, FCodePosition Exclude, int32 MaxWords, TVector<FString>& Out)
        {
            THashSet<FStringView> Seen;
            const FCodeRange Excluded = WordRangeAt(Document, Language, Exclude);
            for (int32 Line = 0; Line < Document.GetLineCount() && Out.Num() < MaxWords; ++Line)
            {
                const FStringView Text = Document.GetLineView(Line);
                int32 Column = 0;
                while (Column < Text.Len())
                {
                    if (!Language.IsWordChar(Text[Column]))
                    {
                        ++Column;
                        continue;
                    }
                    const int32 Start = Column;
                    while (Column < Text.Len() && Language.IsWordChar(Text[Column]))
                    {
                        ++Column;
                    }
                    const bool bExcluded = Line == Excluded.Start.Line && Start == Excluded.Start.Column;
                    const FStringView Word = Text.Mid(Start, Column - Start);
                    if (!bExcluded && Word.Len() >= 3 && !CodeText::IsDigit(Word[0]) && Seen.TryAdd(Word))
                    {
                        Out.Emplace(Word);
                    }
                }
            }
        }

        bool ParseHexColor(FStringView Hex, uint32& OutColor)
        {
            if (!Hex.IsEmpty() && Hex[0] == '#')
            {
                Hex = Hex.Mid(1);
            }
            const int32 Digits = Hex.Len();
            if (Digits != 3 && Digits != 4 && Digits != 6 && Digits != 8)
            {
                return false;
            }

            uint32 Channels[4] = { 0, 0, 0, 255 };
            auto Nibble = [](char C) -> uint32
            {
                return CodeText::IsDigit(C) ? (uint32)(C - '0') : (uint32)(CodeText::ToLower(C) - 'a' + 10);
            };
            for (char C : Hex)
            {
                if (!CodeText::IsHexDigit(C))
                {
                    return false;
                }
            }
            const bool bShort = Digits <= 4;
            const size_t Count = bShort ? Digits : Digits / 2;
            for (int32 i = 0, Limit = IntCastChecked<int32>(Count); i < Limit; ++i)
            {
                Channels[i] = bShort ? Nibble(Hex[i]) * 17 : Nibble(Hex[i * 2]) * 16 + Nibble(Hex[i * 2 + 1]);
            }
            OutColor = IM_COL32(Channels[0], Channels[1], Channels[2], Channels[3]);
            return true;
        }

        FString FormatHexColor(uint32 Color, bool bWithAlpha)
        {
            const uint32 R = (Color >> IM_COL32_R_SHIFT) & 0xFF;
            const uint32 G = (Color >> IM_COL32_G_SHIFT) & 0xFF;
            const uint32 B = (Color >> IM_COL32_B_SHIFT) & 0xFF;
            const uint32 A = (Color >> IM_COL32_A_SHIFT) & 0xFF;
            char Buffer[16];
            if (bWithAlpha)
            {
                std::snprintf(Buffer, sizeof(Buffer), "#%02x%02x%02x%02x", R, G, B, A);
            }
            else
            {
                std::snprintf(Buffer, sizeof(Buffer), "#%02x%02x%02x", R, G, B);
            }
            return FString(Buffer);
        }
    }

    namespace CLikeStates
    {
        constexpr uint32 Code = 0;
        constexpr uint32 BlockComment = 1;
        constexpr uint32 VerbatimString = 2;
    }

    uint32 FCLikeLanguage::Tokenize(FStringView Line, uint32 State, ECodeToken* OutTokens) const
    {
        const int32 Size = Line.Len();
        auto Mark = [OutTokens](int32 From, int32 To, ECodeToken Token)
        {
            for (int32 i = From; i < To; ++i)
            {
                OutTokens[i] = Token;
            }
        };

        int32 i = 0;
        if (State == CLikeStates::BlockComment)
        {
            const int32 Close = Line.Find("*/");
            if (Close == Constants::kIndexNone)
            {
                Mark(0, Size, ECodeToken::Comment);
                return CLikeStates::BlockComment;
            }
            i = (int32)Close + 2;
            Mark(0, i, ECodeToken::Comment);
        }
        else if (State == CLikeStates::VerbatimString)
        {
            while (i < Size)
            {
                if (Line[i] == '"' && (i + 1 >= Size || Line[i + 1] != '"'))
                {
                    break;
                }
                i += Line[i] == '"' ? 2 : 1;
            }
            if (i >= Size)
            {
                Mark(0, Size, ECodeToken::String);
                return CLikeStates::VerbatimString;
            }
            Mark(0, ++i, ECodeToken::String);
        }

        if (bPreprocessor && i == 0)
        {
            const int32 First = CodeText::LeadingWhitespace(Line);
            if (First < Size && Line[First] == '#')
            {
                Mark(0, Size, ECodeToken::Preprocessor);
                return CLikeStates::Code;
            }
        }

        while (i < Size)
        {
            const char C = Line[i];
            const char Next = i + 1 < Size ? Line[i + 1] : 0;

            if (CodeText::IsSpace(C))
            {
                OutTokens[i++] = ECodeToken::Default;
                continue;
            }
            if (C == '/' && Next == '/')
            {
                Mark(i, Size, ECodeToken::Comment);
                return CLikeStates::Code;
            }
            if (C == '/' && Next == '*')
            {
                const int32 Close = Line.Find("*/", i + 2);
                if (Close == Constants::kIndexNone)
                {
                    Mark(i, Size, ECodeToken::Comment);
                    return CLikeStates::BlockComment;
                }
                Mark(i, (int32)Close + 2, ECodeToken::Comment);
                i = (int32)Close + 2;
                continue;
            }

            const bool bVerbatim = bVerbatimStrings && ((C == '@' && Next == '"') || ((C == '$' || C == '@') && (Next == '@' || Next == '$') && i + 2 < Size && Line[i + 2] == '"'));
            if (bVerbatim)
            {
                const int32 Start = i;
                i += C == '@' && Next == '"' ? 2 : 3;
                while (i < Size)
                {
                    if (Line[i] == '"' && (i + 1 >= Size || Line[i + 1] != '"'))
                    {
                        break;
                    }
                    i += Line[i] == '"' ? 2 : 1;
                }
                if (i >= Size)
                {
                    Mark(Start, Size, ECodeToken::String);
                    return CLikeStates::VerbatimString;
                }
                Mark(Start, ++i, ECodeToken::String);
                continue;
            }

            if (C == '"' || C == '\'' || (bVerbatimStrings && C == '$' && Next == '"'))
            {
                const int32 Start = i;
                const char Quote = C == '$' ? '"' : C;
                i += C == '$' ? 2 : 1;
                while (i < Size && Line[i] != Quote)
                {
                    i += Line[i] == '\\' ? 2 : 1;
                }
                i = Math::Min(i + 1, Size);
                Mark(Start, i, ECodeToken::String);
                continue;
            }

            if (CodeText::IsDigit(C) || (C == '.' && CodeText::IsDigit(Next)))
            {
                const int32 Start = i;
                while (i < Size && (CodeText::IsAlpha(Line[i]) || CodeText::IsDigit(Line[i]) || Line[i] == '.' || Line[i] == '_'))
                {
                    const bool bExponent = (Line[i] == 'e' || Line[i] == 'E') && i + 1 < Size && (Line[i + 1] == '+' || Line[i + 1] == '-');
                    i += bExponent ? 2 : 1;
                }
                Mark(Start, i, ECodeToken::Number);
                continue;
            }

            if (CodeLanguageInternal::IsIdentifierStart(C) || (bVerbatimStrings && C == '@' && CodeLanguageInternal::IsIdentifierStart(Next)))
            {
                const int32 Start = i++;
                while (i < Size && IsWordChar(Line[i]))
                {
                    ++i;
                }
                const FStringView Word = Line.Mid(Start, i - Start);
                ECodeToken Token = ECodeToken::Default;
                if (Keywords.Contains(Word))
                {
                    Token = ECodeToken::Keyword;
                }
                else if (Types.Contains(Word))
                {
                    Token = ECodeToken::Type;
                }
                else if (Functions.Contains(Word))
                {
                    Token = ECodeToken::Function;
                }
                else if (KnownIdentifiers.Contains(Word) || FindContextSymbol(Word) != nullptr)
                {
                    Token = ECodeToken::KnownIdentifier;
                }
                else
                {
                    int32 After = i;
                    while (After < Size && CodeText::IsSpace(Line[After]))
                    {
                        ++After;
                    }
                    if (After < Size && Line[After] == '(')
                    {
                        Token = ECodeToken::Function;
                    }
                }
                Mark(Start, i, Token);
                continue;
            }

            const int32 Start = i;
            i = CodeText::NextCharStart(Line, i);
            Mark(Start, i, ECodeToken::Punctuation);
        }
        return CLikeStates::Code;
    }

    bool FCLikeLanguage::CanCompleteAt(const FCodeCompletionRequest& Request) const
    {
        if (Request.CaretState != CLikeStates::Code)
        {
            return false;
        }
        const ECodeToken Before = Request.Document->GetTokenAt({ Request.Caret.Line, Request.Caret.Column - 1 });
        if (Before == ECodeToken::String || Before == ECodeToken::Comment || Before == ECodeToken::Preprocessor)
        {
            return false;
        }
        return Request.Word.IsEmpty() || !CodeText::IsDigit(Request.Word[0]);
    }

    void FCLikeLanguage::AddKeywordsAndSnippets(TVector<FCodeCompletionItem>& Out, THashSet<FStringView>& Added) const
    {
        for (const FStringView Keyword : Keywords)
        {
            if (Added.TryAdd(Keyword))
            {
                FCodeCompletionItem& Item = Out.EmplaceGetRef();
                Item.Label = Keyword;
                Item.Kind = ECodeCompletionKind::Keyword;
                Item.Detail = "keyword";
            }
        }
        for (const FCodeCompletionItem& Snippet : Snippets)
        {
            Out.Add(Snippet);
        }
    }

    void FCLikeLanguage::GetCompletions(const FCodeCompletionRequest& Request, TVector<FCodeCompletionItem>& Out) const
    {
        if (!CanCompleteAt(Request) || (Request.Word.IsEmpty() && !Request.bExplicit))
        {
            return;
        }

        THashSet<FStringView> Added;
        auto Add = [&Out, &Added](FStringView Label, ECodeCompletionKind Kind, FStringView Detail, FStringView Documentation, FStringView Insert, int32 Boost)
        {
            if (!Added.TryAdd(Label))
            {
                return;
            }
            FCodeCompletionItem& Item = Out.EmplaceGetRef();
            Item.Label = Label;
            Item.Kind = Kind;
            Item.Detail = Detail;
            Item.Documentation = Documentation;
            Item.InsertText = Insert;
            Item.SortBoost = Boost;
        };

        for (const FContextSymbol& Symbol : ContextSymbols)
        {
            Add(Symbol.Name.View(), ECodeCompletionKind::Variable, Symbol.Detail.View(), {}, {}, 30);
        }
        for (const auto& [Name, Symbol] : KnownIdentifiers)
        {
            Add(Name, ECodeCompletionKind::Variable, Symbol->Signature, Symbol->Documentation, {}, 10);
        }
        for (const auto& [Name, Symbol] : Functions)
        {
            Add(Name, ECodeCompletionKind::Function, Symbol->Signature, Symbol->Documentation, {}, 5);
        }
        for (const FStringView Type : Types)
        {
            Add(Type, ECodeCompletionKind::Type, "type", {}, {}, 0);
        }
        AddKeywordsAndSnippets(Out, Added);

        TVector<FString> Words;
        CodeLanguageUtil::CollectDocumentWords(*Request.Document, *this, Request.Caret, 2000, Words);
        for (const FString& Word : Words)
        {
            if (!Added.Contains(Word.View()))
            {
                FCodeCompletionItem& Item = Out.EmplaceGetRef();
                Item.Label = Word;
                Item.Kind = ECodeCompletionKind::Text;
                Item.Detail = "in document";
                Item.SortBoost = -5;
            }
        }
    }

    bool FCLikeLanguage::GetHover(const FCodeDocument& Document, FCodePosition Position, FCodeHover& Out) const
    {
        const FCodeRange Range = CodeLanguageUtil::WordRangeAt(Document, *this, Position);
        if (Range.IsEmpty())
        {
            return false;
        }
        const FStringView Text = Document.GetLineView(Range.Start.Line);
        const FStringView Word = Text.Mid(Range.Start.Column, Range.End.Column - Range.Start.Column);
        Out.Range = Range;

        if (const auto Found = Functions.Find(Word); Found != nullptr)
        {
            Out.Title = (*Found)->Signature;
            Out.Body = (*Found)->Documentation;
            return true;
        }
        if (const auto Found = KnownIdentifiers.Find(Word); Found != nullptr)
        {
            Out.Title = *(*Found)->Signature != '\0' ? FString((*Found)->Signature) : FString(Word);
            Out.Body = (*Found)->Documentation;
            return true;
        }
        if (const FContextSymbol* Symbol = FindContextSymbol(Word))
        {
            Out.Title = Symbol->Name;
            Out.Body = Symbol->Detail;
            return true;
        }
        return false;
    }

    bool FCLikeLanguage::GetDiagnostics(const FCodeDocument& Document, TVector<FCodeDiagnostic>& Out) const
    {
        struct FOpen
        {
            char          Bracket;
            FCodePosition Position;
        };
        TVector<FOpen> Stack;
        auto Report = [&Out](FCodePosition At, FString Message)
        {
            FCodeDiagnostic& Diagnostic = Out.EmplaceGetRef();
            Diagnostic.Range = { At, { At.Line, At.Column + 1 } };
            Diagnostic.Message = Move(Message);
        };

        for (int32 Line = 0; Line < Document.GetLineCount(); ++Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            const TVector<ECodeToken>& Tokens = Document.GetTokens(Line);
            for (int32 Column = 0; Column < Text.Len() && Column < Tokens.Num(); ++Column)
            {
                if (Tokens[Column] != ECodeToken::Punctuation)
                {
                    continue;
                }
                const char C = Text[Column];
                if (C == '(' || C == '[' || C == '{')
                {
                    Stack.Add({ C, { Line, Column } });
                }
                else if (C == ')' || C == ']' || C == '}')
                {
                    const char Expected = C == ')' ? '(' : C == ']' ? '[' : '{';
                    if (Stack.IsEmpty() || Stack.Last().Bracket != Expected)
                    {
                        Report({ Line, Column }, Format("Unexpected '{}'", C));
                        return true;
                    }
                    Stack.Pop();
                }
            }
        }
        if (!Stack.IsEmpty())
        {
            Report(Stack.Last().Position, Format("'{}' is never closed", Stack.Last().Bracket));
        }
        return true;
    }

    // A light outline for the C family without a compiler behind it, the types and functions a line opens at the top two levels.
    bool FCLikeLanguage::GetDocumentSymbols(const FCodeDocument& Document, TVector<FCodeSymbolInfo>& Out) const
    {
        constexpr FStringView TypeKeywords[] = { "struct", "class", "interface", "enum", "namespace", "cbuffer" };
        int32 Depth = 0;
        TVector<int32> Open;
        for (int32 Line = 0; Line < Document.GetLineCount(); ++Line)
        {
            const FStringView Text = Document.GetLineView(Line);
            const TVector<ECodeToken>& Tokens = Document.GetTokens(Line);
            const bool bTokens = Tokens.Num() == Text.Len();
            const int32 Indent = CodeText::LeadingWhitespace(Text);
            if (Depth <= 1 && bTokens && Indent < Text.Len() && Tokens[Indent] != ECodeToken::Comment && Tokens[Indent] != ECodeToken::Preprocessor)
            {
                int32 Column = Indent;
                auto ReadWord = [&](int32& At) -> FStringView
                {
                    while (At < Text.Len() && CodeText::IsSpace(Text[At])) { ++At; }
                    const int32 Start = At;
                    while (At < Text.Len() && IsWordChar(Text[At])) { ++At; }
                    return Text.Mid(Start, At - Start);
                };
                const FStringView First = ReadWord(Column);
                const bool bType = Algo::Contains(TypeKeywords, First);
                if (bType)
                {
                    const int32 NameStart = Column + 1;
                    const FStringView Name = ReadWord(Column);
                    if (!Name.IsEmpty())
                    {
                        FCodeSymbolInfo& Symbol = Out.EmplaceGetRef();
                        Symbol.Name = Name;
                        Symbol.Detail = First;
                        Symbol.Kind = First == "namespace" ? ECodeCompletionKind::Module : ECodeCompletionKind::Type;
                        Symbol.Position = { Line, Math::Min(NameStart, Text.Len()) };
                        Symbol.EndLine = Line;
                        Symbol.Depth = Depth;
                    }
                }
                else if (!First.IsEmpty() && !Keywords.Contains(First))
                {
                    // A return type, a name and an open parenthesis, with no semicolon closing it as a call or a declaration.
                    const int32 Paren = Text.Find('(');
                    if (Paren != Constants::kIndexNone && Text.Find(';') == Constants::kIndexNone && Text.Find('=') == Constants::kIndexNone)
                    {
                        int32 NameEnd = (int32)Paren;
                        while (NameEnd > 0 && CodeText::IsSpace(Text[NameEnd - 1])) { --NameEnd; }
                        int32 NameStart = NameEnd;
                        while (NameStart > 0 && IsWordChar(Text[NameStart - 1])) { --NameStart; }
                        const FStringView Name = Text.Mid(NameStart, NameEnd - NameStart);
                        if (!Name.IsEmpty() && NameStart > Indent && !Keywords.Contains(Name))
                        {
                            FCodeSymbolInfo& Symbol = Out.EmplaceGetRef();
                            Symbol.Name = Name;
                            Symbol.Detail = FString(Text.Mid(Indent));
                            Symbol.Kind = ECodeCompletionKind::Function;
                            Symbol.Position = { Line, NameStart };
                            Symbol.EndLine = Line;
                            Symbol.Depth = Depth;
                        }
                    }
                }
            }
            for (int32 Column = 0; bTokens && Column < Text.Len(); ++Column)
            {
                if (Tokens[Column] == ECodeToken::Punctuation && Text[Column] == '{')
                {
                    ++Depth;
                }
                else if (Tokens[Column] == ECodeToken::Punctuation && Text[Column] == '}')
                {
                    Depth = Math::Max(0, Depth - 1);
                }
            }
        }
        return true;
    }

    bool FCLikeLanguage::FindSignature(FStringView Function, FCodeSignature& Out) const
    {
        const auto Found = Functions.Find(Function);
        if (Found == nullptr || *(*Found)->Signature == '\0')
        {
            return false;
        }
        Out.Label = (*Found)->Signature;
        Out.Documentation = (*Found)->Documentation;
        CodeLanguageUtil::ParseSignatureParameters(Out);
        return true;
    }

    void FCLikeLanguage::AddKeywords(TSpan<const char* const> Words)
    {
        for (const char* Word : Words)
        {
            Keywords.Emplace(Word);
        }
    }

    void FCLikeLanguage::AddTypes(TSpan<const char* const> Words)
    {
        for (const char* Word : Words)
        {
            Types.Emplace(Word);
        }
    }

    void FCLikeLanguage::AddFunctions(TSpan<const FCodeSymbol> Symbols)
    {
        for (const FCodeSymbol& Symbol : Symbols)
        {
            Functions.TryAdd(FStringView(Symbol.Name), &Symbol);
        }
    }

    void FCLikeLanguage::AddKnownIdentifiers(TSpan<const FCodeSymbol> Symbols)
    {
        for (const FCodeSymbol& Symbol : Symbols)
        {
            KnownIdentifiers.TryAdd(FStringView(Symbol.Name), &Symbol);
        }
    }

    void FCLikeLanguage::AddSnippet(const char* Label, const char* Body, const char* Detail)
    {
        FCodeCompletionItem& Item = Snippets.EmplaceGetRef();
        Item.Label = Label;
        Item.InsertText = Body;
        Item.Detail = Detail;
        Item.Kind = ECodeCompletionKind::Snippet;
        Item.SortBoost = -2;
    }

    const FCLikeLanguage::FContextSymbol* FCLikeLanguage::FindContextSymbol(FStringView Name) const
    {
        for (const FContextSymbol& Symbol : ContextSymbols)
        {
            if (Symbol.Name == Name)
            {
                return &Symbol;
            }
        }
        return nullptr;
    }
}
