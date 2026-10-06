#include "RmlBindingAnalysis.h"

#include <algorithm>
#include <cctype>

namespace Lumina::RmlBinding
{
    // Named rather than anonymous, so a unity build merging this with the editor tool cannot collide on helper names.
    namespace Scanning
    {
        bool StartsWith(std::string_view Text, std::string_view Prefix)
        {
            return Text.size() >= Prefix.size() && Text.substr(0, Prefix.size()) == Prefix;
        }

        bool IsIdentStart(char Character)
        {
            return std::isalpha(static_cast<unsigned char>(Character)) || Character == '_';
        }

        bool IsIdentChar(char Character)
        {
            return std::isalnum(static_cast<unsigned char>(Character)) || Character == '_';
        }

        FString ToFString(std::string_view Text)
        {
            return FString(Text.data(), Text.size());
        }

        std::string_view Trim(std::string_view Text)
        {
            while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.front())))
            {
                Text.remove_prefix(1);
            }
            while (!Text.empty() && std::isspace(static_cast<unsigned char>(Text.back())))
            {
                Text.remove_suffix(1);
            }
            return Text;
        }

        // Line of each byte offset, by binary search over the newline positions.
        struct FLineIndex
        {
            explicit FLineIndex(std::string_view Text)
            {
                for (size_t Index = 0; Index < Text.size(); ++Index)
                {
                    if (Text[Index] == '\n')
                    {
                        Newlines.push_back(Index);
                    }
                }
            }

            int32 LineAt(size_t Offset) const
            {
                return (int32)(std::upper_bound(Newlines.begin(), Newlines.end(), Offset) - Newlines.begin()) + 1;
            }

            TVector<size_t> Newlines;
        };

        struct FRawAttribute
        {
            std::string_view Name;
            std::string_view Value;
            size_t           ValueOffset = 0;
        };

        // Reads the attributes of the tag whose name ends at Cursor, stopping on the closing '>'.
        size_t ParseAttributes(std::string_view Text, size_t Cursor, TVector<FRawAttribute>& Out, bool& bSelfClosing)
        {
            bSelfClosing = false;
            while (Cursor < Text.size())
            {
                const char Character = Text[Cursor];
                if (Character == '>')
                {
                    return Cursor + 1;
                }
                if (Character == '/' && Cursor + 1 < Text.size() && Text[Cursor + 1] == '>')
                {
                    bSelfClosing = true;
                    return Cursor + 2;
                }
                if (std::isspace(static_cast<unsigned char>(Character)))
                {
                    ++Cursor;
                    continue;
                }

                const size_t NameStart = Cursor;
                while (Cursor < Text.size() && !std::isspace(static_cast<unsigned char>(Text[Cursor])) && Text[Cursor] != '='
                    && Text[Cursor] != '>' && Text[Cursor] != '/')
                {
                    ++Cursor;
                }
                FRawAttribute Attribute;
                Attribute.Name = Text.substr(NameStart, Cursor - NameStart);

                while (Cursor < Text.size() && std::isspace(static_cast<unsigned char>(Text[Cursor])))
                {
                    ++Cursor;
                }
                if (Cursor < Text.size() && Text[Cursor] == '=')
                {
                    ++Cursor;
                    while (Cursor < Text.size() && std::isspace(static_cast<unsigned char>(Text[Cursor])))
                    {
                        ++Cursor;
                    }
                    if (Cursor < Text.size() && (Text[Cursor] == '"' || Text[Cursor] == '\''))
                    {
                        const char Quote = Text[Cursor++];
                        const size_t ValueStart = Cursor;
                        while (Cursor < Text.size() && Text[Cursor] != Quote)
                        {
                            ++Cursor;
                        }
                        Attribute.Value = Text.substr(ValueStart, Cursor - ValueStart);
                        Attribute.ValueOffset = ValueStart;
                        if (Cursor < Text.size())
                        {
                            ++Cursor;
                        }
                    }
                    else
                    {
                        const size_t ValueStart = Cursor;
                        while (Cursor < Text.size() && !std::isspace(static_cast<unsigned char>(Text[Cursor])) && Text[Cursor] != '>')
                        {
                            ++Cursor;
                        }
                        Attribute.Value = Text.substr(ValueStart, Cursor - ValueStart);
                        Attribute.ValueOffset = ValueStart;
                    }
                }
                if (!Attribute.Name.empty())
                {
                    Out.push_back(Attribute);
                }
                else
                {
                    ++Cursor;
                }
            }
            return Cursor;
        }

        bool IsVoidTag(std::string_view Tag)
        {
            return Tag == "br" || Tag == "img" || Tag == "input" || Tag == "hr" || Tag == "meta" || Tag == "link" || Tag == "col";
        }

        // Every model, list or command name an RmlUi data expression reads.
        void ScanExpression(std::string_view Expression, bool bEvent, ERefUse Use, int32 Line, const FString& Model, TVector<FBindingRef>& Out)
        {
            size_t Index = 0;
            bool bTransform = false;
            while (Index < Expression.size())
            {
                const char Character = Expression[Index];
                if (Character == '\'' || Character == '"')
                {
                    const size_t Close = Expression.find(Character, Index + 1);
                    Index = Close == std::string_view::npos ? Expression.size() : Close + 1;
                    continue;
                }
                // An attribute spells < as &lt;, which is markup rather than a member name.
                if (Character == '&' && Index + 1 < Expression.size() && IsIdentStart(Expression[Index + 1]))
                {
                    const size_t Semicolon = Expression.find(';', Index);
                    if (Semicolon != std::string_view::npos && Semicolon - Index <= 8)
                    {
                        Index = Semicolon + 1;
                        continue;
                    }
                }
                if (std::isdigit(static_cast<unsigned char>(Character)))
                {
                    while (Index < Expression.size() && (IsIdentChar(Expression[Index]) || Expression[Index] == '.'))
                    {
                        ++Index;
                    }
                    continue;
                }
                if (Character == '|' && (Index + 1 >= Expression.size() || Expression[Index + 1] != '|'))
                {
                    bTransform = true;
                    ++Index;
                    continue;
                }
                if (!IsIdentStart(Character))
                {
                    if (Character == '|' || Character == '&')
                    {
                        Index += 2;
                    }
                    else
                    {
                        ++Index;
                    }
                    continue;
                }

                const size_t Start = Index;
                size_t RootEnd = std::string_view::npos;
                while (Index < Expression.size())
                {
                    if (IsIdentChar(Expression[Index]))
                    {
                        ++Index;
                    }
                    else if (Expression[Index] == '.' && Index + 1 < Expression.size() && IsIdentStart(Expression[Index + 1]))
                    {
                        if (RootEnd == std::string_view::npos)
                        {
                            RootEnd = Index;
                        }
                        ++Index;
                    }
                    else if (Expression[Index] == '[')
                    {
                        const size_t Close = Expression.find(']', Index);
                        Index = Close == std::string_view::npos ? Expression.size() : Close + 1;
                    }
                    else
                    {
                        break;
                    }
                }

                const std::string_view Whole = Expression.substr(Start, Index - Start);
                const std::string_view Root = RootEnd == std::string_view::npos ? Whole : Expression.substr(Start, RootEnd - Start);
                std::string_view Member;
                if (RootEnd != std::string_view::npos)
                {
                    Member = Expression.substr(RootEnd + 1, Index - RootEnd - 1);
                    const size_t Dot = Member.find('.');
                    if (Dot != std::string_view::npos)
                    {
                        Member = Member.substr(0, Dot);
                    }
                    const size_t Bracket = Member.find('[');
                    if (Bracket != std::string_view::npos)
                    {
                        Member = Member.substr(0, Bracket);
                    }
                }

                size_t Next = Index;
                while (Next < Expression.size() && std::isspace(static_cast<unsigned char>(Expression[Next])))
                {
                    ++Next;
                }
                const bool bCall = Next < Expression.size() && Expression[Next] == '(';

                if (bTransform)
                {
                    bTransform = false;
                    continue;
                }
                if (Root == "true" || Root == "false" || Root == "ev")
                {
                    continue;
                }

                FBindingRef Ref;
                Ref.Model = Model;
                Ref.Name = ToFString(Root);
                Ref.Member = ToFString(Member);
                Ref.Use = Use;
                Ref.Line = Line;
                if (bCall)
                {
                    if (!bEvent)
                    {
                        continue;
                    }
                    Ref.Kind = ERefKind::Command;
                    Ref.Member.clear();
                }
                Out.push_back(Ref);
            }
        }

        ERefUse UseOf(std::string_view Attribute)
        {
            if (Attribute == "data-if" || Attribute == "data-visible" || Attribute == "data-checked" || StartsWith(Attribute, "data-class-"))
            {
                return ERefUse::Condition;
            }
            if (StartsWith(Attribute, "data-style-"))
            {
                return ERefUse::Style;
            }
            if (Attribute == "data-value")
            {
                return ERefUse::Input;
            }
            if (Attribute == "data-text" || Attribute == "data-rml")
            {
                return ERefUse::Text;
            }
            return ERefUse::Other;
        }

        void ScanForScope(std::string_view Value, int32 Line, const FString& Model, FDocumentBindings& Out)
        {
            FForScope Scope;
            Scope.Model = Model;
            Scope.Line = Line;
            Scope.Alias = "it";
            Scope.IndexAlias = "it_index";

            std::string_view Source = Value;
            const size_t Colon = Value.find(':');
            if (Colon != std::string_view::npos)
            {
                const std::string_view Names = Value.substr(0, Colon);
                Source = Value.substr(Colon + 1);
                const size_t Comma = Names.find(',');
                Scope.Alias = ToFString(Trim(Names.substr(0, Comma)));
                Scope.IndexAlias = Comma == std::string_view::npos ? Scope.Alias + "_index" : ToFString(Trim(Names.substr(Comma + 1)));
            }

            Source = Trim(Source);
            const size_t Dot = Source.find('.');
            Scope.List = ToFString(Dot == std::string_view::npos ? Source : Source.substr(0, Dot));
            Out.ForScopes.push_back(Scope);

            FBindingRef Ref;
            Ref.Model = Model;
            Ref.Name = Scope.List;
            Ref.Kind = ERefKind::ListSource;
            Ref.Line = Line;
            Out.Refs.push_back(Ref);
        }

        struct FOpenElement
        {
            std::string_view Tag;
            FString          Model;
        };

        FString CurrentModel(const TVector<FOpenElement>& Stack)
        {
            for (size_t Index = Stack.size(); Index > 0; --Index)
            {
                if (!Stack[Index - 1].Model.empty())
                {
                    return Stack[Index - 1].Model;
                }
            }
            return FString();
        }

        template<typename T>
        bool ContainsName(const TVector<T>& Items, FStringView Name)
        {
            for (const T& Item : Items)
            {
                if (FStringView(Item.Name.c_str(), Item.Name.size()) == Name)
                {
                    return true;
                }
            }
            return false;
        }
    }

    using namespace Scanning;

    bool FDocumentBindings::IsAlias(FStringView Model, FStringView Name) const
    {
        return FindAlias(Model, Name) != nullptr;
    }

    const FForScope* FDocumentBindings::FindAlias(FStringView Model, FStringView Name) const
    {
        for (const FForScope& Scope : ForScopes)
        {
            if (FStringView(Scope.Model.c_str(), Scope.Model.size()) != Model)
            {
                continue;
            }
            if (FStringView(Scope.Alias.c_str(), Scope.Alias.size()) == Name || FStringView(Scope.IndexAlias.c_str(), Scope.IndexAlias.size()) == Name)
            {
                return &Scope;
            }
        }
        return nullptr;
    }

    void Scan(std::string_view Text, FDocumentBindings& Out)
    {
        Out = FDocumentBindings();
        const FLineIndex Lines(Text);
        TVector<FOpenElement> Stack;

        size_t Cursor = 0;
        while (Cursor < Text.size())
        {
            const std::string_view Rest = Text.substr(Cursor);
            if (StartsWith(Rest, "<!--"))
            {
                const size_t End = Text.find("-->", Cursor + 4);
                Cursor = End == std::string_view::npos ? Text.size() : End + 3;
                continue;
            }
            if (StartsWith(Rest, "<![CDATA["))
            {
                const size_t End = Text.find("]]>", Cursor);
                Cursor = End == std::string_view::npos ? Text.size() : End + 3;
                continue;
            }
            if (Text[Cursor] == '<' && Cursor + 1 < Text.size() && Text[Cursor + 1] == '/')
            {
                size_t NameEnd = Cursor + 2;
                while (NameEnd < Text.size() && IsIdentChar(Text[NameEnd]))
                {
                    ++NameEnd;
                }
                const std::string_view Tag = Text.substr(Cursor + 2, NameEnd - Cursor - 2);
                for (size_t Index = Stack.size(); Index > 0; --Index)
                {
                    if (Stack[Index - 1].Tag == Tag)
                    {
                        Stack.resize(Index - 1);
                        break;
                    }
                }
                const size_t Close = Text.find('>', NameEnd);
                Cursor = Close == std::string_view::npos ? Text.size() : Close + 1;
                continue;
            }
            if (Text[Cursor] == '<' && Cursor + 1 < Text.size() && IsIdentStart(Text[Cursor + 1]))
            {
                size_t NameEnd = Cursor + 1;
                while (NameEnd < Text.size() && (IsIdentChar(Text[NameEnd]) || Text[NameEnd] == '-'))
                {
                    ++NameEnd;
                }
                const std::string_view Tag = Text.substr(Cursor + 1, NameEnd - Cursor - 1);

                TVector<FRawAttribute> Attributes;
                bool bSelfClosing = false;
                Cursor = ParseAttributes(Text, NameEnd, Attributes, bSelfClosing);

                FOpenElement Element;
                Element.Tag = Tag;
                for (const FRawAttribute& Attribute : Attributes)
                {
                    if (Attribute.Name == "data-model")
                    {
                        Element.Model = ToFString(Trim(Attribute.Value));
                        Out.Models.push_back(Element.Model);
                        Out.ModelLines.push_back(Lines.LineAt(Attribute.ValueOffset));
                    }
                }

                const FString Model = Element.Model.empty() ? CurrentModel(Stack) : Element.Model;
                for (const FRawAttribute& Attribute : Attributes)
                {
                    if (!StartsWith(Attribute.Name, "data-") || Attribute.Name == "data-model" || StartsWith(Attribute.Name, "data-alias-"))
                    {
                        continue;
                    }
                    const int32 Line = Lines.LineAt(Attribute.ValueOffset);
                    if (Attribute.Name == "data-for")
                    {
                        ScanForScope(Attribute.Value, Line, Model, Out);
                    }
                    else if (StartsWith(Attribute.Name, "data-event-"))
                    {
                        ScanExpression(Attribute.Value, true, ERefUse::Event, Line, Model, Out.Refs);
                    }
                    else
                    {
                        ScanExpression(Attribute.Value, false, UseOf(Attribute.Name), Line, Model, Out.Refs);
                    }
                }

                // A stylesheet or script body is not markup, so its braces must not read as {{ }}.
                if (Tag == "style" || Tag == "script")
                {
                    const std::string Closing = "</" + std::string(Tag);
                    const size_t End = Text.find(Closing, Cursor);
                    Cursor = End == std::string_view::npos ? Text.size() : End;
                    continue;
                }
                if (!bSelfClosing && !IsVoidTag(Tag))
                {
                    Stack.push_back(Element);
                }
                continue;
            }

            // Text up to the next tag, where {{ }} interpolations live.
            size_t TextEnd = Text.find('<', Cursor + 1);
            if (TextEnd == std::string_view::npos)
            {
                TextEnd = Text.size();
            }
            const std::string_view Run = Text.substr(Cursor, TextEnd - Cursor);
            size_t Open = Run.find("{{");
            while (Open != std::string_view::npos)
            {
                const size_t Close = Run.find("}}", Open + 2);
                if (Close == std::string_view::npos)
                {
                    break;
                }
                ScanExpression(Run.substr(Open + 2, Close - Open - 2), false, ERefUse::Text, Lines.LineAt(Cursor + Open), CurrentModel(Stack), Out.Refs);
                Open = Run.find("{{", Close + 2);
            }
            Cursor = TextEnd;
        }
    }

    FUIDesignModel Infer(const FString& ModelName, const FDocumentBindings& Bindings)
    {
        FUIDesignModel Model;
        Model.Name = ModelName;
        const FStringView ModelView(ModelName.c_str(), ModelName.size());

        for (const FForScope& Scope : Bindings.ForScopes)
        {
            if (Scope.Model != ModelName || ContainsName(Model.Lists, FStringView(Scope.List.c_str(), Scope.List.size())))
            {
                continue;
            }
            FUIDesignList& List = Model.Lists.emplace_back();
            List.Name = Scope.List;
        }

        for (const FBindingRef& Ref : Bindings.Refs)
        {
            if (Ref.Model != ModelName)
            {
                continue;
            }
            const FStringView Name(Ref.Name.c_str(), Ref.Name.size());
            if (Ref.Kind == ERefKind::Command)
            {
                if (!ContainsName(Model.Commands, Name))
                {
                    Model.Commands.emplace_back().Name = Ref.Name;
                }
                continue;
            }
            if (Ref.Kind == ERefKind::ListSource)
            {
                continue;
            }

            if (const FForScope* Scope = Bindings.FindAlias(ModelView, Name))
            {
                if (Ref.Member.empty())
                {
                    continue;
                }
                for (FUIDesignList& List : Model.Lists)
                {
                    if (List.Name == Scope->List && std::find(List.Members.begin(), List.Members.end(), Ref.Member) == List.Members.end())
                    {
                        List.Members.push_back(Ref.Member);
                    }
                }
                continue;
            }

            if (ContainsName(Model.Lists, Name))
            {
                continue;
            }
            FUIDesignScalar* Existing = nullptr;
            for (FUIDesignScalar& Scalar : Model.Scalars)
            {
                if (Scalar.Name == Ref.Name)
                {
                    Existing = &Scalar;
                }
            }
            if (Existing != nullptr)
            {
                // Text wins, since a value that is both shown and tested reads best as its own name.
                if (Ref.Use == ERefUse::Text && Existing->Type != EUIVarType::String)
                {
                    Existing->Type = EUIVarType::String;
                    Existing->Value = Ref.Name;
                }
                continue;
            }

            FUIDesignScalar& Scalar = Model.Scalars.emplace_back();
            Scalar.Name = Ref.Name;
            Scalar.bWritable = true;
            switch (Ref.Use)
            {
                case ERefUse::Condition: Scalar.Type = EUIVarType::Bool;  Scalar.Value = "1";  break;
                case ERefUse::Style:     Scalar.Type = EUIVarType::Float; Scalar.Value = "50"; break;
                case ERefUse::Input:     Scalar.Type = EUIVarType::String; Scalar.Value = "";  break;
                default:                 Scalar.Type = EUIVarType::String; Scalar.Value = Ref.Name; break;
            }
        }

        FillPlaceholderRows(Model);
        return Model;
    }

    void FillPlaceholderRows(FUIDesignModel& Model)
    {
        for (FUIDesignList& List : Model.Lists)
        {
            if (!List.Rows.empty())
            {
                continue;
            }
            for (int32 Row = 1; Row <= 3; ++Row)
            {
                TVector<FString>& Cells = List.Rows.emplace_back();
                for (const FString& Member : List.Members)
                {
                    Cells.push_back(Member + " " + std::to_string(Row).c_str());
                }
                if (List.Members.empty())
                {
                    Cells.push_back(List.Name + " " + std::to_string(Row).c_str());
                }
            }
        }
    }

    void Lint(const FDocumentBindings& Bindings, const TVector<FUIDesignModel>& Models, const TVector<bool>& Described, TVector<FProblem>& Out)
    {
        Out.clear();
        auto Add = [&Out](int32 Line, FString Message, bool bError)
        {
            for (const FProblem& Existing : Out)
            {
                if (Existing.Line == Line && Existing.Message == Message)
                {
                    return;
                }
            }
            Out.push_back(FProblem{ Line, Move(Message), bError });
        };

        auto FindModel = [&](const FString& Name, bool& bOutDescribed) -> const FUIDesignModel*
        {
            for (size_t Index = 0; Index < Models.size(); ++Index)
            {
                if (Models[Index].Name == Name)
                {
                    bOutDescribed = Index < Described.size() && Described[Index];
                    return &Models[Index];
                }
            }
            bOutDescribed = false;
            return nullptr;
        };

        for (size_t Index = 0; Index < Bindings.Models.size(); ++Index)
        {
            bool bDescribed = false;
            FindModel(Bindings.Models[Index], bDescribed);
            if (!bDescribed)
            {
                Add(Bindings.ModelLines[Index], "No class registers as '" + Bindings.Models[Index]
                    + "', so the preview fills in placeholders. Name a class " + Bindings.Models[Index] + ", or give it [DataModel(\""
                    + Bindings.Models[Index] + "\")] in C# or REFLECT(DataModel = \"" + Bindings.Models[Index] + "\") in C++.", false);
            }
        }

        for (const FBindingRef& Ref : Bindings.Refs)
        {
            if (Ref.Model.empty())
            {
                Add(Ref.Line, "'" + Ref.Name + "' is bound outside any element with data-model, so nothing provides it.", true);
                continue;
            }
            bool bDescribed = false;
            const FUIDesignModel* Model = FindModel(Ref.Model, bDescribed);
            if (Model == nullptr || !bDescribed)
            {
                continue;
            }
            const FStringView Name(Ref.Name.c_str(), Ref.Name.size());
            const FString& TypeName = Model->SourceType.empty() ? Model->Name : Model->SourceType;

            if (Ref.Kind == ERefKind::Command)
            {
                if (!Model->HasCommand(Name))
                {
                    Add(Ref.Line, "'" + Ref.Name + "()' is not a [Bind] method of " + TypeName + ".", true);
                }
                continue;
            }
            if (Ref.Kind == ERefKind::ListSource)
            {
                if (!ContainsName(Model->Lists, Name))
                {
                    Add(Ref.Line, "'" + Ref.Name + "' is not a bound collection of " + TypeName + ".", true);
                }
                continue;
            }
            if (const FForScope* Scope = Bindings.FindAlias(FStringView(Ref.Model.c_str(), Ref.Model.size()), Name))
            {
                if (Ref.Member.empty() || Ref.Name == Scope->IndexAlias)
                {
                    continue;
                }
                for (const FUIDesignList& List : Model->Lists)
                {
                    if (List.Name == Scope->List && std::find(List.Members.begin(), List.Members.end(), Ref.Member) == List.Members.end())
                    {
                        Add(Ref.Line, "'" + Ref.Name + "." + Ref.Member + "' is not a [Bind] member of the items in " + List.Name + ".", true);
                    }
                }
                continue;
            }
            if (!Model->HasMember(Name))
            {
                Add(Ref.Line, "'" + Ref.Name + "' is not a [Bind] member of " + TypeName + ".", true);
                continue;
            }
            const bool bStruct = !Ref.Member.empty() && Model->FindScalar(Name) == nullptr && !ContainsName(Model->Lists, Name);
            if (bStruct)
            {
                const FString Field = Ref.Member.substr(0, Ref.Member.find_first_of(".["));
                const FString Path = Ref.Name + "." + Field;
                if (Model->FindScalar(FStringView(Path.c_str(), Path.size())) == nullptr)
                {
                    Add(Ref.Line, "'" + Path + "' is not a member of " + Ref.Name + ".", true);
                }
            }
        }

        std::stable_sort(Out.begin(), Out.end(), [](const FProblem& A, const FProblem& B) { return A.Line < B.Line; });
    }

    void ReadDataAttributes(std::string_view Text, size_t OpenLt, TVector<FAttribute>& Out)
    {
        Out.clear();
        if (OpenLt >= Text.size() || Text[OpenLt] != '<')
        {
            return;
        }
        size_t NameEnd = OpenLt + 1;
        while (NameEnd < Text.size() && (IsIdentChar(Text[NameEnd]) || Text[NameEnd] == '-'))
        {
            ++NameEnd;
        }
        TVector<FRawAttribute> Attributes;
        bool bSelfClosing = false;
        ParseAttributes(Text, NameEnd, Attributes, bSelfClosing);
        for (const FRawAttribute& Attribute : Attributes)
        {
            if (StartsWith(Attribute.Name, "data-"))
            {
                Out.push_back(FAttribute{ ToFString(Attribute.Name), ToFString(Attribute.Value) });
            }
        }
    }

    FCompletionContext ClassifyCompletion(std::string_view Prefix)
    {
        FCompletionContext Context;

        // A word typed right after "alias." completes the members of the items that alias walks.
        auto AliasBeforeDot = [&Prefix]() -> FString
        {
            if (Prefix.empty() || Prefix.back() != '.')
            {
                return FString();
            }
            size_t Start = Prefix.size() - 1;
            while (Start > 0 && IsIdentChar(Prefix[Start - 1]))
            {
                --Start;
            }
            return ToFString(Prefix.substr(Start, Prefix.size() - 1 - Start));
        };

        const size_t Open = Prefix.rfind("{{");
        const size_t Close = Prefix.rfind("}}");
        if (Open != std::string_view::npos && (Close == std::string_view::npos || Close < Open))
        {
            Context.Alias = AliasBeforeDot();
            Context.Kind = Context.Alias.empty() ? ECompletion::Value : ECompletion::AliasMember;
            return Context;
        }

        const size_t Lt = Prefix.rfind('<');
        const size_t Gt = Prefix.rfind('>');
        if (Lt == std::string_view::npos || (Gt != std::string_view::npos && Gt > Lt))
        {
            return Context;
        }

        // Walks the tag so far, and on each opening quote reads back over '=' to the attribute it belongs to.
        std::string_view Attribute;
        std::string_view Value;
        char Quote = 0;
        size_t ValueStart = 0;
        for (size_t Index = Lt + 1; Index < Prefix.size(); ++Index)
        {
            const char Character = Prefix[Index];
            if (Quote != 0)
            {
                if (Character == Quote)
                {
                    Quote = 0;
                }
                continue;
            }
            if (Character != '"' && Character != '\'')
            {
                continue;
            }
            Quote = Character;
            ValueStart = Index + 1;

            size_t Back = Index;
            while (Back > Lt && std::isspace(static_cast<unsigned char>(Prefix[Back - 1])))
            {
                --Back;
            }
            if (Back > Lt && Prefix[Back - 1] == '=')
            {
                --Back;
            }
            while (Back > Lt && std::isspace(static_cast<unsigned char>(Prefix[Back - 1])))
            {
                --Back;
            }
            const size_t NameEnd = Back;
            while (Back > Lt && !std::isspace(static_cast<unsigned char>(Prefix[Back - 1])) && Prefix[Back - 1] != '<')
            {
                --Back;
            }
            Attribute = Prefix.substr(Back, NameEnd - Back);
        }

        if (Quote == 0)
        {
            Context.Kind = ECompletion::Attribute;
            return Context;
        }

        Value = Prefix.substr(ValueStart);
        if (!StartsWith(Attribute, "data-"))
        {
            return Context;
        }
        if (Attribute == "data-model")
        {
            Context.Kind = ECompletion::Model;
            return Context;
        }
        if (Attribute == "data-for")
        {
            Context.Kind = Value.find(':') != std::string_view::npos ? ECompletion::ListSource : ECompletion::None;
            return Context;
        }
        Context.Alias = AliasBeforeDot();
        if (!Context.Alias.empty())
        {
            Context.Kind = ECompletion::AliasMember;
            return Context;
        }
        Context.Kind = StartsWith(Attribute, "data-event-") ? ECompletion::Command : ECompletion::Value;
        return Context;
    }

    const TVector<std::pair<const char*, const char*>>& KnownAttributes()
    {
        static const TVector<std::pair<const char*, const char*>> Attributes =
        {
            { "data-model",        "Names the data model this element and its children bind to" },
            { "data-if",           "Keeps the element only while the expression is true" },
            { "data-visible",      "Shows the element only while the expression is true, keeping its layout" },
            { "data-for",          "Repeats the element per item, as item : Collection" },
            { "data-text",         "Replaces the element's text with the expression" },
            { "data-rml",          "Replaces the element's markup with the expression" },
            { "data-value",        "Two-way binds an input's value" },
            { "data-checked",      "Two-way binds a checkbox or radio" },
            { "data-class-",       "Adds the named class while the expression is true" },
            { "data-style-",       "Sets the named style property to the expression" },
            { "data-attr-",        "Sets the named attribute to the expression" },
            { "data-attrif-",      "Adds the named attribute while the expression is true" },
            { "data-event-click",  "Runs a [Bind] method or assignment on click" },
            { "data-event-change", "Runs a [Bind] method or assignment when an input changes" },
            { "data-event-submit", "Runs a [Bind] method or assignment when a form submits" },
            { "data-event-mouseover", "Runs a [Bind] method or assignment on hover" },
            { "data-event-keydown", "Runs a [Bind] method or assignment on a key press" },
            { "data-alias-",       "Gives an expression a local name for a template" },
        };
        return Attributes;
    }
}
