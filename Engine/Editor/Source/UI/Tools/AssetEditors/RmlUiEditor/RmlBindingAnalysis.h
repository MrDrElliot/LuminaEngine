#pragma once

#include <string_view>
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "UI/UIDesignModel.h"

namespace Lumina::RmlBinding
{
    enum class ERefKind : uint8
    {
        Value,
        Command,
        ListSource,
    };

    // Where a bound name appears, which decides the placeholder type the preview gives a member no script describes.
    enum class ERefUse : uint8
    {
        Text,
        Condition,
        Style,
        Input,
        Event,
        Other,
    };

    // One name the markup reads from a data model, with the alias member when it is read through a data-for alias.
    struct FBindingRef
    {
        FString  Model;
        FString  Name;
        FString  Member;
        ERefKind Kind = ERefKind::Value;
        ERefUse  Use = ERefUse::Other;
        int32    Line = 1;
    };

    // A data-for loop, which makes Alias and IndexAlias names inside it rather than model members.
    struct FForScope
    {
        FString Model;
        FString Alias;
        FString IndexAlias;
        FString List;
        int32   Line = 1;
    };

    struct FDocumentBindings
    {
        TVector<FString>     Models;
        TVector<int32>       ModelLines;
        TVector<FBindingRef> Refs;
        TVector<FForScope>   ForScopes;

        bool IsAlias(FStringView Model, FStringView Name) const;
        const FForScope* FindAlias(FStringView Model, FStringView Name) const;
    };

    struct FProblem
    {
        int32   Line = 1;
        FString Message;
        bool    bError = true;
    };

    // One data-* attribute of an element's open tag.
    struct FAttribute
    {
        FString Name;
        FString Value;
    };

    // Reads every binding in an .rml, attributing each to the data-model that encloses it.
    void Scan(std::string_view Text, FDocumentBindings& Out);

    // A stand-in model built from what the markup reads, for a data-model no C# type describes.
    FUIDesignModel Infer(const FString& ModelName, const FDocumentBindings& Bindings);

    // Adds to a described model any list rows it lacks, so a data-for still shows something.
    void FillPlaceholderRows(FUIDesignModel& Model);

    // Names the markup reads that the C# model does not have. Described says which models came from a script.
    void Lint(const FDocumentBindings& Bindings, const TVector<FUIDesignModel>& Models, const TVector<bool>& Described, TVector<FProblem>& Out);

    // The data-* attributes of the open tag starting at OpenLt.
    void ReadDataAttributes(std::string_view Text, size_t OpenLt, TVector<FAttribute>& Out);

    // What the text before the caret is in the middle of, for autocomplete.
    enum class ECompletion : uint8
    {
        None,
        Value,
        Command,
        Model,
        ListSource,
        AliasMember,
        Attribute,
    };

    struct FCompletionContext
    {
        ECompletion Kind = ECompletion::None;
        FString     Alias;
    };

    // Prefix is the line up to the start of the word being typed.
    FCompletionContext ClassifyCompletion(std::string_view Prefix);

    // The data-* attributes RmlUi understands, for completing an attribute name. Each pairs the name with a short description.
    const TVector<std::pair<const char*, const char*>>& KnownAttributes();
}
