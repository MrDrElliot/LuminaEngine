#include <fstream>
#include "StringHash.h"
#include "nlohmann/json.hpp"
#include "Reflector/Clang/Utils.h"
#include "Reflector/Diagnostics/HeaderIncludeGraph.h"
#include "Reflector/Diagnostics/LRTDiagnostics.h"
#include "Reflector/ProjectSolution.h"
#include "Reflector/Clang/ClangParser.h"
#include "Reflector/CodeGeneration/CodeGenerator.h"
#include "Reflector/ReflectionCore/ReflectedProject.h"
#include <cstdio>


using json = nlohmann::json;
using namespace Lumina::Reflection;

int main(int argc, char* argv[])
{    
    Lumina::FStringHash::Initialize();
    
    std::puts("===============================================");
    std::puts("======== Lumina Reflection Tool (LRT) =========");
    std::puts("===============================================");
    
#if 0
    
    eastl::string InputFile = "H:/LuminaEngine/Reflection_Files.json";
    
#else
    if (argc < 2)
    {
        FDiagnostics::Get().Errorf({}, EDiagId::DriverMissingInput,
            "Missing command line argument: a Reflection_Files.json path is required.");
        FDiagnostics::Get().PrintSummary();
        return 1;
    }

    eastl::string InputFile = argv[1];
#endif

    std::ifstream File(InputFile.c_str());
    if (!File.is_open())
    {
        // Anchor the diagnostic at the JSON path so double-clicking the build
        // log error opens the missing file in the IDE.
        FDiagLocation Loc;
        Loc.File = InputFile;
        FDiagnostics::Get().Errorf(Loc, EDiagId::DriverInputUnreadable,
            "Failed to open Reflector input file '%s'.", InputFile.c_str());
        FDiagnostics::Get().PrintSummary();
        return 1;
    }
    
    
    json Data = json::parse(File);
    
    eastl::string WorkspaceName     = Data["WorkspaceName"].get<std::string>().c_str();
    eastl::string WorkspacePath     = Data["WorkspacePath"].get<std::string>().c_str();
    
    FReflectedWorkspace Workspace(WorkspacePath.c_str());
    
    for (const auto& Project : Data["Projects"])
    {
        eastl::string ProjectName = Project["Name"].get<std::string>().c_str();
        eastl::string ProjectPath = Project["Path"].get<std::string>().c_str();
        
        auto ReflectedProject = eastl::make_unique<FReflectedProject>(&Workspace);
        ReflectedProject->Name = eastl::move(ProjectName);
        // Normalize so prefix-matching against Header->HeaderPath (also normalized)
        // works without per-call slash/case fixups.
        ReflectedProject->Path = Lumina::ClangUtils::NormalizeHeaderPath(eastl::move(ProjectPath));

        // Optional: a plugin/game module routes its C# bindings into its own Scripts/Generated dir.
        if (Project.contains("CSharpBindingsDir") && !Project["CSharpBindingsDir"].get<std::string>().empty())
        {
            eastl::string CSharpDir = Project["CSharpBindingsDir"].get<std::string>().c_str();
            ReflectedProject->CSharpBindingsDir = Lumina::ClangUtils::NormalizeHeaderPath(eastl::move(CSharpDir));
        }

        for (const auto& IncludeDirJson : Project["IncludeDirs"])
        {
            eastl::string IncludeDir = IncludeDirJson.get<std::string>().c_str();
            IncludeDir = Lumina::ClangUtils::NormalizeHeaderPath(eastl::move(IncludeDir));
            ReflectedProject->IncludeDirs.push_back(eastl::move(IncludeDir));
        }
        
        for (const auto& ProjectFileJson : Project["Files"])
        {
            eastl::string ProjectFile = ProjectFileJson.get<std::string>().c_str();
            ProjectFile = Lumina::ClangUtils::NormalizeHeaderPath(eastl::move(ProjectFile));

            auto ReflectedHeader = eastl::make_unique<FReflectedHeader>(ReflectedProject.get(), ProjectFile);

            Lumina::FStringHash HeaderHash(ProjectFile);
            ReflectedProject->Headers.emplace(HeaderHash, eastl::move(ReflectedHeader));
        }
        
        Workspace.AddReflectedProject(eastl::move(ReflectedProject));
    }

    // Static include-graph pass before libclang: cycles otherwise produce confusing
    // downstream parse errors; surfacing them up front gives a clean LRT error.
    {
        FHeaderIncludeGraph Graph;
        Graph.BuildFromWorkspace(&Workspace);

        const auto Cycles = Graph.DetectCycles();
        for (const FHeaderCycle& Cycle : Cycles)
        {
            // Build a "A.h -> B.h -> A.h" arrow chain for the message body.
            eastl::string Arrow;
            for (size_t i = 0; i < Cycle.size(); ++i)
            {
                if (i > 0)
                {
                    Arrow += " -> ";
                }
                Arrow += Cycle[i];
            }

            // Anchor at the cycle's first include edge so the build-log error opens the offending line.
            FDiagLocation Loc;
            Loc.File = Cycle.front();
            if (Cycle.size() >= 2)
            {
                Loc.Line = Graph.GetIncludeLine(Cycle[0], Cycle[1]);
            }

            FDiagnostics::Get().Errorf(Loc, EDiagId::CircularHeaderInclude,
                "Circular header include: %s", Arrow.c_str());
        }

        if (!Cycles.empty())
        {
            // Bail before parsing -- clang would otherwise spend tens of
            // seconds chewing on a workspace that has a structural defect.
            FDiagnostics::Get().PrintSummary();
            return 1;
        }
    }

    FClangParser Parser;
    bool bParseResult = Parser.Parse(&Workspace);

    if (!bParseResult)
    {
        FDiagnostics::Get().PrintSummary();
        return 1;
    }

    // Per-header include validation: any header with a reflection macro must end its
    // include block with `<stem>.generated.h` (catches missing/misordered/wrong-file includes).
    for (const auto& Project : Workspace.ReflectedProjects)
    {
        for (auto& [_, Header] : Project->Headers)
        {
            if (!Header->bHasReflectionMacros)
            {
                continue;
            }

            // ManualStub-only headers can't include their .generated.h (forward-decl clashes with the `using` alias).
            // find() not operator[]: the latter inserts empty entries the codegen would emit empty files for.
            auto TypeIt = Parser.ParsingContext.ReflectionDatabase.ReflectedTypes.find(Header.get());
            if (TypeIt != Parser.ParsingContext.ReflectionDatabase.ReflectedTypes.end() && !TypeIt->second.empty())
            {
                bool bAllManualStubs = true;
                for (const auto& T : TypeIt->second)
                {
                    if (!T->HasMetadata("ManualStub"))
                    {
                        bAllManualStubs = false;
                        break;
                    }
                }
                if (bAllManualStubs)
                {
                    continue;
                }
            }

            eastl::string ExpectedGenerated = Header->FileName + ".generated.h";
            ExpectedGenerated.make_lower();

            const FIncludeRef* GeneratedInclude = nullptr;
            const FIncludeRef* WrongGenerated   = nullptr;

            constexpr const char* kGeneratedSuffix = ".generated.h";
            constexpr size_t      kGeneratedSuffixLen = 12;

            for (const FIncludeRef& Inc : Header->Includes)
            {
                const bool bEndsWithGen = Inc.Basename.size() >= kGeneratedSuffixLen &&
                    Inc.Basename.compare(Inc.Basename.size() - kGeneratedSuffixLen, kGeneratedSuffixLen, kGeneratedSuffix) == 0;
                if (!bEndsWithGen)
                {
                    continue;
                }

                if (Inc.Basename == ExpectedGenerated)
                {
                    GeneratedInclude = &Inc;
                }
                else if (WrongGenerated == nullptr)
                {
                    WrongGenerated = &Inc;
                }
            }

            FDiagLocation HeaderLoc;
            HeaderLoc.File = Header->HeaderPath;

            if (GeneratedInclude == nullptr)
            {
                if (WrongGenerated != nullptr)
                {
                    FDiagLocation Loc = HeaderLoc;
                    Loc.Line = WrongGenerated->LineNumber;
                    FDiagnostics::Get().Errorf(Loc, EDiagId::WrongGeneratedHeader,
                        "Header includes '%s' but reflection expects '%s'. "
                        "The generated header name must match the source filename stem.",
                        WrongGenerated->Spelling.c_str(), ExpectedGenerated.c_str());
                }
                else
                {
                    HeaderLoc.Line = 1;
                    FDiagnostics::Get().Errorf(HeaderLoc, EDiagId::MissingGeneratedHeader,
                        "Header uses REFLECT/GENERATED_BODY/PROPERTY/FUNCTION but does not "
                        "#include \"%s\". Add it as the last include in the file.",
                        ExpectedGenerated.c_str());
                }
                continue;
            }

            // Confirmed the right generated.h is included; now verify it's
            // the last include in the file.
            const FIncludeRef* LaterInclude = nullptr;
            for (const FIncludeRef& Inc : Header->Includes)
            {
                if (Inc.LineNumber > GeneratedInclude->LineNumber && LaterInclude == nullptr)
                {
                    LaterInclude = &Inc;
                }
            }

            if (LaterInclude != nullptr)
            {
                FDiagLocation Loc = HeaderLoc;
                Loc.Line = LaterInclude->LineNumber;
                FDiagnostics::Get().Errorf(Loc, EDiagId::GeneratedHeaderNotLast,
                    "'%s' must be the last #include in this header, but '%s' follows it.",
                    ExpectedGenerated.c_str(), LaterInclude->Spelling.c_str());
            }
        }
    }

    if (FDiagnostics::Get().GetErrorCount() != 0)
    {
        FDiagnostics::Get().PrintSummary();
        return 1;
    }

    FCodeGenerator CodeGenerator(&Workspace, Parser.ParsingContext.ReflectionDatabase);

    CodeGenerator.GenerateCode();

    Lumina::FStringHash::Shutdown();

    // If any LRT diagnostics fired we surface a non-zero exit so the build
    // halts; the diagnostic lines themselves were already printed when emitted.
    FDiagnostics::Get().PrintSummary();
    return FDiagnostics::Get().GetErrorCount() == 0 ? 0 : 1;
}
