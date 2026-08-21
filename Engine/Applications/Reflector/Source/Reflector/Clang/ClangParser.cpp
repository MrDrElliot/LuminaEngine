#include "ClangParser.h"
#include <filesystem>
#include <fstream>
#include <clang-c/Index.h>
#include <vector>
#include <unordered_set>
#include "Reflector/Clang/Utils.h"
#include "Reflector/Diagnostics/LRTDiagnostics.h"
#include "Reflector/ProjectSolution.h"
#include "Reflector/ReflectionCore/ReflectedProject.h"
#include "Visitors/ClangTranslationUnit.h"



namespace Lumina::Reflection
{
    namespace
    {
        FDiagLocation MakeLocationFromDiagnostic(CXDiagnostic Diagnostic)
        {
            FDiagLocation Result;

            CXFile   File   = nullptr;
            uint32_t Line   = 0;
            uint32_t Column = 0;
            clang_getExpansionLocation(clang_getDiagnosticLocation(Diagnostic), &File, &Line, &Column, nullptr);

            if (File != nullptr)
            {
                CXString Name = clang_getFileName(File);
                if (const char* Raw = clang_getCString(Name))
                {
                    Result.File = Raw;
                    std::replace(Result.File.begin(), Result.File.end(), '\\', '/');
                }
                clang_disposeString(Name);
            }

            Result.Line   = Line;
            Result.Column = Column;
            return Result;
        }

        // The parse runs before this tool writes them, so on a cold target they are legitimately absent.
        bool IsMissingGeneratedHeader(const std::string& Text)
        {
            return Text.find(".generated.h") != std::string::npos
                && Text.find("file not found") != std::string::npos;
        }

        // Toolchain headers parse loosely on purpose, so only a header we emit reflection for can corrupt output.
        bool IsReflectedHeader(const FClangParserContext& Context, const FDiagLocation& Loc)
        {
            return !Loc.File.empty()
                && Context.AllHeaders.find(FStringHash(ClangUtils::NormalizeHeaderPath(Loc.File))) != Context.AllHeaders.end();
        }

        // CXError_Success only means an AST came back, so a bad header still reflects as garbage without this.
        bool ReportClangDiagnostics(CXTranslationUnit TranslationUnit, const FClangParserContext& Context, bool bStrict)
        {
            constexpr uint32_t MaxReported = 25;

            const uint32_t NumDiagnostics = clang_getNumDiagnostics(TranslationUnit);
            uint32_t NumSevere = 0;

            for (uint32_t i = 0; i < NumDiagnostics; ++i)
            {
                CXDiagnostic Diagnostic = clang_getDiagnostic(TranslationUnit, i);
                const CXDiagnosticSeverity Severity = clang_getDiagnosticSeverity(Diagnostic);

                const FDiagLocation Loc = MakeLocationFromDiagnostic(Diagnostic);
                const bool bSevere = Severity == CXDiagnostic_Error || Severity == CXDiagnostic_Fatal;

                if (bSevere && IsReflectedHeader(Context, Loc))
                {
                    CXString Spelling = clang_getDiagnosticSpelling(Diagnostic);
                    const char* Raw = clang_getCString(Spelling);
                    const std::string Text = Raw != nullptr ? Raw : "";
                    clang_disposeString(Spelling);

                    if (IsMissingGeneratedHeader(Text))
                    {
                        clang_disposeDiagnostic(Diagnostic);
                        continue;
                    }

                    ++NumSevere;
                    if (NumSevere <= MaxReported)
                    {
                        const char* Message = Text.empty() ? "clang reported an error with no message" : Text.c_str();
                        const char* Advice = bStrict
                            ? "Reflection generated from this header would be wrong, so the parse is rejected."
                            : "Reflection generated from this header may be wrong. Run with -strict-parse to make this fatal.";

                        if (bStrict)
                        {
                            FDiagnostics::Get().Errorf(Loc, EDiagId::DriverClangDiagnostic, "%s. %s",
                                Message, Advice);
                        }
                        else
                        {
                            FDiagnostics::Get().Warningf(Loc, EDiagId::DriverClangDiagnostic, "%s. %s",
                                Message, Advice);
                        }
                    }
                }

                clang_disposeDiagnostic(Diagnostic);
            }

            if (NumSevere > MaxReported)
            {
                FDiagLocation Loc;
                FDiagnostics::Get().Warningf(Loc, EDiagId::DriverClangDiagnostic,
                    "%u further clang error(s) in reflected headers not listed.", NumSevere - MaxReported);
            }

            return NumSevere == 0 || !bStrict;
        }
    }

    bool FClangParser::Parse(FReflectedWorkspace* Workspace)
    {
        CXTranslationUnit TranslationUnit = nullptr;
        CXIndex ClangIndex = nullptr;
    
        ParsingContext.Workspace = Workspace;
        
        const std::string AmalgamationPath = std::filesystem::absolute("ReflectHeaders.gen.h").string().c_str();

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
        
        // The pointer array is built only once every argument is in place, or c_str() dangles on realloc.
        std::vector<std::string> ClangArgStorage;
        std::vector<const char*>   ClangArgs;

        auto AppendArg = [&](std::string Arg)
        {
            ClangArgStorage.emplace_back(std::move(Arg));
        };

        // Only link-visibility macros differ per module, and those are blanked after this loop.
        std::unordered_set<std::string> SeenDefinitions;
        std::unordered_set<std::string> SeenIncludeDirs;
        std::unordered_set<std::string> SeenForceIncludes;

        for (const auto& Project : Workspace->ReflectedProjects)
        {
            for (const std::string& Definition : Project->Definitions)
            {
                if (SeenDefinitions.insert(Definition).second)
                {
                    AppendArg("-D" + Definition);
                }
            }

            for (const std::string& ForceInclude : Project->ForceIncludes)
            {
                if (SeenForceIncludes.insert(ForceInclude).second)
                {
                    AppendArg("-include");
                    AppendArg(ForceInclude);
                }
            }

            for (const std::string& IncludeDir : Project->IncludeDirs)
            {
                if (!SeenIncludeDirs.insert(IncludeDir).second)
                {
                    continue;
                }
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

        // Last -D wins, so this neutralizes the dllimport/dllexport the build system just supplied.
        for (const std::string& Definition : SeenDefinitions)
        {
            const size_t Equals = Definition.find('=');
            const std::string Macro = Equals == std::string::npos ? Definition : Definition.substr(0, Equals);

            if (Macro.size() > 4 && Macro.compare(Macro.size() - 4, 4, "_API") == 0)
            {
                AppendArg("-D" + Macro + "=");
            }
        }

        AppendArg("-x");
        AppendArg("c++");
        AppendArg("-std=c++23");
        AppendArg("-O0");
        AppendArg("-DREFLECTION_PARSER");
        // libclang trails the MSVC STL's supported-compiler floor, and its version assert poisons <type_traits>.
        AppendArg("-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH");
        // MSVC's offsetof is a reinterpret_cast that is never constexpr; this selects __builtin_offsetof.
        AppendArg("-D_CRT_USE_BUILTIN_OFFSETOF");
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

        ClangArgs.reserve(ClangArgStorage.size());
        for (const std::string& Arg : ClangArgStorage)
        {
            ClangArgs.emplace_back(Arg.c_str());
        }

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
        
        // Walking a broken AST would emit confident, wrong reflection data, so stop before it.
        if (!ReportClangDiagnostics(TranslationUnit, ParsingContext, bStrictParse))
        {
            std::filesystem::remove(AmalgamationPath.c_str());
            clang_disposeIndex(ClangIndex);
            return false;
        }

        CXCursor Cursor = clang_getTranslationUnitCursor(TranslationUnit);

        // A non-zero return abandons every later cursor, so reaching this is a defect in the walk itself.
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
