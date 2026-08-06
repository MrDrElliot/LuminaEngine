#pragma once

#include <EASTL/string.h>
#include "EASTL/vector.h"
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
        virtual eastl::string_view GetLuaType() = 0;

        virtual void AppendDefinition(Reflection::FCodeWriter& Writer) const = 0;

        virtual bool CanDeclareCrossModuleReferences() const { return false; }
        virtual void DeclareCrossModuleReference(const eastl::string& API, Reflection::FCodeWriter& Writer) { }

        virtual bool HasAccessors();
        virtual bool DeclareAccessors(Reflection::FCodeWriter& Writer, const eastl::string& FileID);
        virtual bool DefineAccessors(Reflection::FCodeWriter& Writer, Reflection::FReflectedType* ReflectedType);

        bool GenerateLuaBinding(Reflection::FCodeWriter& Writer) override;
        void GenerateMetadata(const eastl::string& InMetadata) override;

        // True when two of this property's specifiers contradict each other, with a message naming both
        // and which one to remove. Call after GenerateMetadata, from somewhere holding the cursor, so the
        // diagnostic points at the declaration rather than the file.
        bool FindConflictingSpecifiers(eastl::string& OutMessage) const;

        eastl::string GetDisplayName() const { return Name; }

        // Emits the trailing `{ "Name", Flags, TypeFlags, Setter, Getter, Offset[, CustomData][, METADATA_PARAMS] };`
        // shared by every property kind.
        void AppendPropertyDef(Reflection::FCodeWriter& Writer, const char* PropertyFlagsStr, const char* TypeFlags, const eastl::string& CustomData = "") const;

        eastl::vector<FMetadataPair>    Metadata;
        EPropertyFlags                  PropertyFlags;
        eastl::string                   RawTypeName;
        eastl::string                   TypeName;
        eastl::string                   Namespace;
        eastl::string                   Name;
        eastl::string                   Outer;
        eastl::string                   GetterFunc;
        eastl::string                   SetterFunc;

        bool                            bInner = false;
    };
}
