#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Reflector/Types/Functions/ReflectedFunction.h"
#include "Reflector/Types/Properties/ReflectedProperty.h"
#include "Reflector/Public/PropertyFlags.h"
#include "Reflector/ReflectionSpecifiers.h"
#include "Reflector/Utils/MetadataUtils.h"

namespace Lumina::Reflection
{
    class FReflectedHeader;
    class FReflectedProject;

    // Maps a source-level C++ type name onto EPropertyTypeFlags; used by the clang visitors classifying a field.
    EPropertyTypeFlags GetCoreTypeFromName(std::string_view Name);

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

        /// The identifier emitted declarations must use; DisplayName unless ReflectedName aliased it.
        const std::string& EmittedCppName() const { return CppName.empty() ? DisplayName : CppName; }

        // Declaring or defining a member needs the real class name; a type expression may use the alias.
        const std::string& EmittedCppQualifiedName() const { return CppQualifiedName.empty() ? QualifiedName : CppQualifiedName; }

        bool HasMetadata(std::string_view Meta) const;
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

        std::string Parent;
    };

    class FReflectedClass : public FReflectedStruct
    {
    public:

        FReflectedClass() { Type = EType::Class; }

        std::string GetTypeName() const override { return "CClass"; }

    };
}
