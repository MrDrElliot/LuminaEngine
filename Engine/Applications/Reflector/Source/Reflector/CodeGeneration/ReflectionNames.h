#pragma once
#include "Reflector/Utils/StringOps.h"
#include <string>
#include <string_view>

#include "Reflector/Clang/Utils.h"

namespace Lumina::Reflection
{
    // Every generated symbol name, so the mangling rules live in one place.
    namespace Names
    {
        // "Lumina::CObject" -> "Lumina_CObject".
        inline std::string FriendlyFromQualified(std::string_view QualifiedName)
        {
            return ClangUtils::MakeCodeFriendlyNamespace(std::string(QualifiedName));
        }

        // "Lumina" + "FName" gives "Lumina_FName", and a nested namespace has its "::" flattened the same way.
        inline std::string FriendlyFromParts(std::string_view Namespace, std::string_view DisplayName)
        {
            std::string Out = ClangUtils::MakeCodeFriendlyNamespace(std::string(Namespace));
            Out.push_back('_');
            Out.append(DisplayName.data(), DisplayName.data() + DisplayName.size());
            return Out;
        }

        // "Runtime" -> "RUNTIME_API".
        inline std::string ProjectApiMacro(std::string_view ProjectName)
        {
            std::string Out;
            Out.append(ProjectName.data(), ProjectName.data() + ProjectName.size());
            Out += "_api";
            Lumina::StringOps::ToUpper(Out);
            return Out;
        }

        // The package a module's reflected types live in, "/Types/Engine" for Runtime and "/Types/<Project>" otherwise.
        inline std::string TypePackage(std::string_view ProjectName)
        {
            std::string Lower(ProjectName.data(), ProjectName.data() + ProjectName.size());
            Lumina::StringOps::ToLower(Lower);

            std::string Out = "/Types/";
            if (Lower == "runtime")
            {
                Out += "Engine";
            }
            else
            {
                Out.append(ProjectName.data(), ProjectName.data() + ProjectName.size());
            }
            return Out;
        }

        // A normalized identifier for a header file, suitable for use as a macro guard
        // or symbol prefix. Replaces separators with underscores.
        inline void SanitizeFileID(std::string& FileID)
        {
            for (auto& Ch : FileID)
            {
                if (Ch == '/' || Ch == '\\' || Ch == '.' || Ch == '-')
                {
                    Ch = '_';
                }
            }
        }

        // Given a full header path, derive the FileID used by generated macros.
        inline std::string MakeFileIDForHeaderPath(std::string HeaderPath)
        {
            const size_t SlashPos = HeaderPath.find_first_of("/\\");
            if (SlashPos != std::string::npos)
            {
                HeaderPath = HeaderPath.substr(SlashPos + 1);
            }

            SanitizeFileID(HeaderPath);
            return HeaderPath;
        }

        // The frame a reflected call is made through: "<Type>_<Func>_Parms". offsetof is taken against it,
        // so the compiler is what lays the frame out rather than the generator.
        inline std::string FunctionParmsStruct(std::string_view TypeName, std::string_view FunctionName)
        {
            std::string Out(TypeName.data(), TypeName.data() + TypeName.size());
            Out += "_";
            Out.append(FunctionName.data(), FunctionName.size());
            Out += "_Parms";
            return Out;
        }

        inline std::string FunctionThunk(std::string_view TypeName, std::string_view FunctionName)
        {
            std::string Out("Thunk_");
            Out.append(TypeName.data(), TypeName.size());
            Out += "_";
            Out.append(FunctionName.data(), FunctionName.size());
            return Out;
        }

        // The descriptor table of a reflected function's parameters, "Func_<Name>_Params".
        inline std::string FunctionParamsTable(std::string_view FunctionName)
        {
            std::string Out("Func_");
            Out.append(FunctionName.data(), FunctionName.size());
            Out += "_Params";
            return Out;
        }

        // A template argument list carries commas, which the offsetof macro would read as extra arguments.
        inline std::string OffsetBaseTypeName(const std::string& CppQualifiedName, std::string_view Namespace, std::string_view DisplayName)
        {
            return CppQualifiedName.find('<') == std::string::npos ? CppQualifiedName : ("LRT_Owner_" + FriendlyFromParts(Namespace, DisplayName));
        }
    }
}
