#pragma once
#include <EASTL/string.h>
#include <EASTL/string_view.h>

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
        inline eastl::string FriendlyFromQualified(eastl::string_view QualifiedName)
        {
            return ClangUtils::MakeCodeFriendlyNamespace(eastl::string(QualifiedName));
        }

        // "Lumina" + "FName" -> "Lumina_FName"; no namespace -> "_FName".
        //
        // The separator is emitted even with nothing before it, because the engine's IMPLEMENT_CLASS
        // builds the same symbol through CONCAT4(Registration_Info_CClass_, TNamespace, _, TClass).
        // An empty namespace argument still contributes its separator there, so dropping ours made
        // the two disagree by one underscore and a class outside any namespace failed to link.
        inline eastl::string FriendlyFromParts(eastl::string_view Namespace, eastl::string_view DisplayName)
        {
            // Sanitized, because a nested namespace arrives as "MyStudio::Deep" and this result is
            // spliced into identifiers. Left raw, the "::" made the compiler read the symbol as a
            // scope resolution and the surrounding macro name as ill-formed.
            eastl::string Out = ClangUtils::MakeCodeFriendlyNamespace(eastl::string(Namespace));
            Out.push_back('_');
            Out.append(DisplayName.data(), DisplayName.data() + DisplayName.size());
            return Out;
        }

        // "CStruct" / "Lumina" / "FAABB" -> "Construct_CStruct_Lumina_FAABB".
        inline eastl::string ConstructFunction(eastl::string_view TypeKind, eastl::string_view Namespace, eastl::string_view DisplayName)
        {
            eastl::string Out = "Construct_";
            Out.append(TypeKind.data(), TypeKind.data() + TypeKind.size());
            Out.push_back('_');
            Out += FriendlyFromParts(Namespace, DisplayName);
            return Out;
        }

        // The Statics struct holding every compile-time metadata / property param array.
        inline eastl::string StaticsStruct(eastl::string_view TypeKind, eastl::string_view Namespace, eastl::string_view DisplayName)
        {
            return ConstructFunction(TypeKind, Namespace, DisplayName) + "_Statics";
        }

        // The translation-unit-local singleton holder.
        inline eastl::string RegistrationInfo(eastl::string_view TypeKind, eastl::string_view Namespace, eastl::string_view DisplayName)
        {
            eastl::string Out = "Registration_Info_";
            Out.append(TypeKind.data(), TypeKind.data() + TypeKind.size());
            Out.push_back('_');
            Out += FriendlyFromParts(Namespace, DisplayName);
            return Out;
        }

        // Emit a cross-module Construct_C* forward decl, guarded against re-declaration in the same TU.
        // The unity build concatenates many standalone .generated.cpp files; the unique fn name is the guard token.
        inline void EmitGuardedCrossModuleDecl(
            FCodeWriter& Writer,
            eastl::string_view API,
            eastl::string_view Kind,        // "CStruct", "CClass", "CEnum"
            eastl::string_view FnName)      // "Construct_CStruct_Lumina_FVector3"
        {
            const eastl::string FnNameStr(FnName.data(), FnName.size());
            Writer.Linef("#ifndef LRT_XREF_%s", FnNameStr.c_str());
            Writer.Linef("#define LRT_XREF_%s", FnNameStr.c_str());
            Writer.Linef("%.*s Lumina::%.*s* %s();",
                static_cast<int>(API.size()), API.data(),
                static_cast<int>(Kind.size()), Kind.data(),
                FnNameStr.c_str());
            Writer.Line("#endif");
        }

        // "Runtime" -> "RUNTIME_API".
        inline eastl::string ProjectApiMacro(eastl::string_view ProjectName)
        {
            eastl::string Out;
            Out.append(ProjectName.data(), ProjectName.data() + ProjectName.size());
            Out += "_api";
            Out.make_upper();
            return Out;
        }

        // "Runtime" -> "Engine" (special-cased), otherwise the project name.
        inline eastl::string ScriptPackage(eastl::string_view ProjectName)
        {
            eastl::string Lower(ProjectName.data(), ProjectName.data() + ProjectName.size());
            Lower.make_lower();

            eastl::string Out = "/Script/";
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
        inline void SanitizeFileID(eastl::string& FileID)
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
        inline eastl::string MakeFileIDForHeaderPath(eastl::string HeaderPath)
        {
            const size_t SlashPos = HeaderPath.find_first_of("/\\");
            if (SlashPos != eastl::string::npos)
            {
                HeaderPath = HeaderPath.substr(SlashPos + 1);
            }

            SanitizeFileID(HeaderPath);
            return HeaderPath;
        }

        // The metadata array symbol for a type or property: "<Friendly>_Metadata".
        inline eastl::string MetadataArrayForType(eastl::string_view QualifiedName)
        {
            return FriendlyFromQualified(QualifiedName) + "_Metadata";
        }

        // Per-property metadata array inside a Statics struct: "<PropName>_Metadata".
        inline eastl::string MetadataArrayForProperty(eastl::string_view PropertyName)
        {
            eastl::string Out(PropertyName.data(), PropertyName.data() + PropertyName.size());
            Out += "_Metadata";
            return Out;
        }
    }
}
