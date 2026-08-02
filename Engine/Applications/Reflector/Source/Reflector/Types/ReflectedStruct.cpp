#include "ReflectedType.h"

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"
#include "Reflector/ReflectionCore/ReflectedHeader.h"
#include "Reflector/ReflectionCore/ReflectedProject.h"

namespace Lumina::Reflection
{

    FReflectedStruct::~FReflectedStruct() = default;

    void FReflectedStruct::PushProperty(eastl::unique_ptr<FReflectedProperty>&& NewProperty)
    {
        NewProperty->Outer = Namespace.empty() ? DisplayName : (Namespace + "::" + DisplayName);
        Props.push_back(eastl::move(NewProperty));
    }

    void FReflectedStruct::PushFunction(eastl::unique_ptr<FReflectedFunction>&& NewFunction)
    {
        NewFunction->Outer = Namespace.empty() ? DisplayName : (Namespace + "::" + DisplayName);
        Functions.push_back(eastl::move(NewFunction));
    }

    void FReflectedStruct::EmitMetadataArrays(FCodeWriter& Writer) const
    {
        // Type-level metadata.
        EmitMetadataArray(Writer, Names::FriendlyFromQualified(QualifiedName), Metadata);

        // Per-property metadata.
        for (const auto& Prop : Props)
        {
            EmitMetadataArray(Writer, Prop->Name, Prop->Metadata);
        }
    }

    void FReflectedStruct::EmitPropertyFieldDeclarations(FCodeWriter& Writer) const
    {
        for (const auto& Prop : Props)
        {
            Writer.Linef("static const Lumina::%s %s;", Prop->GetPropertyParamType(), Prop->Name.c_str());
        }
    }

    void FReflectedStruct::EmitPropertyDefinitions(FCodeWriter& Writer, eastl::string_view StaticsName)
    {
        // Accessor function bodies first (Getter/Setter wrappers + array wrappers).
        for (const auto& Prop : Props)
        {
            Prop->DefineAccessors(Writer, this);
        }

        // Then the FXxxPropertyParams literal for each property.
        for (const auto& Prop : Props)
        {
            Writer.Appendf("const Lumina::%s %s::%s = ",
                Prop->GetPropertyParamType(), eastl::string(StaticsName).c_str(), Prop->Name.c_str());
            Prop->AppendDefinition(Writer);
        }
    }

    void FReflectedStruct::EmitPropertyPointerTable(FCodeWriter& Writer, eastl::string_view StaticsName) const
    {
        Writer.Line();
        Writer.Linef("const Lumina::FPropertyParams* const %s::PropPointers[] = {",
            eastl::string(StaticsName).c_str());
        for (const auto& Prop : Props)
        {
            Writer.Linef("\t(const Lumina::FPropertyParams*)&%s::%s,",
                eastl::string(StaticsName).c_str(), Prop->Name.c_str());
        }
        Writer.Line("};");
        Writer.Line();
    }

    void FReflectedStruct::DefineInitialHeader(FCodeWriter& Writer, const eastl::string& /*FileID*/)
    {
        const eastl::string Api = Names::ProjectApiMacro(Header->Project->Name);
        const eastl::string ConstructFn = Names::ConstructFunction("CStruct", Namespace, DisplayName);

        if (!Namespace.empty())
        {
            Writer.Linef("namespace %s { struct %s; }", Namespace.c_str(), DisplayName.c_str());
        }
        else
        {
            Writer.Linef("\tclass %s;", DisplayName.c_str());
        }

        Writer.Linef("%s Lumina::CStruct* %s();", Api.c_str(), ConstructFn.c_str());
    }

    void FReflectedStruct::DefineSecondaryHeader(FCodeWriter& Writer, const eastl::string& FileID)
    {
        // ManualStub types are template-alias shims (e.g. FVector3 = TVec<float,3>);
        // no struct body to inject GENERATED_BODY into, so emit nothing.
        if (HasMetadata("ManualStub"))
        {
            Writer.BlankLines(2);
            return;
        }

        const bool bHasAccessors = DeclareAccessors(Writer, FileID);

        Writer.Linef("#define %s_%u_GENERATED_BODY \\", FileID.c_str(), GeneratedBodyLineNumber);
        if (bHasAccessors)
        {
            Writer.Macrof("%s_%u_ACCESSORS", FileID.c_str(), GeneratedBodyLineNumber);
        }
        // MinimalAPI: export StaticStruct() so the type is reflection-referenceable
        // across module boundaries without force-exporting every member.
        if (HasMetadata("MinimalAPI"))
        {
            const eastl::string Api = Names::ProjectApiMacro(Header->Project->Name);
            Writer.Macrof("static %s class Lumina::CStruct* StaticStruct();", Api.c_str());
        }
        else
        {
            Writer.Macro("static class Lumina::CStruct* StaticStruct();");
        }

        if (!Parent.empty())
        {
            Writer.Macrof("using Super = %s::%s;", Namespace.c_str(), Parent.c_str());
        }

        // Components register with EnTT using in_place_delete so handles survive
        // pool rearrangement.
        if (HasMetadata("Component"))
        {
            Writer.Macro("static constexpr auto in_place_delete = true;");
        }

        Writer.FinalizeMacro();
        Writer.BlankLines(2);
    }

    namespace
    {
        void EmitComponentMetaRegistrations(FCodeWriter& Writer, const FReflectedStruct& Struct)
        {
            for (const FMetadataPair& Data : Struct.Metadata)
            {
                if (Data.Key == "Component")
                {
                    Writer.Linef("::Lumina::Meta::RegisterComponentMeta<%s>();", Struct.QualifiedName.c_str());
                }
                else if (Data.Key == "System")
                {
                    Writer.Linef("::Lumina::Meta::RegisterECSSystem<%s>();", Struct.QualifiedName.c_str());
                }
                else if (Data.Key == "Event")
                {
                    Writer.Linef("::Lumina::Meta::RegisterECSEvent<%s>();", Struct.QualifiedName.c_str());
                }
            }
        }

        void EmitStructParams(FCodeWriter& Writer, const FReflectedStruct& Struct, eastl::string_view StaticsName)
        {
            const eastl::string MetadataSymbol = Names::FriendlyFromQualified(Struct.QualifiedName);

            Writer.Linef("const Lumina::FStructParams %s::StructParams = {",
                eastl::string(StaticsName).c_str());

            if (Struct.Parent.empty())
            {
                Writer.Line("\tnullptr,");
            }
            else
            {
                Writer.Linef("\t%s::Super::StaticStruct,", Struct.QualifiedName.c_str());
            }

            Writer.Line("\t&GetStructOps,");
            Writer.Linef("\t\"%s\",", Struct.DisplayName.c_str());

            if (!Struct.Props.empty())
            {
                Writer.Line("\tPropPointers,");
                Writer.Line("\t(uint32)std::size(PropPointers),");
            }
            else
            {
                Writer.Line("\tnullptr,");
                Writer.Line("\t0,");
            }

            Writer.Linef("\tsizeof(%s),", Struct.QualifiedName.c_str());
            Writer.Appendf("\talignof(%s)", Struct.QualifiedName.c_str());

            if (!Struct.Metadata.empty())
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
        }
    }

    void FReflectedStruct::DeclareImplementation(FCodeWriter& Writer)
    {
        const eastl::string RegInfo = Names::RegistrationInfo("CStruct", Namespace, DisplayName);
        const eastl::string ConstructFn = Names::ConstructFunction("CStruct", Namespace, DisplayName);
        const eastl::string Statics = Names::StaticsStruct("CStruct", Namespace, DisplayName);

        Writer.BlankLines(2);
        Writer.Linef("// Begin %s", DisplayName.c_str());
        Writer.Linef("static Lumina::FStructRegistrationInfo %s;", RegInfo.c_str());
        Writer.Line();

        // Statics struct body.
        Writer.Linef("struct %s", Statics.c_str());
        Writer.BeginBlock();

        Writer.Line("static Lumina::FStructOps* GetStructOps()");
        Writer.BeginBlock();
        Writer.Linef("return Lumina::MakeStructOps<%s>();", QualifiedName.c_str());
        Writer.EndBlock();
        Writer.Line();

        EmitMetadataArrays(Writer);
        Writer.Line();

        EmitPropertyFieldDeclarations(Writer);
        Writer.Line("static const Lumina::FStructParams StructParams;");

        if (!Props.empty())
        {
            Writer.Line("static const Lumina::FPropertyParams* const PropPointers[];");
        }

        Writer.Line();

        Writer.PopIndent();
        Writer.Line("};");
        Writer.Line();

        // Construct_CStruct_*
        Writer.Linef("Lumina::CStruct* %s()", ConstructFn.c_str());
        Writer.BeginBlock();
        Writer.Linef("if (!%s.InnerSingleton)", RegInfo.c_str());
        Writer.BeginBlock();
        Writer.Linef("Lumina::ConstructCStruct(&%s.InnerSingleton, %s::StructParams);",
            RegInfo.c_str(), Statics.c_str());
        EmitComponentMetaRegistrations(Writer, *this);
        Writer.EndBlock();
        Writer.Linef("return %s.InnerSingleton;", RegInfo.c_str());
        Writer.EndBlock();
        Writer.Line();

        // Outer singleton: QualifiedName::StaticStruct(). Skipped for ManualStub
        // types since the runtime alias has no such member.
        if (!HasMetadata("ManualStub"))
        {
            Writer.Linef("class Lumina::CStruct* %s::StaticStruct()", QualifiedName.c_str());
            Writer.BeginBlock();
            Writer.Linef("if (!%s.OuterSingleton)", RegInfo.c_str());
            Writer.BeginBlock();
            Writer.Linef("%s.OuterSingleton = %s();", RegInfo.c_str(), ConstructFn.c_str());
            Writer.EndBlock();
            Writer.Linef("return %s.OuterSingleton;", RegInfo.c_str());
            Writer.EndBlock();
        }

        // Property definitions + accessor impls + PropPointers table.
        if (!Props.empty())
        {
            EmitPropertyDefinitions(Writer, Statics);
            EmitPropertyPointerTable(Writer, Statics);
        }

        // FStructParams singleton definition.
        EmitStructParams(Writer, *this, Statics);

        Writer.Linef("//~ End %s", DisplayName.c_str());
        Writer.Line();
        Writer.Line("//------------------------------------------------------------");
        Writer.Line();
    }

    void FReflectedStruct::DeclareStaticRegistration(FCodeWriter& Writer)
    {
        const eastl::string ConstructFn = Names::ConstructFunction("CStruct", Namespace, DisplayName);
        Writer.Linef("\t{ %s, TEXT(\"%s\") },", ConstructFn.c_str(), DisplayName.c_str());
    }
}
