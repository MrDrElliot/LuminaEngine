#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "Reflector/Public/ReflectorApi.h"

namespace Lumina::Reflection
{
    // Which reflection macro a specifier may appear inside.
    enum class ESpecifierTarget : uint8_t
    {
        Reflect,
        Property,
        Function,
        ScriptExport,
        Size,
    };

    // How a specifier is written at the call site.
    enum class ESpecifierForm : uint8_t
    {
        Flag,   // bare keyword, PROPERTY(Editable)
        Value,  // key and value, PROPERTY(Category = "Rendering")
        Either, // legal both ways, PROPERTY(Getter) or PROPERTY(Getter = "GetFoo")
        Number, // key and numeric value, PROPERTY(ClampMin = 0.5f)
    };

    // Which layer reads the specifier after the Reflector has parsed it.
    enum class ESpecifierConsumer : uint8_t
    {
        Reflector, // changes the generated C++
        Runtime,   // read through CStruct/FProperty metadata at runtime
        Editor,    // drives the property grid or another editor surface only
        Script,    // shapes the generated C# binding
    };

    // Where a specifier lands in generated code.
    enum class ESpecifierStorage : uint8_t
    {
        None,           // consumed by the Reflector or turned into an EPropertyFlags bit
        Metadata,       // a metadata entry every build keeps
        EditorMetadata, // a metadata entry only the editor keeps
        EditorFlag,     // an EPropertyEditorFlags bit
    };

    // Name and Documentation must outlive the run, which string literals do.
    struct FSpecifierInfo
    {
        std::string_view   Name;
        ESpecifierForm     Form;
        ESpecifierConsumer Consumer;
        std::string_view   Documentation;
    };

    // Every specifier and metadata key the run's generators claimed, which is all validation accepts.
    class LRT_API FSpecifierRegistry
    {
    public:

        // False when another generator already claimed the name for Target, which keeps the first claim.
        bool Claim(ESpecifierTarget Target, const FSpecifierInfo& Info);

        const FSpecifierInfo* Find(ESpecifierTarget Target, std::string_view Key) const;

        // Nearest claimed specifier within a small edit distance, or nullptr. Turns a typo into a suggestion.
        const FSpecifierInfo* Suggest(ESpecifierTarget Target, std::string_view Key) const;

        ESpecifierStorage GetStorage(ESpecifierTarget Target, std::string_view Key) const;

        std::span<const FSpecifierInfo> GetClaimed(ESpecifierTarget Target) const { return Claimed[static_cast<size_t>(Target)]; }

    private:

        // Sorted by name, so every lookup is a binary search.
        std::vector<FSpecifierInfo> Claimed[static_cast<size_t>(ESpecifierTarget::Size)];
    };
}
