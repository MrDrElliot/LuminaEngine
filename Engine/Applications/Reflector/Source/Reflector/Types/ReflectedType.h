#pragma once
#include <EASTL/shared_ptr.h>
#include <EASTL/string.h>
#include <EASTL/vector.h>

#include "Reflector/Types/Functions/ReflectedFunction.h"
#include "Reflector/Types/Properties/ReflectedProperty.h"
#include "Reflector/Types/PropertyFlags.h"
#include "Reflector/Utils/MetadataUtils.h"

namespace Lumina::Reflection
{
    class FReflectedHeader;
    class FReflectedProject;
    class FCodeWriter;

    // Maps a source-level C++ type name onto EPropertyTypeFlags; used by the clang visitors classifying a field.
    EPropertyTypeFlags GetCoreTypeFromName(const char* Name);

    // Writes a static FMetaDataPairParam array named <SymbolBase>_Metadata, or nothing when there
    // is no metadata. Shared by the enum and struct emitters, which need identical output.
    void EmitMetadataArray(FCodeWriter& Writer, eastl::string_view SymbolBase, const eastl::vector<FMetadataPair>& Metadata);

    // Abstract base for everything the reflector emits (enum/struct/class).
    // Concrete types fill the four emission slots: DefineInitialHeader/DefineSecondaryHeader/DeclareImplementation/DeclareStaticRegistration.
    class FReflectedType
    {
    public:

        enum class EType : uint8_t
        {
            Class,
            Structure,
            Enum,
        };

        virtual ~FReflectedType() = default;

        // Returns "CClass", "CStruct", or "CEnum". Used to form Construct_* symbol names.
        virtual eastl::string GetTypeName() const = 0;

        virtual void DefineInitialHeader(FCodeWriter& Writer, const eastl::string& FileID) = 0;
        virtual void DefineSecondaryHeader(FCodeWriter& Writer, const eastl::string& FileID) = 0;
        virtual void DeclareImplementation(FCodeWriter& Writer) = 0;
        virtual void DeclareStaticRegistration(FCodeWriter& Writer) = 0;

        bool HasMetadata(const eastl::string& Meta) const;
        void GenerateMetadata(const eastl::string& InMetadata);

        // Common helper that writes "<FileID>_<Line>_ACCESSORS" macro body when any
        // property has Getter/Setter metadata. Returns true if the macro was emitted.
        bool DeclareAccessors(FCodeWriter& Writer, const eastl::string& FileID);

        eastl::vector<eastl::unique_ptr<FReflectedProperty>>    Props;
        eastl::vector<eastl::unique_ptr<FReflectedFunction>>    Functions;
        eastl::vector<FMetadataPair>                            Metadata;
        FReflectedHeader*                                       Header = nullptr;
        eastl::string                                           DisplayName;
        eastl::string                                           QualifiedName;
        eastl::string                                           Namespace;
        uint32_t                                                GeneratedBodyLineNumber = 0;
        uint32_t                                                LineNumber = 0;
        // True if the type has a non-static data member with NO PROPERTY() macro. Such state is invisible to
        // the reflector, so the type's reflected layout is incomplete and it must NOT be mirrored by value in
        // C# (a flat by-value struct would be the wrong size). Set by the struct field visitor.
        bool                                                    bHasUnreflectedFields = false;
        EType                                                   Type = EType::Structure;
    };

    class FReflectedEnum : public FReflectedType
    {
    public:

        struct FConstant
        {
            eastl::string ID;
            eastl::string Label;
            eastl::string Description;
            uint32_t      Value = 0;
        };

        FReflectedEnum() { Type = EType::Enum; }

        eastl::string GetTypeName() const override { return "CEnum"; }

        void DefineInitialHeader(FCodeWriter& Writer, const eastl::string& FileID) override;
        void DefineSecondaryHeader(FCodeWriter& Writer, const eastl::string& FileID) override;
        void DeclareImplementation(FCodeWriter& Writer) override;
        void DeclareStaticRegistration(FCodeWriter& Writer) override;

        void AddConstant(const FConstant& Constant) { Constants.push_back(Constant); }

        eastl::vector<FConstant> Constants;
        // Size in bytes of the enum's underlying integer type. The generated C# enum is emitted with a matching
        // explicit backing type (byte/ushort/uint/ulong or the signed forms), so an enum of any width can sit
        // inside a blittable by-value struct mirror; ComputeBlittableLayout uses this as the field size/align.
        uint32_t                 UnderlyingSize = 4;
        // True when the underlying integer type is unsigned (uint8/uint16/...); selects the C# backing type's
        // signedness. Default scoped enums (no fixed type) are int-backed, hence signed.
        bool                     bUnsignedUnderlying = false;
    };

    class FReflectedStruct : public FReflectedType
    {
    public:

        FReflectedStruct() { Type = EType::Structure; }
        ~FReflectedStruct() override;

        void PushProperty(eastl::unique_ptr<FReflectedProperty>&& NewProperty);
        void PushFunction(eastl::unique_ptr<FReflectedFunction>&& NewFunction);

        eastl::string GetTypeName() const override { return "CStruct"; }

        void DefineInitialHeader(FCodeWriter& Writer, const eastl::string& FileID) override;
        void DefineSecondaryHeader(FCodeWriter& Writer, const eastl::string& FileID) override;
        void DeclareImplementation(FCodeWriter& Writer) override;
        void DeclareStaticRegistration(FCodeWriter& Writer) override;

        // Shared helpers consumed by FReflectedClass too.
        void EmitMetadataArrays(FCodeWriter& Writer) const;
        void EmitPropertyFieldDeclarations(FCodeWriter& Writer) const;
        void EmitPropertyDefinitions(FCodeWriter& Writer, eastl::string_view StaticsName);
        void EmitPropertyPointerTable(FCodeWriter& Writer, eastl::string_view StaticsName) const;

        eastl::string Parent;
    };

    class FReflectedClass : public FReflectedStruct
    {
    public:

        FReflectedClass() { Type = EType::Class; }

        eastl::string GetTypeName() const override { return "CClass"; }

        void DefineInitialHeader(FCodeWriter& Writer, const eastl::string& FileID) override;
        void DefineSecondaryHeader(FCodeWriter& Writer, const eastl::string& FileID) override;
        void DeclareImplementation(FCodeWriter& Writer) override;
        void DeclareStaticRegistration(FCodeWriter& Writer) override;
    };
}
