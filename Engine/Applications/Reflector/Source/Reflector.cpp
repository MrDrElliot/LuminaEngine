#include "Reflector/Utils/StringOps.h"
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
    
    std::string InputFile = "H:/LuminaEngine/Reflection_Files.json";
    
#else
    if (argc < 2)
    {
        FDiagnostics::Get().Errorf({}, EDiagId::DriverMissingInput,
            "Missing command line argument: a Reflection_Files.json path is required.");
        FDiagnostics::Get().PrintSummary();
        return 1;
    }

    std::string InputFile = argv[1];
#endif

    std::ifstream File(InputFile.c_str());
    if (!File.is_open())
    {
        // Anchoring at the JSON path lets a double-click on the build log open the missing file.
        FDiagLocation Loc;
        Loc.File = InputFile;
        FDiagnostics::Get().Errorf(Loc, EDiagId::DriverInputUnreadable,
            "Failed to open Reflector input file '%s'.", InputFile.c_str());
        FDiagnostics::Get().PrintSummary();
        return 1;
    }
    
    
    json Data = json::parse(File);
    
    std::string WorkspaceName     = Data["WorkspaceName"].get<std::string>().c_str();
    std::string WorkspacePath     = Data["WorkspacePath"].get<std::string>().c_str();
    
    FReflectedWorkspace Workspace(WorkspacePath.c_str());
    
    for (const auto& Project : Data["Projects"])
    {
        std::string ProjectName = Project["Name"].get<std::string>().c_str();
        std::string ProjectPath = Project["Path"].get<std::string>().c_str();
        
        auto ReflectedProject = std::make_unique<FReflectedProject>(&Workspace);
        ReflectedProject->Name = std::move(ProjectName);
        // Normalized so prefix-matching against Header->HeaderPath works without per-call fixups.
        ReflectedProject->Path = Lumina::ClangUtils::NormalizeHeaderPath(std::move(ProjectPath));

        // A reference-only project is parsed so its types are known, but never generated for.
        if (Project.contains("ReferenceOnly"))
        {
            ReflectedProject->bReferenceOnly = Project["ReferenceOnly"].get<bool>();
        }

        // Optional, the build system pins where this project's generated C++ lands.
        if (Project.contains("GeneratedDir") && !Project["GeneratedDir"].get<std::string>().empty())
        {
            std::string GeneratedDir = Project["GeneratedDir"].get<std::string>().c_str();
            ReflectedProject->GeneratedDir = Lumina::ClangUtils::NormalizeHeaderPath(std::move(GeneratedDir));
        }

        // Absent or empty means the module has none, and generated sources then include no PCH.
        if (Project.contains("PrecompiledHeader"))
        {
            ReflectedProject->PrecompiledHeader = Project["PrecompiledHeader"].get<std::string>().c_str();
        }

        // Optional, a plugin or game module routes its C# bindings into its own Scripts/Generated dir.
        if (Project.contains("CSharpBindingsDir") && !Project["CSharpBindingsDir"].get<std::string>().empty())
        {
            std::string CSharpDir = Project["CSharpBindingsDir"].get<std::string>().c_str();
            ReflectedProject->CSharpBindingsDir = Lumina::ClangUtils::NormalizeHeaderPath(std::move(CSharpDir));
        }

        if (Project.contains("RouteTypeBindings"))
        {
            ReflectedProject->bRouteTypeBindings = Project["RouteTypeBindings"].get<bool>();
        }

        for (const auto& IncludeDirJson : Project["IncludeDirs"])
        {
            std::string IncludeDir = IncludeDirJson.get<std::string>().c_str();
            IncludeDir = Lumina::ClangUtils::NormalizeHeaderPath(std::move(IncludeDir));
            ReflectedProject->IncludeDirs.push_back(std::move(IncludeDir));
        }
        
        if (Project.contains("Definitions"))
        {
            for (const auto& DefinitionJson : Project["Definitions"])
            {
                ReflectedProject->Definitions.push_back(DefinitionJson.get<std::string>().c_str());
            }
        }

        if (Project.contains("ForceIncludes"))
        {
            for (const auto& ForceIncludeJson : Project["ForceIncludes"])
            {
                std::string ForceInclude = ForceIncludeJson.get<std::string>().c_str();
                ForceInclude = Lumina::ClangUtils::NormalizeHeaderPath(std::move(ForceInclude));
                ReflectedProject->ForceIncludes.push_back(std::move(ForceInclude));
            }
        }

        for (const auto& ProjectFileJson : Project["Files"])
        {
            std::string ProjectFile = ProjectFileJson.get<std::string>().c_str();
            ProjectFile = Lumina::ClangUtils::NormalizeHeaderPath(std::move(ProjectFile));

            auto ReflectedHeader = std::make_unique<FReflectedHeader>(ReflectedProject.get(), ProjectFile);

            Lumina::FStringHash HeaderHash(ProjectFile);
            ReflectedProject->Headers.emplace(HeaderHash, std::move(ReflectedHeader));
        }
        
        Workspace.AddReflectedProject(std::move(ReflectedProject));
    }

    // Static include-graph pass first, since cycles otherwise surface as confusing parse errors.
    {
        FHeaderIncludeGraph Graph;
        Graph.BuildFromWorkspace(&Workspace);

        const auto Cycles = Graph.DetectCycles();
        for (const FHeaderCycle& Cycle : Cycles)
        {
            // Build a "A.h -> B.h -> A.h" arrow chain for the message body.
            std::string Arrow;
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
            // Bail before parsing, or clang chews for tens of seconds on a broken workspace.
            FDiagnostics::Get().PrintSummary();
            return 1;
        }
    }

    FClangParser Parser;

    for (int i = 2; i < argc; ++i)
    {
        Parser.bStrictParse |= std::string(argv[i]) == "-strict-parse";
    }

    bool bParseResult = Parser.Parse(&Workspace);

    if (!bParseResult)
    {
        FDiagnostics::Get().PrintSummary();
        return 1;
    }

    // Any header with a reflection macro must end its include block with the matching generated.h.
    for (const auto& Project : Workspace.ReflectedProjects)
    {
        // Someone else's module, already validated by the workspace that owns it.
        if (Project->bReferenceOnly)
        {
            continue;
        }

        for (auto& [_, Header] : Project->Headers)
        {
            if (!Header->bHasReflectionMacros)
            {
                continue;
            }

            // find() not operator[], since the latter inserts empty entries the codegen would emit empty files for.
            auto TypeIt = Parser.ParsingContext.ReflectionDatabase.ReflectedTypes.find(Header.get());
            if (TypeIt != Parser.ParsingContext.ReflectionDatabase.ReflectedTypes.end() && !TypeIt->second.empty())
            {
                // An alias has no GENERATED_BODY to feed, so its header needs no companion include.
                bool bAllAliases = true;
                for (const auto& T : TypeIt->second)
                {
                    if (!T->bIsAlias)
                    {
                        bAllAliases = false;
                        break;
                    }
                }
                if (bAllAliases)
                {
                    continue;
                }
            }

            std::string ExpectedGenerated = Header->FileName + ".generated.h";
            Lumina::StringOps::ToLower(ExpectedGenerated);

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

            // The right generated.h is included, now verify it is the last include in the file.
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

    // An unreflected property type emits a Construct_ call with nothing to call, so name the header.
    for (const auto& Project : Workspace.ReflectedProjects)
    {
        if (Project->bReferenceOnly)
        {
            continue;
        }

        for (auto& [_, Header] : Project->Headers)
        {
            auto TypeIt = Parser.ParsingContext.ReflectionDatabase.ReflectedTypes.find(Header.get());
            if (TypeIt == Parser.ParsingContext.ReflectionDatabase.ReflectedTypes.end())
            {
                continue;
            }

            for (const auto& Type : TypeIt->second)
            {
                auto* Struct = dynamic_cast<FReflectedStruct*>(Type.get());
                if (Struct == nullptr)
                {
                    continue;
                }

                for (const auto& Property : Struct->Props)
                {
                    if (!Property->CanDeclareCrossModuleReferences())
                    {
                        continue;
                    }

                    // Empty means deliberately unconstrained (a bare FInstancedStruct), so there is no base to demand.
                    if (Property->TypeName.empty())
                    {
                        continue;
                    }

                    const FReflectedType* Referenced =
                        Parser.ParsingContext.ReflectionDatabase.GetReflectedType<FReflectedType>(
                            Lumina::FStringHash(Property->TypeName));

                    FDiagLocation Loc;
                    Loc.File = Header->HeaderPath;
                    Loc.Line = Type->LineNumber;

                    if (Referenced != nullptr)
                    {
                        const auto* AsStruct = dynamic_cast<const FReflectedStruct*>(Referenced);
                        // A value mirror exists for the C# marshal alone, so a PROPERTY of it would store nothing.
                        if (AsStruct != nullptr && AsStruct->Props.empty()
                            && Referenced->HasMetadata("CSharpValueMirror"))
                        {
                            FDiagnostics::Get().Errorf(Loc, EDiagId::ScriptOnlyPropertyType,
                                "Property '%s' on '%s' has type '%s', which is reflected only as a hand-written C# "
                                "value mirror and carries no members. It can cross to script by value but cannot be "
                                "serialized or edited. Remove the PROPERTY() macro from this field.",
                                Property->Name.c_str(), Type->DisplayName.c_str(), Property->TypeName.c_str());
                        }
                        continue;
                    }

                    FDiagnostics::Get().Errorf(Loc, EDiagId::UnreflectedPropertyType,
                        "Property '%s' on '%s' has type '%s', which is not reflected. "
                        "Give that type a REFLECT() + GENERATED_BODY(), or remove the PROPERTY() macro from this field.",
                        Property->Name.c_str(), Type->DisplayName.c_str(), Property->TypeName.c_str());
                }
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

    // Non-zero exit halts the build; the diagnostic lines were already printed when emitted.
    FDiagnostics::Get().PrintSummary();
    return FDiagnostics::Get().GetErrorCount() == 0 ? 0 : 1;
}
