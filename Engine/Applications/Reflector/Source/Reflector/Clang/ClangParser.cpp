#include "ClangParser.h"
#include <filesystem>
#include <fstream>
#include <clang-c/Index.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <ranges>
#include <string_view>
#include <unordered_set>
#include "Reflector/Clang/Utils.h"
#include "Reflector/Diagnostics/LRTDiagnostics.h"
#include "Reflector/ProjectSolution.h"
#include "Reflector/ReflectionCore/ReflectedProject.h"
#include "Reflector/Utils/FileIO.h"
#include "Reflector/Utils/Timing.h"
#include "Visitors/ClangTranslationUnit.h"

#if !defined(_WIN32)
    #include <dlfcn.h>
#endif



namespace Lumina::Reflection
{
    extern uint64_t GCursorsVisited;
    extern uint64_t GCursorsInReflectedHeaders;

    namespace
    {
        // libclang resolves its builtin headers relative to the host executable, which for the Reflector
        // is Binaries/, never the bundle. Windows does not need them: clang takes stddef.h, float.h and
        // the intrinsics from the MSVC headers it auto-detects, which is why only Linux breaks without this.
        std::string FindClangResourceDir()
        {
#if defined(_WIN32)
            return {};
#else
            // &clang_createIndex would resolve to this executable's PLT stub, naming Binaries again.
            void* Symbol = dlsym(RTLD_DEFAULT, "clang_createIndex");

            Dl_info Info;
            if (Symbol == nullptr || dladdr(Symbol, &Info) == 0 || Info.dli_fname == nullptr)
            {
                return {};
            }

            std::error_code Error;
            const std::filesystem::path ClangDir = std::filesystem::path(Info.dli_fname).parent_path() / "clang";

            // Named by version, and which one is a property of the bundle rather than of this tool.
            for (const auto& Entry : std::filesystem::directory_iterator(ClangDir, Error))
            {
                if (std::filesystem::exists(Entry.path() / "include" / "stddef.h"))
                {
                    return Entry.path().string();
                }
            }

            return {};
#endif
        }

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

        // Naming a reflection-free header as an amalgamation root drags its include closure in for nothing.
        bool MayCarryReflection(const std::string& HeaderPath)
        {
            std::string Contents;
            if (!ReadWholeFile(HeaderPath, Contents))
            {
                // Unreadable here means clang should be the one to complain about it.
                return true;
            }

            // Substrings rather than whole words, and a reflected header must also name its companion.
            constexpr std::string_view Signals[] =
            {
                "REFLECT",
                "GENERATED_BODY",
                "PROPERTY",
                "FUNCTION",
                "SCRIPT_EXPORT",
                ".generated.h",
            };

            return std::ranges::any_of(Signals, [&Contents](std::string_view Signal)
            {
                return Contents.find(Signal) != std::string::npos;
            });
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
        bool ReportClangDiagnostics(CXTranslationUnit TranslationUnit, const FClangParserContext& Context, bool bStrict, bool bVerbose)
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

                // The root cause often sits in a system or third-party header, where it is dropped below.
                if (bSevere && bVerbose && !IsReflectedHeader(Context, Loc))
                {
                    CXString VerboseSpelling = clang_getDiagnosticSpelling(Diagnostic);
                    const char* VerboseRaw = clang_getCString(VerboseSpelling);
                    FDiagnostics::Get().Warningf(Loc, EDiagId::DriverClangDiagnostic, "%s",
                        VerboseRaw != nullptr ? VerboseRaw : "clang reported an error with no message");
                    clang_disposeString(VerboseSpelling);
                }

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

        FScopedPhaseTimer AmalgamationTimer("  amalgamation + args");
        
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

        uint32_t NumAmalgamationRoots = 0;

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

            // Sorted, or unspecified map order reshuffles the amalgamation and a bad parse comes and goes.
            std::vector<std::pair<FStringHash, FReflectedHeader*>> OrderedHeaders;
            OrderedHeaders.reserve(Project->Headers.size());

            for (auto& [Path, Header] : Project->Headers)
            {
                OrderedHeaders.emplace_back(Path, Header.get());
            }

            std::sort(OrderedHeaders.begin(), OrderedHeaders.end(),
                [](const auto& A, const auto& B)
                {
                    return std::strcmp(A.first.c_str(), B.first.c_str()) < 0;
                });

            for (const auto& [Path, Header] : OrderedHeaders)
            {
                // Registered whatever happens, so a header something else includes is still walked.
                ParsingContext.AllHeaders.emplace(Path, Header);
                ParsingContext.NumHeadersReflected++;

                if (!MayCarryReflection(Path.c_str()))
                {
                    continue;
                }

                AmalgamationFile << "#include \"" << Path.c_str() << "\"\n";
                ++NumAmalgamationRoots;
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
        if (const std::string ResourceDir = FindClangResourceDir(); !ResourceDir.empty())
        {
            AppendArg("-resource-dir=" + ResourceDir);
        }
        AppendArg("-O0");
        AppendArg("-DREFLECTION_PARSER");
#if defined(_WIN32)
        // libclang trails the MSVC STL's supported-compiler floor, and its version assert poisons <type_traits>.
        AppendArg("-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH");
        // MSVC's offsetof is a reinterpret_cast that is never constexpr; this selects __builtin_offsetof.
        AppendArg("-D_CRT_USE_BUILTIN_OFFSETOF");
        // A Debug target's _STL_VERIFY calls builtins this libclang lacks, breaking the STL types it guards.
        AppendArg("-D_ITERATOR_DEBUG_LEVEL=0");
        AppendArg("-D_CONTAINER_DEBUG_LEVEL=0");
        // These exist to parse the MSVC STL. Elsewhere the standard library is libstdc++, and MS mode
        // demotes char16_t/char32_t to the library typedefs only <vcruntime.h> supplies.
        AppendArg("-fms-extensions");
        AppendArg("-fms-compatibility");
#endif
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

        AmalgamationTimer.Stop();

        ClangIndex = clang_createIndex(0, 0);
        
        // Nothing here completes code, and caching completion results walks every visible declaration.
        constexpr uint32_t ClangOptions =
            CXTranslationUnit_DetailedPreprocessingRecord |
            CXTranslationUnit_SkipFunctionBodies |
            CXTranslationUnit_KeepGoing;
        
        CXErrorCode Result;
        {
        FScopedPhaseTimer LibclangTimer("  libclang parse");
        Result = clang_parseTranslationUnit2(
            ClangIndex,
            AmalgamationPath.c_str(),
            ClangArgs.data(),
            (int)ClangArgs.size(),
            nullptr,
            0,
            ClangOptions,
            &TranslationUnit);
        }
        
        // Walking a broken AST would emit confident, wrong reflection data, so stop before it.
        if (!ReportClangDiagnostics(TranslationUnit, ParsingContext, bStrictParse, bVerboseDiagnostics))
        {
            std::filesystem::remove(AmalgamationPath.c_str());
            clang_disposeIndex(ClangIndex);
            return false;
        }

        CXCursor Cursor = clang_getTranslationUnitCursor(TranslationUnit);

        FScopedPhaseTimer WalkTimer("  AST walk");
        const int WalkResult = clang_visitChildren(Cursor, VisitTranslationUnit, &ParsingContext);
        WalkTimer.Stop();

        if (GReportTimings)
        {
            std::printf("[timing]   cursors=%llu inReflectedHeaders=%llu headers=%u roots=%u\n",
                (unsigned long long)GCursorsVisited, (unsigned long long)GCursorsInReflectedHeaders,
                ParsingContext.NumHeadersReflected, NumAmalgamationRoots);
        }

        // A non-zero return abandons every later cursor, so reaching this is a defect in the walk itself.
        if (WalkResult != 0)
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
