#pragma once

#include <string>
#include <vector>
#include "Reflector/Types/PropertyFlags.h"
#include "Reflector/Types/StructReflectItem.h"
#include "Reflector/Utils/MetadataUtils.h"


namespace Lumina::Reflection
{
    class FReflectedType;
    class FCodeWriter;
}

namespace Lumina
{
    /**
     * Base for every reflected property kind (numeric, string, struct, object, array, etc.).
     * */
    class FReflectedProperty : public IStructReflectable
    {
    public:

        virtual ~FReflectedProperty() = default;

        virtual const char* GetPropertyParamType() const { return "FPropertyParams"; }
        virtual const char* GetTypeName() = 0;
        virtual std::string_view GetLuaType() = 0;

        virtual void AppendDefinition(Reflection::FCodeWriter& Writer) const = 0;

        virtual bool CanDeclareCrossModuleReferences() const { return false; }
        virtual void DeclareCrossModuleReference(const std::string& API, Reflection::FCodeWriter& Writer) { }

        virtual bool HasAccessors();
        virtual bool DeclareAccessors(Reflection::FCodeWriter& Writer, const std::string& FileID);
        virtual bool DefineAccessors(Reflection::FCodeWriter& Writer, Reflection::FReflectedType* ReflectedType);

        bool GenerateLuaBinding(Reflection::FCodeWriter& Writer) override;
        void GenerateMetadata(const std::string& InMetadata) override;

        // True when two of this property's specifiers contradict each other, with a message naming both
        // and which one to remove. Call after GenerateMetadata, from somewhere holding the cursor, so the
        // diagnostic points at the declaration rather than the file.
        bool FindConflictingSpecifiers(std::string& OutMessage) const;

        std::string GetDisplayName() const { return Name; }

        // Emits the trailing `{ "Name", Flags, TypeFlags, Setter, Getter, Offset[, CustomData][, METADATA_PARAMS] };`
        // shared by every property kind.
        void AppendPropertyDef(Reflection::FCodeWriter& Writer, const char* PropertyFlagsStr, const char* TypeFlags, const std::string& CustomData = "") const;

        std::vector<FMetadataPair>    Metadata;
        EPropertyFlags                  PropertyFlags;
        std::string                   RawTypeName;
        std::string                   TypeName;
        std::string                   Namespace;
        std::string                   Name;
        std::string                   Outer;

        // "Owner::" normally, a free-function symbol prefix when the owner has no body to declare them in.
        std::string                   AccessorScope;

        // The same prefix as spelled at the definition site, which sits inside the owner's namespace.
        std::string                   AccessorDefinitionScope;
        std::string                   GetterFunc;
        std::string                   SetterFunc;

        bool                            bInner = false;
    };
}
