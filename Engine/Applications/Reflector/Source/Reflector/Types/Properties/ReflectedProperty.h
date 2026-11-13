#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "Reflector/Public/PropertyFlags.h"
#include "Reflector/Types/StructReflectItem.h"
#include "Reflector/Utils/MetadataUtils.h"

namespace Lumina
{
    class FReflectedProperty : public IStructReflectable
    {
    public:

        virtual ~FReflectedProperty() = default;

        virtual const char* GetTypeName() const = 0;

        virtual EReflectedLink GetLink() const { return EReflectedLink::None; }

        // Whether TypeName must be in the database, which an intrinsic class such as CObject never is.
        virtual bool RequiresReflectedType() const { return GetLink() != EReflectedLink::None; }

        void GenerateMetadata(const std::string& InMetadata) override;

        // Names the two contradicting specifiers and which to drop. Runs after GenerateMetadata, where the cursor is still at hand.
        bool FindConflictingSpecifiers(std::string& OutMessage) const;

        // Editable unless the author asked for ReadOnly or Hidden, the same default a C# [Property] has.
        void ApplyDefaultVisibility();

        bool HasSpecifier(std::string_view Key) const;

        std::vector<FMetadataPair>    Metadata;
        EPropertyFlags                PropertyFlags;
        std::string                   RawTypeName;
        std::string                   TypeName;
        std::string                   Namespace;
        std::string                   Name;

        // The type offsetof and decltype are taken against, the owner or a function's frame.
        std::string                   Outer;
        std::string                   GetterFunc;
        std::string                   SetterFunc;

        // The type a ReflectAs specifier presents the member as, in place of the one it is declared with.
        std::string                   ReflectedAsType;

        // An element, key, value, payload or argument, described by the trait of what holds it.
        bool                          bInner = false;

        // Byte offset inside the owning type as clang lays it out, or -1 for an inner or an unknown layout.
        int64_t                       NativeOffset = -1;

        uint32_t                      LineNumber = 0;
    };
}
