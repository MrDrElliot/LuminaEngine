#include "ReflectedType.h"

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"
#include "Reflector/ReflectionCore/ReflectedHeader.h"
#include "Reflector/ReflectionCore/ReflectedProject.h"

namespace Lumina::Reflection
{
    void FReflectedClass::DefineInitialHeader(FCodeWriter& Writer, const std::string& FileID)
    {
        const std::string Api = Names::ProjectApiMacro(Header->Project->Name);
        const std::string Package = Names::ScriptPackage(Header->Project->Name);
        const std::string ConstructFn = Names::ConstructFunction("CClass", Namespace, DisplayName);

        if (!Namespace.empty())
        {
            Writer.Linef("namespace %s { class %s; }", Namespace.c_str(), DisplayName.c_str());
        }
        else
        {
            Writer.Linef("\tclass %s;", DisplayName.c_str());
        }

        Writer.Linef("%s Lumina::CClass* %s();", Api.c_str(), ConstructFn.c_str());

        // DECLARE_CLASS block used by GENERATED_BODY.
        Writer.Linef("#define %s_%u_CLASS \\", FileID.c_str(), LineNumber);
        Writer.Macro("private:");
        Writer.Macro("");
        Writer.Macro("public:");
        // MinimalAPI exports only GetPrivateStaticClass() so the type is reflection-referenceable
        // cross-module without force-exporting every member.
        const char* ClassApi = HasMetadata("MinimalAPI") ? Api.c_str() : "NO_API";
        Writer.Macrof("\tDECLARE_CLASS(%s, %s, %s, \"%s\", %s)",
            Namespace.c_str(), DisplayName.c_str(), Parent.c_str(), Package.c_str(), ClassApi);
        Writer.Macrof("\tDEFINE_CLASS_FACTORY(%s::%s)", Namespace.c_str(), DisplayName.c_str());
        Writer.Macrof("\tDECLARE_SERIALIZER(%s, %s)", Namespace.c_str(), DisplayName.c_str());
        Writer.FinalizeMacro();
        Writer.BlankLines(2);
    }

    void FReflectedClass::DefineSecondaryHeader(FCodeWriter& Writer, const std::string& FileID)
    {
        const bool bHasAccessors = DeclareAccessors(Writer, FileID);

        Writer.Linef("#define %s_%u_GENERATED_BODY \\", FileID.c_str(), GeneratedBodyLineNumber);
        Writer.Macro("public:");
        if (bHasAccessors)
        {
            Writer.Macrof("%s_%u_ACCESSORS", FileID.c_str(), GeneratedBodyLineNumber);
        }
        Writer.Macrof("\t%s_%u_CLASS", FileID.c_str(), LineNumber);
        Writer.Macro("private:");
        Writer.FinalizeMacro();
        Writer.BlankLines(2);
    }
    
    namespace
    {
        void EmitClassParams(FCodeWriter& Writer, const FReflectedClass& Class, std::string_view StaticsName)
        {
            const std::string MetadataSymbol = Names::FriendlyFromQualified(Class.QualifiedName);

            Writer.Linef("const Lumina::FClassParams %s::ClassParams = {", StaticsName.data());
            Writer.Linef("\t&%s::%s::StaticClass,", Class.Namespace.c_str(), Class.DisplayName.c_str());

            if (!Class.Props.empty())
            {
                Writer.Line("\tPropPointers,");
                Writer.Append("\t(uint32)std::size(PropPointers),");
            }
            else
            {
                Writer.Line("\tnullptr,");
                Writer.Append("\t0,");
            }

            if (!Class.Metadata.empty())
            {
                Writer.Line();
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

    void FReflectedClass::DeclareImplementation(FCodeWriter& Writer)
    {
        const std::string ConstructFn = Names::ConstructFunction("CClass", Namespace, DisplayName);
        const std::string Statics = Names::StaticsStruct("CClass", Namespace, DisplayName);
        const std::string RegInfo = Names::RegistrationInfo("CClass", Namespace, DisplayName);

        Writer.Linef("// Begin %s", DisplayName.c_str());
        Writer.Linef("IMPLEMENT_CLASS(%s, %s)", Namespace.c_str(), DisplayName.c_str());

        // Statics struct.
        Writer.Linef("struct %s", Statics.c_str());
        Writer.BeginBlock();

        EmitMetadataArrays(Writer);
        Writer.Line();

        EmitPropertyFieldDeclarations(Writer);
        Writer.Line("//...");
        Writer.Line();

        Writer.Line("static const Lumina::FClassParams ClassParams;");
        if (!Props.empty())
        {
            Writer.Line("static const Lumina::FPropertyParams* const PropPointers[];");
        }

        Writer.PopIndent();
        Writer.Line("};");
        Writer.Line();

        // Construct_CClass_*: classes populate OuterSingleton here (IMPLEMENT_CLASS
        // owns InnerSingleton).
        Writer.Linef("Lumina::CClass* %s()", ConstructFn.c_str());
        Writer.BeginBlock();
        Writer.Linef("if (!%s.OuterSingleton)", RegInfo.c_str());
        Writer.BeginBlock();
        Writer.Linef("Lumina::ConstructCClass(&%s.OuterSingleton, %s::ClassParams);", RegInfo.c_str(), Statics.c_str());
        Writer.EndBlock();
        Writer.Linef("return %s.OuterSingleton;", RegInfo.c_str());
        Writer.EndBlock();
        Writer.Line();

        if (!Props.empty())
        {
            EmitPropertyDefinitions(Writer, Statics);
            EmitPropertyPointerTable(Writer, Statics);
        }

        EmitClassParams(Writer, *this, Statics);

        Writer.Linef("//~ End %s", DisplayName.c_str());
        Writer.Line();
        Writer.Line("//------------------------------------------------------------");
        Writer.Line();
    }

    void FReflectedClass::DeclareStaticRegistration(FCodeWriter& Writer)
    {
        const std::string ConstructFn = Names::ConstructFunction("CClass", Namespace, DisplayName);
        Writer.Linef("\t{ %s, TEXT(\"/Script\"), TEXT(\"%s\") },",
            ConstructFn.c_str(), DisplayName.c_str());
    }
}
