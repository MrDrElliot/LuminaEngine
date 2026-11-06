#include "ReflectedType.h"
#include <algorithm>

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
        Props.push_back(std::move(NewProperty));
    }

    void FReflectedStruct::PushFunction(std::unique_ptr<FReflectedFunction>&& NewFunction)
    {
        NewFunction->Outer = Namespace.empty() ? DisplayName : (Namespace + "::" + DisplayName);
        Functions.push_back(std::move(NewFunction));
    }

    std::vector<std::unique_ptr<FReflectedProperty>> FReflectedStruct::TakeBackPropertiesFrom(size_t FirstIndex)
    {
        std::vector<std::unique_ptr<FReflectedProperty>> Taken;
        if (FirstIndex >= Props.size())
        {
            return Taken;
        }

        Taken.reserve(Props.size() - FirstIndex);
        for (size_t Index = FirstIndex; Index < Props.size(); ++Index)
        {
            Taken.push_back(std::move(Props[Index]));
        }
        Props.erase(Props.begin() + (std::ptrdiff_t)FirstIndex, Props.end());

        return Taken;
    }

    void FReflectedStruct::EmitMetadataArrays(FCodeWriter& Writer) const
    {
        EmitMetadataArray(Writer, Names::FriendlyFromQualified(QualifiedName), Metadata, ESpecifierTarget::Reflect);

        for (const auto& Prop : Props)
        {
            if (!Prop->bInner)
            {
                EmitMetadataArray(Writer, Prop->Name, Prop->Metadata, ESpecifierTarget::Property);
            }
        }

        for (const auto& Function : Functions)
        {
            if (Function->bReflectNatively)
            {
                EmitMetadataArray(Writer, "Func_" + Function->Name, Function->Metadata, ESpecifierTarget::Function);
            }
        }
    }

    void FReflectedStruct::EmitPropertyTable(FCodeWriter& Writer) const
    {
        const size_t Count = std::count_if(Props.begin(), Props.end(), [](const auto& Prop) { return !Prop->bInner; });
        if (Count == 0)
        {
            return;
        }

        Writer.Linef("static const Lumina::FPropertyDesc Properties[%zu];", Count);
        Writer.Line();
    }

    void FReflectedStruct::DefinePropertyTable(FCodeWriter& Writer) const
    {
        if (std::none_of(Props.begin(), Props.end(), [](const auto& Prop) { return !Prop->bInner; }))
        {
            return;
        }

        Writer.Linef("constexpr Lumina::FPropertyDesc %s::Properties[] =", TypeInfoName().c_str());
        Writer.BeginBlock();
        for (const auto& Prop : Props)
        {
            if (!Prop->bInner)
            {
                Prop->EmitDescriptor(Writer);
            }
        }
        Writer.PopIndent();
        Writer.Line("};");
        Writer.Line();
    }

    void FReflectedStruct::EmitLayoutAsserts(FCodeWriter& Writer) const
    {
        if (NativeSize > 0 && Type != EType::Class)
        {
            Writer.Linef("static_assert(sizeof(%s) == %lld, \"%s changed size since the reflector laid it out; rebuild so the C# bindings match.\");",
                EmittedCppQualifiedName().c_str(), (long long)NativeSize, DisplayName.c_str());
        }

        for (const auto& Prop : Props)
        {
            if (Prop->bInner || Prop->NativeOffset < 0)
            {
                continue;
            }
            Writer.Linef("static_assert(offsetof(%s, %s) == %lld, \"%s::%s moved since the reflector laid it out; rebuild so the C# bindings match.\");",
                Prop->Outer.c_str(), Prop->Name.c_str(), (long long)Prop->NativeOffset, DisplayName.c_str(), Prop->Name.c_str());
        }
    }

    std::string FReflectedStruct::MemberTableFields() const
    {
        std::string Fields;
        if (std::any_of(Props.begin(), Props.end(), [](const auto& Prop) { return !Prop->bInner; }))
        {
            Fields += ", .Properties = Properties, .NumProperties = (Lumina::uint32)std::size(Properties)";
        }
        if (HasNativeFunctions())
        {
            Fields += ", .Functions = Functions, .NumFunctions = (Lumina::uint32)std::size(Functions)";
        }
        return Fields;
    }

    void FReflectedStruct::DefineInitialHeader(FCodeWriter& Writer, const std::string& /*FileID*/)
    {
        // An alias name belongs to the `using`, which is declared after this header is included, so it gets no forward declaration.
        if (bIsAlias)
        {
            return;
        }

        if (!Namespace.empty())
        {
            Writer.Linef("namespace %s { struct %s; }", Namespace.c_str(), EmittedCppName().c_str());
        }
        else
        {
            Writer.Linef("struct %s;", EmittedCppName().c_str());
        }

        const std::string Api = Names::ProjectApiMacro(Header->Project->Name);
        Writer.Linef("template<> %s Lumina::CStruct* Lumina::StaticStruct<%s>();", Api.c_str(), EmittedCppQualifiedName().c_str());
    }

    void FReflectedStruct::DefineSecondaryHeader(FCodeWriter& Writer, const std::string& FileID)
    {
        // An alias has no struct body to inject GENERATED_BODY into.
        if (bIsAlias)
        {
            Writer.BlankLines(2);
            return;
        }

        Writer.Linef("#define %s_%u_GENERATED_BODY \\", FileID.c_str(), GeneratedBodyLineNumber);
        Writer.Macrof("friend struct %s;", TypeInfoName().c_str());

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

    void FReflectedStruct::DeclareImplementation(FCodeWriter& Writer)
    {
        const std::string& Cpp = EmittedCppQualifiedName();

        const std::string OffsetBase = OffsetBaseTypeName();
        if (OffsetBase != Cpp)
        {
            Writer.Linef("using %s = %s;", OffsetBase.c_str(), Cpp.c_str());
            Writer.Line();
        }

        Writer.Linef("// Begin %s", DisplayName.c_str());
        OpenNamespace(Writer);
        EmitFunctionFrames(Writer);

        Writer.Linef("struct %s", TypeInfoName().c_str());
        Writer.BeginBlock();

        Writer.Linef("static Lumina::FStructOps* MakeOps() { return Lumina::MakeStructOps<%s>(); }", Cpp.c_str());
        Writer.Line();

        EmitMetadataArrays(Writer);
        EmitPropertyTable(Writer);
        EmitFunctionTable(Writer);
        EmitLayoutAsserts(Writer);

        const std::string Super = Parent.empty() ? std::string() : (", .Super = &" + Cpp + "::Super::StaticStruct");
        Writer.Linef("static constexpr Lumina::FStructDesc Desc = { .Name = \"%s\"%s, .MakeOps = &MakeOps, "
            ".Size = sizeof(%s), .Alignment = alignof(%s)%s%s };",
            DisplayName.c_str(), Super.c_str(), Cpp.c_str(), Cpp.c_str(), MemberTableFields().c_str(),
            MetadataField(Names::FriendlyFromQualified(QualifiedName), Metadata, ESpecifierTarget::Reflect).c_str());

        Writer.PopIndent();
        Writer.Line("};");
        DefinePropertyTable(Writer);
        DefineFunctionParameterTables(Writer);
        DefineFunctionTable(Writer);
        CloseNamespace(Writer);

        // Components register their ops and events their queue type, so both are reachable by name from a script.
        std::string Registration;
        if (HasMetadata("Component"))
        {
            Registration = "Lumina::Meta::RegisterComponentMeta<" + Cpp + ">();";
        }
        else if (HasMetadata("Event"))
        {
            Registration = "Lumina::Meta::RegisterEventMeta<" + Cpp + ">();";
        }
        else if (bIsResource)
        {
            Registration = "Lumina::Meta::RegisterResourceMeta<" + Cpp + ">();";
        }
        EmitTypeFunctions(Writer, Registration);

        // An alias has no StaticStruct member to define.
        if (!bIsAlias)
        {
            Writer.Linef("Lumina::CStruct* %s::StaticStruct()", Cpp.c_str());
            Writer.Line("{");
            Writer.Linef("\treturn Lumina::Generated::%s();", GetTypeFunctionName().c_str());
            Writer.Line("}");
        }

        Writer.Linef("//~ End %s", DisplayName.c_str());
        Writer.Line();
    }
}
