#include "ReflectedType.h"

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"
#include "Reflector/ReflectionCore/ReflectedHeader.h"
#include "Reflector/ReflectionCore/ReflectedProject.h"

namespace Lumina::Reflection
{

    void FReflectedEnum::DefineInitialHeader(FCodeWriter& Writer, const eastl::string& /*FileID*/)
    {
        const eastl::string Api = Names::ProjectApiMacro(Header->Project->Name);
        const eastl::string ConstructFn = Names::ConstructFunction("CEnum", Namespace, DisplayName);

        Writer.Linef("enum class %s : uint8;", DisplayName.c_str());
        Writer.Linef("%s Lumina::CEnum* %s();", Api.c_str(), ConstructFn.c_str());
        Writer.Linef("template<> Lumina::CEnum* StaticEnum<%s>();", DisplayName.c_str());
        Writer.Line();
    }

    void FReflectedEnum::DefineSecondaryHeader(FCodeWriter& /*Writer*/, const eastl::string& /*FileID*/)
    {
        // Enums don't have a GENERATED_BODY expansion - StaticEnum is a template
        // specialization declared in the initial header.
    }

    void FReflectedEnum::DeclareImplementation(FCodeWriter& Writer)
    {
        const eastl::string RegInfo = Names::RegistrationInfo("CEnum", Namespace, DisplayName);
        const eastl::string ConstructFn = Names::ConstructFunction("CEnum", Namespace, DisplayName);
        const eastl::string Statics = Names::StaticsStruct("CEnum", Namespace, DisplayName);
        const eastl::string MetadataSymbol = Names::FriendlyFromQualified(QualifiedName);

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

        // StaticEnum<T>() outer singleton.
        Writer.Linef("template<> Lumina::CEnum* StaticEnum<%s>()", DisplayName.c_str());
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
        const eastl::string ConstructFn = Names::ConstructFunction("CEnum", Namespace, DisplayName);
        Writer.Linef("\t{ %s, TEXT(\"%s\") },", ConstructFn.c_str(), DisplayName.c_str());
    }
}
