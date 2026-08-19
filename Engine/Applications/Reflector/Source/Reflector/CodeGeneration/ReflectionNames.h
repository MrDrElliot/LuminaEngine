#pragma once
#include "Reflector/Utils/StringOps.h"
#include <string>
#include <string_view>

#include "Reflector/Clang/Utils.h"
#include "Reflector/CodeGeneration/CodeWriter.h"

namespace Lumina::Reflection
{
    class FReflectedType;
    class FReflectedHeader;
    class FReflectedProject;

    // Central source of truth for every generated symbol name; all "Construct_CStruct_Lumina_FName"-style
    // mangling routes through these helpers so the rules live in one place.
    namespace Names
    {
        // "Lumina::CObject" -> "Lumina_CObject".
        inline std::string FriendlyFromQualified(std::string_view QualifiedName)
        {
            return ClangUtils::MakeCodeFriendlyNamespace(std::string(QualifiedName));
        }

        // "Lumina" + "FName" -> "Lumina_FName"; no namespace -> "_FName".
        //
        // The separator is emitted even with nothing before it, because the engine's IMPLEMENT_CLASS
        // builds the same symbol through CONCAT4(Registration_Info_CClass_, TNamespace, _, TClass).
        // An empty namespace argument still contributes its separator there, so dropping ours made
        // the two disagree by one underscore and a class outside any namespace failed to link.
        inline std::string FriendlyFromParts(std::string_view Namespace, std::string_view DisplayName)
        {
            // Sanitized, because a nested namespace arrives as "MyStudio::Deep" and this result is
            // spliced into identifiers. Left raw, the "::" made the compiler read the symbol as a
            // scope resolution and the surrounding macro name as ill-formed.
            std::string Out = ClangUtils::MakeCodeFriendlyNamespace(std::string(Namespace));
            Out.push_back('_');
            Out.append(DisplayName.data(), DisplayName.data() + DisplayName.size());
            return Out;
        }

        // "CStruct" / "Lumina" / "FAABB" -> "Construct_CStruct_Lumina_FAABB".
        inline std::string ConstructFunction(std::string_view TypeKind, std::string_view Namespace, std::string_view DisplayName)
        {
            std::string Out = "Construct_";
            Out.append(TypeKind.data(), TypeKind.data() + TypeKind.size());
            Out.push_back('_');
            Out += FriendlyFromParts(Namespace, DisplayName);
            return Out;
        }

        // The Statics struct holding every compile-time metadata / property param array.
        inline std::string StaticsStruct(std::string_view TypeKind, std::string_view Namespace, std::string_view DisplayName)
        {
            return ConstructFunction(TypeKind, Namespace, DisplayName) + "_Statics";
        }

        // The translation-unit-local singleton holder.
        inline std::string RegistrationInfo(std::string_view TypeKind, std::string_view Namespace, std::string_view DisplayName)
        {
            std::string Out = "Registration_Info_";
            Out.append(TypeKind.data(), TypeKind.data() + TypeKind.size());
            Out.push_back('_');
            Out += FriendlyFromParts(Namespace, DisplayName);
            return Out;
        }

        // Emit a cross-module Construct_C* forward decl, guarded against re-declaration in the same TU.
        // The unity build concatenates many standalone .generated.cpp files; the unique fn name is the guard token.
        inline void EmitGuardedCrossModuleDecl(
            FCodeWriter& Writer,
            std::string_view API,
            std::string_view Kind,        // "CStruct", "CClass", "CEnum"
            std::string_view FnName)      // "Construct_CStruct_Lumina_FVector3"
        {
            const std::string FnNameStr(FnName.data(), FnName.size());
            Writer.Linef("#ifndef LRT_XREF_%s", FnNameStr.c_str());
            Writer.Linef("#define LRT_XREF_%s", FnNameStr.c_str());
            Writer.Linef("%.*s Lumina::%.*s* %s();",
                static_cast<int>(API.size()), API.data(),
                static_cast<int>(Kind.size()), Kind.data(),
                FnNameStr.c_str());
            Writer.Line("#endif");
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

        // "Runtime" -> "Engine" (special-cased), otherwise the project name.
        inline std::string ScriptPackage(std::string_view ProjectName)
        {
            std::string Lower(ProjectName.data(), ProjectName.data() + ProjectName.size());
            Lumina::StringOps::ToLower(Lower);

            std::string Out = "/Script/";
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

        // The metadata array symbol for a type or property: "<Friendly>_Metadata".
        inline std::string MetadataArrayForType(std::string_view QualifiedName)
        {
            return FriendlyFromQualified(QualifiedName) + "_Metadata";
        }

        // Per-property metadata array inside a Statics struct: "<PropName>_Metadata".
        inline std::string MetadataArrayForProperty(std::string_view PropertyName)
        {
            std::string Out(PropertyName.data(), PropertyName.data() + PropertyName.size());
            Out += "_Metadata";
            return Out;
        }
    }
}
