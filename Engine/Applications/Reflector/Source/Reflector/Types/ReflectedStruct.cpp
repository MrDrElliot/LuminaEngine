#include "ReflectedType.h"

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"
#include "Reflector/ReflectionCore/ReflectedHeader.h"
#include "Reflector/ReflectionCore/ReflectedProject.h"

namespace Lumina::Reflection
{

    FReflectedStruct::~FReflectedStruct() = default;

    // A template argument list carries commas, which the offsetof macro would read as extra arguments.
    std::string FReflectedStruct::OffsetBaseTypeName() const
    {
        const std::string& Cpp = EmittedCppQualifiedName();
        return Cpp.find('<') == std::string::npos
            ? Cpp
            : ("LRT_Owner_" + Names::FriendlyFromParts(Namespace, DisplayName));
    }

    void FReflectedStruct::PushProperty(std::unique_ptr<FReflectedProperty>&& NewProperty)
    {
        NewProperty->Outer = OffsetBaseTypeName();

        if (bIsAlias)
        {
            NewProperty->AccessorDefinitionScope = DisplayName + "_";
            NewProperty->AccessorScope = Namespace.empty()
                ? NewProperty->AccessorDefinitionScope
                : Namespace + "::" + NewProperty->AccessorDefinitionScope;
        }
        else
        {
            NewProperty->AccessorScope = EmittedCppQualifiedName() + "::";
            NewProperty->AccessorDefinitionScope = NewProperty->AccessorScope;
        }

        Props.push_back(std::move(NewProperty));
    }

    void FReflectedStruct::PushFunction(std::unique_ptr<FReflectedFunction>&& NewFunction)
    {
        NewFunction->Outer = Namespace.empty() ? DisplayName : (Namespace + "::" + DisplayName);
        Functions.push_back(std::move(NewFunction));
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

    void FReflectedStruct::EmitPropertyDefinitions(FCodeWriter& Writer, std::string_view StaticsName)
    {
        // Free-function wrappers go in the owner's namespace, where its members' type names resolve.
        const bool bWrapInNamespace = bIsAlias && !Namespace.empty();
        if (bWrapInNamespace)
        {
            Writer.Linef("namespace %s", Namespace.c_str());
            Writer.BeginBlock();
        }

        // Accessor function bodies first (Getter/Setter wrappers + array wrappers).
        for (const auto& Prop : Props)
        {
            Prop->DefineAccessors(Writer, this);
        }

        if (bWrapInNamespace)
        {
            Writer.EndBlock();
            Writer.Line();
        }

        // Then the FXxxPropertyParams literal for each property.
        for (const auto& Prop : Props)
        {
            Writer.Appendf("const Lumina::%s %s::%s = ",
                Prop->GetPropertyParamType(), std::string(StaticsName).c_str(), Prop->Name.c_str());
            Prop->AppendDefinition(Writer);
        }
    }

    void FReflectedStruct::EmitPropertyPointerTable(FCodeWriter& Writer, std::string_view StaticsName) const
    {
        Writer.Line();
        Writer.Linef("const Lumina::FPropertyParams* const %s::PropPointers[] = {",
            std::string(StaticsName).c_str());
        for (const auto& Prop : Props)
        {
            Writer.Linef("\t(const Lumina::FPropertyParams*)&%s::%s,",
                std::string(StaticsName).c_str(), Prop->Name.c_str());
        }
        Writer.Line("};");
        Writer.Line();
    }

    void FReflectedStruct::DefineInitialHeader(FCodeWriter& Writer, const std::string& /*FileID*/)
    {
        const std::string Api = Names::ProjectApiMacro(Header->Project->Name);
        const std::string ConstructFn = Names::ConstructFunction("CStruct", Namespace, DisplayName);

        // An alias name belongs to the `using`, so forward declaring it as a struct would redeclare it.
        if (!bIsAlias)
        {
            if (!Namespace.empty())
            {
                Writer.Linef("namespace %s { struct %s; }", Namespace.c_str(), EmittedCppName().c_str());
            }
            else
            {
                Writer.Linef("\tclass %s;", EmittedCppName().c_str());
            }
        }

        Writer.Linef("%s Lumina::CStruct* %s();", Api.c_str(), ConstructFn.c_str());
    }

    void FReflectedStruct::DefineSecondaryHeader(FCodeWriter& Writer, const std::string& FileID)
    {
        // An alias has no struct body to inject GENERATED_BODY into.
        if (bIsAlias)
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
        // Exports StaticStruct() so the type is referenceable without force-exporting every member.
        if (HasMetadata("MinimalAPI"))
        {
            const std::string Api = Names::ProjectApiMacro(Header->Project->Name);
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
                    Writer.Linef("::Lumina::Meta::RegisterComponentMeta<%s>();", Struct.EmittedCppQualifiedName().c_str());
                }
                else if (Data.Key == "System")
                {
                    Writer.Linef("::Lumina::Meta::RegisterECSSystem<%s>();", Struct.EmittedCppQualifiedName().c_str());
                }
            }
        }

        void EmitStructParams(FCodeWriter& Writer, const FReflectedStruct& Struct, std::string_view StaticsName)
        {
            const std::string MetadataSymbol = Names::FriendlyFromQualified(Struct.QualifiedName);

            Writer.Linef("const Lumina::FStructParams %s::StructParams = {",
                std::string(StaticsName).c_str());

            if (Struct.Parent.empty())
            {
                Writer.Line("\tnullptr,");
            }
            else
            {
                Writer.Linef("\t%s::Super::StaticStruct,", Struct.EmittedCppQualifiedName().c_str());
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

            Writer.Linef("\tsizeof(%s),", Struct.EmittedCppQualifiedName().c_str());
            Writer.Appendf("\talignof(%s)", Struct.EmittedCppQualifiedName().c_str());

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
        const std::string RegInfo = Names::RegistrationInfo("CStruct", Namespace, DisplayName);
        const std::string ConstructFn = Names::ConstructFunction("CStruct", Namespace, DisplayName);
        const std::string Statics = Names::StaticsStruct("CStruct", Namespace, DisplayName);

        const std::string OffsetBase = OffsetBaseTypeName();
        if (OffsetBase != EmittedCppQualifiedName())
        {
            Writer.Line();
            Writer.Linef("using %s = %s;", OffsetBase.c_str(), EmittedCppQualifiedName().c_str());
        }

        Writer.BlankLines(2);
        Writer.Linef("// Begin %s", DisplayName.c_str());
        Writer.Linef("static Lumina::FStructRegistrationInfo %s;", RegInfo.c_str());
        Writer.Line();

        // Statics struct body.
        Writer.Linef("struct %s", Statics.c_str());
        Writer.BeginBlock();

        Writer.Line("static Lumina::FStructOps* GetStructOps()");
        Writer.BeginBlock();
        Writer.Linef("return Lumina::MakeStructOps<%s>();", EmittedCppQualifiedName().c_str());
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

        // The outer singleton is a member of QualifiedName, and an alias has no such member to define.
        if (!bIsAlias)
        {
            Writer.Linef("class Lumina::CStruct* %s::StaticStruct()", EmittedCppQualifiedName().c_str());
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
        const std::string ConstructFn = Names::ConstructFunction("CStruct", Namespace, DisplayName);
        Writer.Linef("\t{ %s, TEXT(\"%s\") },", ConstructFn.c_str(), DisplayName.c_str());
    }
}
