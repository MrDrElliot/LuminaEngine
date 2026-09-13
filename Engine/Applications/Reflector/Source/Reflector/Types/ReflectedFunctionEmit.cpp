#include "ReflectedType.h"

#include "Reflector/CodeGeneration/CodeWriter.h"
#include "Reflector/CodeGeneration/ReflectionNames.h"

namespace Lumina::Reflection
{
    namespace
    {
        // Indexed rather than named after the parameter, so a parameter called Params cannot collide with
        // the table beside it, and the reflected name stays whatever the signature spelled.
        std::string ParamSymbol(const FReflectedFunction& Function, size_t Index)
        {
            return Names::FunctionParamsStatic(Function.Name) + "_P" + std::to_string(Index);
        }

        // The real class name, since a call has to name the type rather than whatever alias reflects it.
        std::string QualifiedOwner(const FReflectedStruct& Owner)
        {
            return Owner.EmittedCppQualifiedName();
        }

        // The frames and thunks are emitted inside the type's namespace, so anything outside names them through it.
        std::string NamespacePrefix(const FReflectedStruct& Owner)
        {
            return Owner.Namespace.empty() ? std::string() : (Owner.Namespace + "::");
        }
    }

    bool FReflectedStruct::HasNativeFunctions() const
    {
        for (const auto& Function : Functions)
        {
            if (Function->bReflectNatively)
            {
                return true;
            }
        }
        return false;
    }

    void FReflectedStruct::EmitFunctionFrames(FCodeWriter& Writer) const
    {
        if (!HasNativeFunctions())
        {
            return;
        }

        // Emitted in the type's own namespace, because the member types are spelled the way the header
        // spelled them: a parameter written as FName is Lumina::FName only from inside Lumina.
        const bool bWrapInNamespace = !Namespace.empty();
        if (bWrapInNamespace)
        {
            Writer.Linef("namespace %s", Namespace.c_str());
            Writer.Line("{");
        }

        for (const auto& Function : Functions)
        {
            if (!Function->bReflectNatively)
            {
                continue;
            }

            const std::string Parms = Names::FunctionParmsStruct(DisplayName, Function->Name);
            const std::string Thunk = Names::FunctionThunk(DisplayName, Function->Name);

            Writer.Linef("// The frame %s is called through. The compiler lays it out, so the offsets below are the real ones.", Function->Name.c_str());
            Writer.Linef("struct %s", Parms.c_str());
            Writer.Line("{");
            for (size_t Index = 0; Index < Function->TopLevelParams.size(); ++Index)
            {
                Writer.Linef("\t%s %s;", Function->ParameterStorageTypes[Index].c_str(),
                    Function->TopLevelParams[Index]->Name.c_str());
            }
            Writer.Line("};");
            Writer.Line();

            // Beside the frame, since a container parameter's ops forwarder is referenced by its property
            // definition and there is no owning type to hang it off.
            for (auto& Entry : Function->ParamEntries)
            {
                if (Entry->HasAccessors())
                {
                    Entry->DefineAccessors(Writer, const_cast<FReflectedStruct*>(this));
                }
            }

            // The call itself: a static takes no object, and anything else dispatches through the pointer,
            // which is what makes a virtual reach its override.
            std::string Arguments;
            const size_t NumArguments = Function->ReturnIndex >= 0
                ? Function->TopLevelParams.size() - 1
                : Function->TopLevelParams.size();

            for (size_t Index = 0; Index < NumArguments; ++Index)
            {
                if (Index != 0)
                {
                    Arguments += ", ";
                }

                // An object argument is held as a handle, so the call takes the pointer back out of it.
                // The cast is unqualified because a generated file need not have the class's definition,
                // and every reflected class shares one root, so the pointer value is the same either way.
                const std::string& CastType = Function->ParameterObjectCastTypes[Index];
                if (!CastType.empty())
                {
                    Arguments += "(" + CastType + ")P." + Function->TopLevelParams[Index]->Name + ".Get()";
                }
                else if (Function->ParameterIsRvalue[Index])
                {
                    Arguments += "std::move(P." + Function->TopLevelParams[Index]->Name + ")";
                }
                else
                {
                    Arguments += "P.";
                    Arguments += Function->TopLevelParams[Index]->Name;
                }
            }

            const std::string Owner = QualifiedOwner(*this);
            const std::string Call = Function->bIsStatic
                ? (Owner + "::" + Function->Name + "(" + Arguments + ")")
                : ("((" + Owner + "*)Context)->" + Function->Name + "(" + Arguments + ")");

            Writer.Linef("static void %s(void* Context, void* Frame)", Thunk.c_str());
            Writer.Line("{");
            if (!Function->TopLevelParams.empty())
            {
                Writer.Linef("\t%s& P = *(%s*)Frame;", Parms.c_str(), Parms.c_str());
            }
            else
            {
                Writer.Line("\t(void)Frame;");
            }
            if (Function->bIsStatic)
            {
                Writer.Line("\t(void)Context;");
            }
            if (Function->ReturnIndex >= 0)
            {
                const size_t ReturnAt = (size_t)Function->ReturnIndex;

                // An object return goes into a handle to CObject, and the derived-to-base conversion is not
                // available where the class is only forward declared, so it is spelled out.
                const std::string Assigned = Function->ParameterObjectCastTypes[ReturnAt].empty()
                    ? Call
                    : ("(Lumina::CObject*)(" + Call + ")");

                Writer.Linef("\tP.%s = %s;",
                    Function->TopLevelParams[ReturnAt]->Name.c_str(), Assigned.c_str());
            }
            else
            {
                Writer.Linef("\t%s;", Call.c_str());
            }
            Writer.Line("}");
            Writer.Line();
        }

        if (bWrapInNamespace)
        {
            Writer.Line("}");
            Writer.Line();
        }
    }

    void FReflectedStruct::EmitFunctionFieldDeclarations(FCodeWriter& Writer) const
    {
        for (const auto& Function : Functions)
        {
            if (!Function->bReflectNatively)
            {
                continue;
            }

            const std::string Statics = Names::FunctionParamsStatic(Function->Name);

            for (size_t Index = 0; Index < Function->ParamEntries.size(); ++Index)
            {
                Writer.Linef("static const Lumina::%s %s;",
                    Function->ParamEntries[Index]->GetPropertyParamType(), ParamSymbol(*Function, Index).c_str());
            }

            if (!Function->ParamEntries.empty())
            {
                Writer.Linef("static const Lumina::FPropertyParams* const %s_Params[];", Statics.c_str());
            }
            Writer.Linef("static const Lumina::FFunctionParams %s;", Statics.c_str());
        }

        if (HasNativeFunctions())
        {
            Writer.Line("static const Lumina::FFunctionParams* const FuncPointers[];");
        }
    }

    void FReflectedStruct::EmitFunctionDefinitions(FCodeWriter& Writer, std::string_view StaticsName) const
    {
        const std::string Statics(StaticsName);

        for (const auto& Function : Functions)
        {
            if (!Function->bReflectNatively)
            {
                continue;
            }

            const std::string FuncStatic = Names::FunctionParamsStatic(Function->Name);
            const std::string Prefix = NamespacePrefix(*this);
            const std::string Parms = Prefix + Names::FunctionParmsStruct(DisplayName, Function->Name);
            const std::string Thunk = Prefix + Names::FunctionThunk(DisplayName, Function->Name);

            for (size_t Index = 0; Index < Function->ParamEntries.size(); ++Index)
            {
                Writer.Appendf("const Lumina::%s %s::%s = ",
                    Function->ParamEntries[Index]->GetPropertyParamType(), Statics.c_str(),
                    ParamSymbol(*Function, Index).c_str());
                Function->ParamEntries[Index]->AppendDefinition(Writer);
            }

            if (!Function->ParamEntries.empty())
            {
                Writer.Linef("const Lumina::FPropertyParams* const %s::%s_Params[] = {", Statics.c_str(), FuncStatic.c_str());
                for (size_t Index = 0; Index < Function->ParamEntries.size(); ++Index)
                {
                    Writer.Linef("\t(const Lumina::FPropertyParams*)&%s::%s,", Statics.c_str(), ParamSymbol(*Function, Index).c_str());
                }
                Writer.Line("};");
            }

            std::string Flags = "Lumina::EFunctionFlags::None";
            if (Function->bIsStatic)  { Flags += " | Lumina::EFunctionFlags::Static"; }
            if (Function->bIsConst)   { Flags += " | Lumina::EFunctionFlags::Const"; }
            if (Function->bIsVirtual) { Flags += " | Lumina::EFunctionFlags::Virtual"; }

            const std::string ParamsArg = Function->ParamEntries.empty()
                ? std::string("nullptr")
                : (FuncStatic + "_Params");
            const std::string CountArg = Function->ParamEntries.empty()
                ? std::string("0")
                : ("(uint16)std::size(" + FuncStatic + "_Params)");

            Writer.Linef("const Lumina::FFunctionParams %s::%s = { \"%s\", %s, %s, %s, %d, (uint16)sizeof(%s), &%s };",
                Statics.c_str(), FuncStatic.c_str(), Function->Name.c_str(), Flags.c_str(),
                ParamsArg.c_str(), CountArg.c_str(), Function->ReturnIndex,
                Parms.c_str(), Thunk.c_str());
            Writer.Line();
        }
    }

    void FReflectedStruct::EmitFunctionPointerTable(FCodeWriter& Writer, std::string_view StaticsName) const
    {
        if (!HasNativeFunctions())
        {
            return;
        }

        const std::string Statics(StaticsName);

        Writer.Linef("const Lumina::FFunctionParams* const %s::FuncPointers[] = {", Statics.c_str());
        for (const auto& Function : Functions)
        {
            if (!Function->bReflectNatively)
            {
                continue;
            }
            Writer.Linef("\t&%s::%s,", Statics.c_str(), Names::FunctionParamsStatic(Function->Name).c_str());
        }
        Writer.Line("};");
        Writer.Line();
    }
}
