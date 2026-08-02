#include "ClangParser.h"
#include <filesystem>
#include <fstream>
#include <clang-c/Index.h>
#include "EASTL/fixed_vector.h"
#include "Reflector/Diagnostics/LRTDiagnostics.h"
#include "Reflector/ProjectSolution.h"
#include "Reflector/ReflectionCore/ReflectedProject.h"
#include "Visitors/ClangTranslationUnit.h"



namespace Lumina::Reflection
{
    bool FClangParser::Parse(FReflectedWorkspace* Workspace)
    {
        CXTranslationUnit TranslationUnit = nullptr;
        CXIndex ClangIndex = nullptr;
    
        ParsingContext.Workspace = Workspace;
        
        const eastl::string AmalgamationPath = std::filesystem::absolute("ReflectHeaders.gen.h").string().c_str();

        std::ofstream AmalgamationFile(AmalgamationPath.c_str());
        if (!AmalgamationFile.is_open())
        {
            FDiagLocation Loc;
            Loc.File = AmalgamationPath;
            FDiagnostics::Get().Errorf(Loc, EDiagId::DriverAmalgamationCreate,
                "Failed to create amalgamation file '%s'. Check write permissions on the working directory.",
                AmalgamationPath.c_str());
            return false;
        }
        AmalgamationFile << "#pragma once\n\n";
        
        // Needed to keep dynamic args alive.
        eastl::fixed_vector<eastl::string, 256, false>  ClangArgStorage;
        eastl::fixed_vector<const char*, 256, false>    ClangArgs;
        
        auto AppendArg = [&](eastl::string Arg)
        {
            ClangArgStorage.emplace_back(eastl::move(Arg));
            ClangArgs.emplace_back(ClangArgStorage.back().c_str());
        };
        
        for (const auto& Project : Workspace->ReflectedProjects)
        {
            eastl::string APIDecl = "-D" + Project->Name + "_API=";
            APIDecl.make_upper();

            AppendArg(eastl::move(APIDecl));

            for (const eastl::string& IncludeDir : Project->IncludeDirs)
            {
                AppendArg("-I" + IncludeDir);
            }

            for (auto& [Path, Header] : Project->Headers)
            {
                AmalgamationFile << "#include \"" << Path.c_str() << "\"\n";
                ParsingContext.AllHeaders.emplace(Path, Header.get());
                ParsingContext.NumHeadersReflected++;
            }
        }

        AmalgamationFile.close();

        AppendArg("-x");
        AppendArg("c++");
        AppendArg("-std=c++23");
        AppendArg("-O0");
        AppendArg("-DREFLECTION_PARSER");
        AppendArg("-DNDEBUG");
        AppendArg("-fms-extensions");
        AppendArg("-fms-compatibility");
        AppendArg("-Wfatal-errors=0");
        AppendArg("-ferror-limit=1000000000");
        AppendArg("-Wno-multichar");
        AppendArg("-Wno-deprecated-builtins");
        AppendArg("-Wno-unknown-warning-option");
        AppendArg("-Wno-return-type-c-linkage");
        AppendArg("-Wno-c++98-compat-pedantic");
        AppendArg("-Wno-gnu-folding-constant");
        AppendArg("-Wno-vla-extension-static-assert");
        AppendArg("-fno-spell-checking");
        AppendArg("-fno-delayed-template-parsing");
    
        ClangIndex = clang_createIndex(0, 0);
        
        constexpr uint32_t ClangOptions = 
            CXTranslationUnit_DetailedPreprocessingRecord |
            CXTranslationUnit_SkipFunctionBodies | 
            CXTranslationUnit_CacheCompletionResults |
            CXTranslationUnit_IncludeBriefCommentsInCodeCompletion |
            CXTranslationUnit_KeepGoing;
        
        CXErrorCode Result = clang_parseTranslationUnit2(
            ClangIndex,
            AmalgamationPath.c_str(),
            ClangArgs.data(),
            (int)ClangArgs.size(),
            nullptr,
            0,
            ClangOptions,
            &TranslationUnit);
        
        CXCursor Cursor = clang_getTranslationUnitCursor(TranslationUnit);

        // A non-zero return means a visitor asked to stop, which abandons every cursor after it
        // and silently drops the reflection data for the rest of the workspace. Visitors report
        // their own problems and keep going, so reaching this is a defect in the walk itself
        // rather than something the user can act on in their code.
        if (clang_visitChildren(Cursor, VisitTranslationUnit, &ParsingContext) != 0)
        {
            FDiagLocation Loc;
            Loc.File = AmalgamationPath;
            FDiagnostics::Get().Errorf(Loc, EDiagId::DriverTranslationUnitWalk,
                "The AST walk was aborted before it finished, so reflection data is incomplete. "
                "This is an internal Reflector fault, not an error in the parsed headers.");
        }

        if (Result != CXError_Success)
        {
            FDiagLocation Loc;
            Loc.File = AmalgamationPath;
            const char* Reason = "unknown";
            switch (Result)
            {
            case CXError_Failure:          Reason = "unknown failure";           break;
            case CXError_Crashed:          Reason = "libclang crashed";          break;
            case CXError_InvalidArguments: Reason = "invalid arguments";         break;
            case CXError_ASTReadError:     Reason = "AST read error";            break;
            default: break;
            }
            FDiagnostics::Get().Errorf(Loc, EDiagId::DriverClangParseFailure,
                "libclang parse failed: %s (CXErrorCode=%d).",
                Reason, static_cast<int>(Result));
        }
        
        std::filesystem::remove(AmalgamationPath.c_str());
        clang_disposeIndex(ClangIndex);
        return Result == CXError_Success;
    }
}
