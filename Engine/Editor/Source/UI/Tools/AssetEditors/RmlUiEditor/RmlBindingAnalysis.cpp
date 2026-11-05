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
            return FString(Text.data(), IntCastChecked<int32>(Text.size()));
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
                        Newlines.Add(Index);
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
                    Out.Add(Attribute);
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
                    Ref.Member.Reset();
                }
                Out.Add(Ref);
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
            Out.ForScopes.Add(Scope);

            FBindingRef Ref;
            Ref.Model = Model;
            Ref.Name = Scope.List;
            Ref.Kind = ERefKind::ListSource;
            Ref.Line = Line;
            Out.Refs.Add(Ref);
        }

        struct FOpenElement
        {
            std::string_view Tag;
            FString          Model;
        };

        FString CurrentModel(const TVector<FOpenElement>& Stack)
        {
            for (int32 Index = Stack.Num(); Index > 0; --Index)
            {
                if (!Stack[Index - 1].Model.IsEmpty())
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
                if (FStringView(*Item.Name, Item.Name.Len()) == Name)
                {
                    return true;
                }
            }
            return false;
        }
    }


    bool FDocumentBindings::IsAlias(FStringView Model, FStringView Name) const
    {
        return FindAlias(Model, Name) != nullptr;
    }

    const FForScope* FDocumentBindings::FindAlias(FStringView Model, FStringView Name) const
    {
        for (const FForScope& Scope : ForScopes)
        {
            if (FStringView(*Scope.Model, Scope.Model.Len()) != Model)
            {
                continue;
            }
            if (FStringView(*Scope.Alias, Scope.Alias.Len()) == Name || FStringView(*Scope.IndexAlias, Scope.IndexAlias.Len()) == Name)
            {
                return &Scope;
            }
        }
        return nullptr;
    }

    void Scan(std::string_view Text, FDocumentBindings& Out)
    {
        Out = FDocumentBindings();
        const Scanning::FLineIndex Lines(Text);
        TVector<Scanning::FOpenElement> Stack;

        size_t Cursor = 0;
        while (Cursor < Text.size())
        {
            const std::string_view Rest = Text.substr(Cursor);
            if (Scanning::StartsWith(Rest, "<!--"))
            {
                const size_t End = Text.find("-->", Cursor + 4);
                Cursor = End == std::string_view::npos ? Text.size() : End + 3;
                continue;
            }
            if (Scanning::StartsWith(Rest, "<![CDATA["))
            {
                const size_t End = Text.find("]]>", Cursor);
                Cursor = End == std::string_view::npos ? Text.size() : End + 3;
                continue;
            }
            if (Text[Cursor] == '<' && Cursor + 1 < Text.size() && Text[Cursor + 1] == '/')
            {
                size_t NameEnd = Cursor + 2;
                while (NameEnd < Text.size() && Scanning::IsIdentChar(Text[NameEnd]))
                {
                    ++NameEnd;
                }
                const std::string_view Tag = Text.substr(Cursor + 2, NameEnd - Cursor - 2);
                for (int32 Index = Stack.Num(); Index > 0; --Index)
                {
                    if (Stack[Index - 1].Tag == Tag)
                    {
                        Stack.SetNum(Index - 1);
                        break;
                    }
                }
                const size_t Close = Text.find('>', NameEnd);
                Cursor = Close == std::string_view::npos ? Text.size() : Close + 1;
                continue;
            }
            if (Text[Cursor] == '<' && Cursor + 1 < Text.size() && Scanning::IsIdentStart(Text[Cursor + 1]))
            {
                size_t NameEnd = Cursor + 1;
                while (NameEnd < Text.size() && (Scanning::IsIdentChar(Text[NameEnd]) || Text[NameEnd] == '-'))
                {
                    ++NameEnd;
                }
                const std::string_view Tag = Text.substr(Cursor + 1, NameEnd - Cursor - 1);

                TVector<Scanning::FRawAttribute> Attributes;
                bool bSelfClosing = false;
                Cursor = Scanning::ParseAttributes(Text, NameEnd, Attributes, bSelfClosing);

                Scanning::FOpenElement Element;
                Element.Tag = Tag;
                for (const Scanning::FRawAttribute& Attribute : Attributes)
                {
                    if (Attribute.Name == "data-model")
                    {
                        Element.Model = Scanning::ToFString(Scanning::Trim(Attribute.Value));
                        Out.Models.Add(Element.Model);
                        Out.ModelLines.Add(Lines.LineAt(Attribute.ValueOffset));
                    }
                }

                const FString Model = Element.Model.IsEmpty() ? Scanning::CurrentModel(Stack) : Element.Model;
                for (const Scanning::FRawAttribute& Attribute : Attributes)
                {
                    if (!Scanning::StartsWith(Attribute.Name, "data-") || Attribute.Name == "data-model" || Scanning::StartsWith(Attribute.Name, "data-alias-"))
                    {
                        continue;
                    }
                    const int32 Line = Lines.LineAt(Attribute.ValueOffset);
                    if (Attribute.Name == "data-for")
                    {
                        Scanning::ScanForScope(Attribute.Value, Line, Model, Out);
                    }
                    else if (Scanning::StartsWith(Attribute.Name, "data-event-"))
                    {
                        Scanning::ScanExpression(Attribute.Value, true, ERefUse::Event, Line, Model, Out.Refs);
                    }
                    else
                    {
                        Scanning::ScanExpression(Attribute.Value, false, Scanning::UseOf(Attribute.Name), Line, Model, Out.Refs);
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
                if (!bSelfClosing && !Scanning::IsVoidTag(Tag))
                {
                    Stack.Add(Element);
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
                Scanning::ScanExpression(Run.substr(Open + 2, Close - Open - 2), false, ERefUse::Text, Lines.LineAt(Cursor + Open), Scanning::CurrentModel(Stack), Out.Refs);
                Open = Run.find("{{", Close + 2);
            }
            Cursor = TextEnd;
        }
    }

    FUIDesignModel Infer(const FString& ModelName, const FDocumentBindings& Bindings)
    {
        FUIDesignModel Model;
        Model.Name = ModelName;
        const FStringView ModelView(*ModelName, ModelName.Len());

        for (const FForScope& Scope : Bindings.ForScopes)
        {
            if (Scope.Model != ModelName || Scanning::ContainsName(Model.Lists, FStringView(*Scope.List, Scope.List.Len())))
            {
                continue;
            }
            FUIDesignList& List = Model.Lists.EmplaceGetRef();
            List.Name = Scope.List;
        }

        for (const FBindingRef& Ref : Bindings.Refs)
        {
            if (Ref.Model != ModelName)
            {
                continue;
            }
            const FStringView Name(*Ref.Name, Ref.Name.Len());
            if (Ref.Kind == ERefKind::Command)
            {
                if (!Scanning::ContainsName(Model.Commands, Name))
                {
                    Model.Commands.EmplaceGetRef().Name = Ref.Name;
                }
                continue;
            }
            if (Ref.Kind == ERefKind::ListSource)
            {
                continue;
            }

            if (const FForScope* Scope = Bindings.FindAlias(ModelView, Name))
            {
                if (Ref.Member.IsEmpty())
                {
                    continue;
                }
                for (FUIDesignList& List : Model.Lists)
                {
                    if (List.Name == Scope->List && std::find(List.Members.begin(), List.Members.end(), Ref.Member) == List.Members.end())
                    {
                        List.Members.Add(Ref.Member);
                    }
                }
                continue;
            }

            if (Scanning::ContainsName(Model.Lists, Name))
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

            FUIDesignScalar& Scalar = Model.Scalars.EmplaceGetRef();
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
            if (!List.Rows.IsEmpty())
            {
                continue;
            }
            for (int32 Row = 1; Row <= 3; ++Row)
            {
                TVector<FString>& Cells = List.Rows.EmplaceGetRef();
                for (const FString& Member : List.Members)
                {
                    Cells.Add(Member + " " + std::to_string(Row).c_str());
                }
                if (List.Members.IsEmpty())
                {
                    Cells.Add(List.Name + " " + std::to_string(Row).c_str());
                }
            }
        }
    }

    void Lint(const FDocumentBindings& Bindings, const TVector<FUIDesignModel>& Models, const TVector<bool>& Described, TVector<FProblem>& Out)
    {
        Out.Reset();
        auto Add = [&Out](int32 Line, FString Message, bool bError)
        {
            for (const FProblem& Existing : Out)
            {
                if (Existing.Line == Line && Existing.Message == Message)
                {
                    return;
                }
            }
            Out.Emplace(Line, Move(Message), bError);
        };

        auto FindModel = [&](const FString& Name, bool& bOutDescribed) -> const FUIDesignModel*
        {
            for (int32 Index = 0; Index < Models.Num(); ++Index)
            {
                if (Models[Index].Name == Name)
                {
                    bOutDescribed = Index < Described.Num() && Described[Index];
                    return &Models[Index];
                }
            }
            bOutDescribed = false;
            return nullptr;
        };

        for (int32 Index = 0; Index < Bindings.Models.Num(); ++Index)
        {
            bool bDescribed = false;
            FindModel(Bindings.Models[Index], bDescribed);
            if (!bDescribed)
            {
                Add(Bindings.ModelLines[Index], "No class registers as '" + Bindings.Models[Index]
                    + "', so the preview fills in placeholders. Name a class " + Bindings.Models[Index] + ", or give it [Reflect(DataModel = \""
                    + Bindings.Models[Index] + "\")] in C# or REFLECT(DataModel = \"" + Bindings.Models[Index] + "\") in C++.", false);
            }
        }

        for (const FBindingRef& Ref : Bindings.Refs)
        {
            if (Ref.Model.IsEmpty())
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
            const FStringView Name(*Ref.Name, Ref.Name.Len());
            const FString& TypeName = Model->SourceType.IsEmpty() ? Model->Name : Model->SourceType;

            if (Ref.Kind == ERefKind::Command)
            {
                if (!Model->HasCommand(Name))
                {
                    Add(Ref.Line, "'" + Ref.Name + "()' is not a [Function(Bind)] method of " + TypeName + ".", true);
                }
                continue;
            }
            if (Ref.Kind == ERefKind::ListSource)
            {
                if (!Scanning::ContainsName(Model->Lists, Name))
                {
                    Add(Ref.Line, "'" + Ref.Name + "' is not a bound collection of " + TypeName + ".", true);
                }
                continue;
            }
            if (const FForScope* Scope = Bindings.FindAlias(FStringView(*Ref.Model, Ref.Model.Len()), Name))
            {
                if (Ref.Member.IsEmpty() || Ref.Name == Scope->IndexAlias)
                {
                    continue;
                }
                for (const FUIDesignList& List : Model->Lists)
                {
                    if (List.Name == Scope->List && std::find(List.Members.begin(), List.Members.end(), Ref.Member) == List.Members.end())
                    {
                        Add(Ref.Line, "'" + Ref.Name + "." + Ref.Member + "' is not a [Property(Bind)] member of the items in " + List.Name + ".", true);
                    }
                }
                continue;
            }
            if (!Model->HasMember(Name))
            {
                Add(Ref.Line, "'" + Ref.Name + "' is not a [Property(Bind)] member of " + TypeName + ".", true);
                continue;
            }
            const bool bStruct = !Ref.Member.IsEmpty() && Model->FindScalar(Name) == nullptr && !Scanning::ContainsName(Model->Lists, Name);
            if (bStruct)
            {
                const int32 FieldEnd = Ref.Member.FindFirstOf(".[");
                const FString Field = FieldEnd == Constants::kIndexNone ? Ref.Member : Ref.Member.Left(FieldEnd);
                const FString Path = Ref.Name + "." + Field;
                if (Model->FindScalar(FStringView(*Path, Path.Len())) == nullptr)
                {
                    Add(Ref.Line, "'" + Path + "' is not a member of " + Ref.Name + ".", true);
                }
            }
        }

        std::stable_sort(Out.begin(), Out.end(), [](const FProblem& A, const FProblem& B) { return A.Line < B.Line; });
    }

    void ReadDataAttributes(std::string_view Text, size_t OpenLt, TVector<FAttribute>& Out)
    {
        Out.Reset();
        if (OpenLt >= Text.size() || Text[OpenLt] != '<')
        {
            return;
        }
        size_t NameEnd = OpenLt + 1;
        while (NameEnd < Text.size() && (Scanning::IsIdentChar(Text[NameEnd]) || Text[NameEnd] == '-'))
        {
            ++NameEnd;
        }
        TVector<Scanning::FRawAttribute> Attributes;
        bool bSelfClosing = false;
        Scanning::ParseAttributes(Text, NameEnd, Attributes, bSelfClosing);
        for (const Scanning::FRawAttribute& Attribute : Attributes)
        {
            if (Scanning::StartsWith(Attribute.Name, "data-"))
            {
                Out.Emplace(Scanning::ToFString(Attribute.Name), Scanning::ToFString(Attribute.Value));
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
            while (Start > 0 && Scanning::IsIdentChar(Prefix[Start - 1]))
            {
                --Start;
            }
            return Scanning::ToFString(Prefix.substr(Start, Prefix.size() - 1 - Start));
        };

        const size_t Open = Prefix.rfind("{{");
        const size_t Close = Prefix.rfind("}}");
        if (Open != std::string_view::npos && (Close == std::string_view::npos || Close < Open))
        {
            Context.Alias = AliasBeforeDot();
            Context.Kind = Context.Alias.IsEmpty() ? ECompletion::Value : ECompletion::AliasMember;
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
        if (!Scanning::StartsWith(Attribute, "data-"))
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
        if (!Context.Alias.IsEmpty())
        {
            Context.Kind = ECompletion::AliasMember;
            return Context;
        }
        Context.Kind = Scanning::StartsWith(Attribute, "data-event-") ? ECompletion::Command : ECompletion::Value;
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
            { "data-event-click",  "Runs a Bind function or assignment on click" },
            { "data-event-change", "Runs a Bind function or assignment when an input changes" },
            { "data-event-submit", "Runs a Bind function or assignment when a form submits" },
            { "data-event-mouseover", "Runs a Bind function or assignment on hover" },
            { "data-event-keydown", "Runs a Bind function or assignment on a key press" },
            { "data-alias-",       "Gives an expression a local name for a template" },
        };
        return Attributes;
    }
}
