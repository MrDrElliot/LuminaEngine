#pragma once
#include <memory>
#include <string>
#include <vector>

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
    void EmitMetadataArray(FCodeWriter& Writer, std::string_view SymbolBase, const std::vector<FMetadataPair>& Metadata);

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
        virtual std::string GetTypeName() const = 0;

        virtual void DefineInitialHeader(FCodeWriter& Writer, const std::string& FileID) = 0;
        virtual void DefineSecondaryHeader(FCodeWriter& Writer, const std::string& FileID) = 0;
        virtual void DeclareImplementation(FCodeWriter& Writer) = 0;
        virtual void DeclareStaticRegistration(FCodeWriter& Writer) = 0;

        /// The identifier emitted declarations must use; DisplayName unless ReflectedName aliased it.
        const std::string& EmittedCppName() const { return CppName.empty() ? DisplayName : CppName; }

        // Declaring or defining a member needs the real class name; a type expression may use the alias.
        const std::string& EmittedCppQualifiedName() const { return CppQualifiedName.empty() ? QualifiedName : CppQualifiedName; }

        bool HasMetadata(const std::string& Meta) const;

        /// Value for Key, or nullptr when absent.
        const std::string* TryGetMetadata(const std::string& Key) const;
        void GenerateMetadata(const std::string& InMetadata);

        // Common helper that writes "<FileID>_<Line>_ACCESSORS" macro body when any
        // property has Getter/Setter metadata. Returns true if the macro was emitted.
        bool DeclareAccessors(FCodeWriter& Writer, const std::string& FileID);

        std::vector<std::unique_ptr<FReflectedProperty>>    Props;
        std::vector<std::unique_ptr<FReflectedFunction>>    Functions;
        std::vector<FMetadataPair>                            Metadata;
        FReflectedHeader*                                       Header = nullptr;
        std::string                                           DisplayName;

        /// The real C++ identifier, which differs from DisplayName when ReflectedName aliases the type.
        std::string                                           CppName;
        std::string                                           CppQualifiedName;

        // The name the type is registered and looked up under: the ReflectedName alias when one is set.
        std::string                                           QualifiedName;
        std::string                                           Namespace;
        uint32_t                                                GeneratedBodyLineNumber = 0;
        uint32_t                                                LineNumber = 0;
        // True if the type has a non-static data member with NO PROPERTY() macro. Such state is invisible to
        // the reflector, so the type's reflected layout is incomplete and it must NOT be mirrored by value in
        // C# (a flat by-value struct would be the wrong size). Set by the struct field visitor.
        bool                                                    bHasUnreflectedFields = false;

        // Reflected through a REFLECT'd `using`, so nothing declares the name and it has no injected body.
        bool                                                    bIsAlias = false;
        EType                                                   Type = EType::Structure;
    };

    class FReflectedEnum : public FReflectedType
    {
    public:

        struct FConstant
        {
            std::string ID;
            std::string Label;
            std::string Description;
            uint32_t      Value = 0;
        };

        FReflectedEnum() { Type = EType::Enum; }

        std::string GetTypeName() const override { return "CEnum"; }

        void DefineInitialHeader(FCodeWriter& Writer, const std::string& FileID) override;
        void DefineSecondaryHeader(FCodeWriter& Writer, const std::string& FileID) override;
        void DeclareImplementation(FCodeWriter& Writer) override;
        void DeclareStaticRegistration(FCodeWriter& Writer) override;

        void AddConstant(const FConstant& Constant) { Constants.push_back(Constant); }

        std::vector<FConstant> Constants;
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

        void PushProperty(std::unique_ptr<FReflectedProperty>&& NewProperty);
        void PushFunction(std::unique_ptr<FReflectedFunction>&& NewFunction);

        std::string GetTypeName() const override { return "CStruct"; }

        void DefineInitialHeader(FCodeWriter& Writer, const std::string& FileID) override;
        void DefineSecondaryHeader(FCodeWriter& Writer, const std::string& FileID) override;
        void DeclareImplementation(FCodeWriter& Writer) override;
        void DeclareStaticRegistration(FCodeWriter& Writer) override;

        // The type offsetof is taken against, aliased when the real spelling carries template arguments.
        std::string OffsetBaseTypeName() const;

        // Shared helpers consumed by FReflectedClass too.
        void EmitMetadataArrays(FCodeWriter& Writer) const;
        void EmitPropertyFieldDeclarations(FCodeWriter& Writer) const;
        void EmitPropertyDefinitions(FCodeWriter& Writer, std::string_view StaticsName);
        void EmitPropertyPointerTable(FCodeWriter& Writer, std::string_view StaticsName) const;

        std::string Parent;
    };

    class FReflectedClass : public FReflectedStruct
    {
    public:

        FReflectedClass() { Type = EType::Class; }

        std::string GetTypeName() const override { return "CClass"; }

        void DefineInitialHeader(FCodeWriter& Writer, const std::string& FileID) override;
        void DefineSecondaryHeader(FCodeWriter& Writer, const std::string& FileID) override;
        void DeclareImplementation(FCodeWriter& Writer) override;
        void DeclareStaticRegistration(FCodeWriter& Writer) override;
    };
}
