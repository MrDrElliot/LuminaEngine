#include "ReflectedType.h"

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"
#include "Reflector/ReflectionCore/ReflectedHeader.h"
#include "Reflector/ReflectionCore/ReflectedProject.h"

namespace Lumina::Reflection
{
    namespace
    {
        // The forward declaration must name the SAME underlying type as the real one or MSVC rejects it C3433.
        const char* UnderlyingTypeName(uint32_t Size, bool bUnsigned)
        {
            switch (Size)
            {
            case 1:  return bUnsigned ? "uint8"  : "int8";
            case 2:  return bUnsigned ? "uint16" : "int16";
            case 8:  return bUnsigned ? "uint64" : "int64";
            default: return bUnsigned ? "uint32" : "int32";
            }
        }
    }

    void FReflectedEnum::DefineInitialHeader(FCodeWriter& Writer, const std::string& /*FileID*/)
    {
        const std::string Api = Names::ProjectApiMacro(Header->Project->Name);
        const std::string ConstructFn = Names::ConstructFunction("CEnum", Namespace, DisplayName);

        // At global scope this would create a second distinct type and any TU seeing both gets C2872.
        const char* Underlying = UnderlyingTypeName(UnderlyingSize, bUnsignedUnderlying);

        if (!Namespace.empty())
        {
            Writer.Linef("namespace %s { enum class %s : %s; }",
                Namespace.c_str(), DisplayName.c_str(), Underlying);
        }
        else
        {
            Writer.Linef("enum class %s : %s;", DisplayName.c_str(), Underlying);
        }

        Writer.Linef("%s Lumina::CEnum* %s();", Api.c_str(), ConstructFn.c_str());

        // Qualified, since this specialization is emitted at global scope where an unqualified name misses.
        Writer.Linef("template<> Lumina::CEnum* StaticEnum<%s>();", QualifiedName.c_str());
        Writer.Line();
    }

    void FReflectedEnum::DefineSecondaryHeader(FCodeWriter& /*Writer*/, const std::string& /*FileID*/)
    {
        // Enums have no GENERATED_BODY, since StaticEnum is a specialization declared in the header.
    }

    void FReflectedEnum::DeclareImplementation(FCodeWriter& Writer)
    {
        const std::string RegInfo = Names::RegistrationInfo("CEnum", Namespace, DisplayName);
        const std::string ConstructFn = Names::ConstructFunction("CEnum", Namespace, DisplayName);
        const std::string Statics = Names::StaticsStruct("CEnum", Namespace, DisplayName);
        const std::string MetadataSymbol = Names::FriendlyFromQualified(QualifiedName);

        // Translation-unit-local singleton holder.
        Writer.Linef("static Lumina::FEnumRegistrationInfo %s;", RegInfo.c_str());
        Writer.Line();

        // Statics struct
        Writer.Linef("struct %s", Statics.c_str());
        Writer.BeginBlock();

        EmitMetadataArray(Writer, MetadataSymbol, Metadata);

        // Enumerator list.
        Writer.Line("static constexpr Lumina::FEnumeratorParam Enumerators[] = {");
        for (const FConstant& Constant : Constants)
        {
            Writer.Linef("\t{ \"%s::%s\", %u },",
                DisplayName.c_str(), Constant.Label.c_str(), Constant.Value);
        }
        Writer.Line("};");
        Writer.Line();

        Writer.Line("static const Lumina::FEnumParams EnumParams;");
        Writer.PopIndent();
        Writer.Line("};");

        // EnumParams definition.
        Writer.Linef("const Lumina::FEnumParams %s::EnumParams = {", Statics.c_str());
        Writer.Linef("\t\"%s\",", DisplayName.c_str());
        Writer.Line("\tEnumerators,");
        Writer.Append("\t(uint32)std::size(Enumerators)");

        if (!Metadata.empty())
        {
            Writer.Line(",");
            Writer.Linef("\t(uint32)std::size(%s_Metadata),", MetadataSymbol.c_str());
            Writer.Linef("\t%s_Metadata", MetadataSymbol.c_str());
        }
        else
        {
            Writer.Line();
        }

        Writer.Line("};");
        Writer.Line();

        // Construct_CEnum_* inner singleton.
        Writer.Linef("Lumina::CEnum* %s()", ConstructFn.c_str());
        Writer.BeginBlock();
        Writer.Linef("if(!%s.InnerSingleton)", RegInfo.c_str());
        Writer.BeginBlock();
        Writer.Linef("Lumina::ConstructCEnum(&%s.InnerSingleton, %s::EnumParams);",
            RegInfo.c_str(), Statics.c_str());
        Writer.EndBlock();
        Writer.Linef("return %s.InnerSingleton;", RegInfo.c_str());
        Writer.EndBlock();
        Writer.Line();

        // Qualified to match the declaration, or this defines a specialization for a different type.
        Writer.Linef("template<> Lumina::CEnum* StaticEnum<%s>()", QualifiedName.c_str());
        Writer.BeginBlock();
        Writer.Linef("if (!%s.OuterSingleton)", RegInfo.c_str());
        Writer.BeginBlock();
        Writer.Linef("%s.OuterSingleton = %s();", RegInfo.c_str(), ConstructFn.c_str());
        Writer.EndBlock();
        Writer.Linef("return %s.OuterSingleton;", RegInfo.c_str());
        Writer.EndBlock();
    }

    void FReflectedEnum::DeclareStaticRegistration(FCodeWriter& Writer)
    {
        const std::string ConstructFn = Names::ConstructFunction("CEnum", Namespace, DisplayName);
        Writer.Linef("\t{ %s, TEXT(\"%s\") },", ConstructFn.c_str(), DisplayName.c_str());
    }
}
