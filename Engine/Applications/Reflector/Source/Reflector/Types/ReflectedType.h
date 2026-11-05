#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Reflector/Types/Functions/ReflectedFunction.h"
#include "Reflector/Types/Properties/ReflectedProperty.h"
#include "Reflector/Types/PropertyFlags.h"
#include "Reflector/ReflectionSpecifiers.h"
#include "Reflector/Utils/MetadataUtils.h"

namespace Lumina::Reflection
{
    class FReflectedHeader;
    class FReflectedProject;
    class FCodeWriter;

    // Maps a source-level C++ type name onto EPropertyTypeFlags; used by the clang visitors classifying a field.
    EPropertyTypeFlags GetCoreTypeFromName(std::string_view Name);

    // A static FMetadataEntry array named <SymbolBase>_Metadata holding what Target stores as metadata, entries every build keeps first.
    void EmitMetadataArray(FCodeWriter& Writer, std::string_view SymbolBase, const std::vector<FMetadataPair>& Metadata, ESpecifierTarget Target);

    // The trailing LUMINA_METADATA field of a descriptor, or nothing when Target stores no metadata.
    std::string MetadataField(std::string_view SymbolBase, const std::vector<FMetadataPair>& Metadata, ESpecifierTarget Target);

    // Everything the reflector emits, an enum, struct or class.
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

        // "CClass", "CStruct" or "CEnum", the runtime type that describes this one.
        virtual std::string GetTypeName() const = 0;

        virtual void DefineInitialHeader(FCodeWriter& Writer, const std::string& FileID) = 0;
        virtual void DefineSecondaryHeader(FCodeWriter& Writer, const std::string& FileID) = 0;
        virtual void DeclareImplementation(FCodeWriter& Writer) = 0;

        // The struct holding this type's descriptor, declared in the type's own namespace so a class can befriend it.
        std::string TypeInfoName() const { return "FTypeInfo_" + DisplayName; }
        std::string TypeInfoQualifiedName() const { return Namespace.empty() ? ("::" + TypeInfoName()) : ("::" + Namespace + "::" + TypeInfoName()); }

        // The function that builds this type, named without the type so a module table can list it by declaration alone.
        std::string GetTypeFunctionName() const;

        // The accessor generated code and traits reach the type through, StaticClass, StaticStruct or StaticEnum.
        const char* AccessorName() const;

        void OpenNamespace(FCodeWriter& Writer) const;
        void CloseNamespace(FCodeWriter& Writer) const;

        // GetType_<Name> and the accessor specialization that forwards to it. ExtraConstruction runs once, after the descriptor is built.
        void EmitTypeFunctions(FCodeWriter& Writer, const std::string& ExtraConstruction = {}) const;

        /// The identifier emitted declarations must use; DisplayName unless ReflectedName aliased it.
        const std::string& EmittedCppName() const { return CppName.empty() ? DisplayName : CppName; }

        // Declaring or defining a member needs the real class name; a type expression may use the alias.
        const std::string& EmittedCppQualifiedName() const { return CppQualifiedName.empty() ? QualifiedName : CppQualifiedName; }

        bool HasMetadata(std::string_view Meta) const;

        /// Value for Key, or nullptr when absent.
        const std::string* TryGetMetadata(std::string_view Key) const;
        void GenerateMetadata(const std::string& InMetadata);

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

        // Derives from ECS::FResource, so it registers as a world resource and C# reaches it in place.
        bool                                                    bIsResource = false;

        // The compiled layout as clang computes it for the build target, or -1 when the type was incomplete.
        int64_t                                                 NativeSize = -1;
        int64_t                                                 NativeAlign = -1;
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

        /// Takes back every property added from FirstIndex onward, keeping the order they were added in.
        /// A container parameter adds its inners as well as itself, and all of them belong to the function.
        std::vector<std::unique_ptr<FReflectedProperty>> TakeBackPropertiesFrom(size_t FirstIndex);

        size_t NumProperties() const { return Props.size(); }

        std::string GetTypeName() const override { return "CStruct"; }

        void DefineInitialHeader(FCodeWriter& Writer, const std::string& FileID) override;
        void DefineSecondaryHeader(FCodeWriter& Writer, const std::string& FileID) override;
        void DeclareImplementation(FCodeWriter& Writer) override;

        // The type offsetof is taken against, aliased when the real spelling carries template arguments.
        std::string OffsetBaseTypeName() const;

        // Shared with FReflectedClass, whose descriptor differs only in what registers it.
        void EmitMetadataArrays(FCodeWriter& Writer) const;
        void EmitPropertyTable(FCodeWriter& Writer) const;
        void DefinePropertyTable(FCodeWriter& Writer) const;

        // Holds the real compiler to the clang layout the C# bindings bake in as constants.
        void EmitLayoutAsserts(FCodeWriter& Writer) const;

        // The descriptor fields naming the tables above, empty for a table that was not emitted.
        std::string MemberTableFields() const;

        bool HasNativeFunctions() const;

        // The frame struct and thunk for each function, at file scope since offsetof is taken against them.
        void EmitFunctionFrames(FCodeWriter& Writer) const;
        void EmitFunctionTable(FCodeWriter& Writer) const;
        void DefineFunctionParameterTables(FCodeWriter& Writer) const;
        void DefineFunctionTable(FCodeWriter& Writer) const;

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
    };
}
