#include "HeaderIncludeGraph.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

#include "Reflector/ProjectSolution.h"
#include "Reflector/ReflectionCore/ReflectedHeader.h"
#include "Reflector/ReflectionCore/ReflectedProject.h"

namespace Lumina::Reflection
{
    namespace
    {
        // Forward slashes + absolute, case preserved; mirrors NormalizeHeaderPath
        // so these strings compare equal to AllHeaders entries on every filesystem.
        std::string Normalize(const std::filesystem::path& InPath)
        {
            std::error_code Ec;
            std::filesystem::path Abs = std::filesystem::weakly_canonical(InPath, Ec);
            if (Ec)
            {
                Abs = std::filesystem::absolute(InPath, Ec);
            }

            std::string Result = Abs.string().c_str();
            std::replace(Result.begin(), Result.end(), '\\', '/');
            return Result;
        }

        bool StartsWith(const std::string& Haystack, const std::string& Needle)
        {
            if (Needle.size() > Haystack.size())
            {
                return false;
            }
            return std::memcmp(Haystack.data(), Needle.data(), Needle.size()) == 0;
        }

        // Extract the parent directory of `Path`, normalized. Empty if the path
        // has no parent.
        std::string ParentDir(const std::string& Path)
        {
            std::filesystem::path P(Path.c_str());
            std::filesystem::path Parent = P.parent_path();
            if (Parent.empty())
            {
                return {};
            }
            return Normalize(Parent);
        }

        // Paths the cycle detector ignores even when they resolve: the `.inl` idiom
        // (X.h includes X.inl includes X.h) is a deliberate pattern, not a cycle.
        bool IsExtensionIgnored(const std::string& Path)
        {
            const size_t Dot = Path.find_last_of('.');
            if (Dot == std::string::npos)
            {
                return false;
            }
            return Path.compare(Dot, std::string::npos, ".inl") == 0;
        }
    }

    bool FHeaderIncludeGraph::ScanHeader(const std::string& AbsPath, std::vector<std::pair<std::string, uint32_t>>& OutIncludes) const
    {
        std::ifstream File(AbsPath.c_str());
        if (!File.is_open())
        {
            return false;
        }

        // Match `#include "x"` (any inter-token whitespace); `#include <...>` is
        // skipped since system/external headers can't be resolved into the workspace.
        static const std::regex IncludeRegex(R"(^\s*#\s*include\s*\"([^\"]+)\")");

        std::string LineBuf;
        uint32_t Line = 0;
        while (std::getline(File, LineBuf))
        {
            ++Line;

            std::smatch Match;
            if (std::regex_search(LineBuf, Match, IncludeRegex))
            {
                OutIncludes.emplace_back(std::pair<std::string, uint32_t>{ Match[1].str().c_str(), Line });
            }
        }

        return true;
    }

    std::string FHeaderIncludeGraph::ResolveInclude(
        const std::string& IncludeText,
        const std::string& IncluderDir,
        const std::vector<std::string>& IncludeDirs) const
    {
        // 1) Try relative to the includer (matches `#include "Sibling.h"`).
        if (!IncluderDir.empty())
        {
            std::filesystem::path Candidate = std::filesystem::path(IncluderDir.c_str()) / IncludeText.c_str();
            std::error_code Ec;
            if (std::filesystem::exists(Candidate, Ec) && !Ec)
            {
                return Normalize(Candidate);
            }
        }

        // 2) Walk the include search dirs in order, same as clang would.
        for (const std::string& Dir : IncludeDirs)
        {
            std::filesystem::path Candidate = std::filesystem::path(Dir.c_str()) / IncludeText.c_str();
            std::error_code Ec;
            if (std::filesystem::exists(Candidate, Ec) && !Ec)
            {
                return Normalize(Candidate);
            }
        }

        return {};
    }

    bool FHeaderIncludeGraph::IsInsideProjectRoots(const std::string& AbsPath) const
    {
        for (const std::string& Root : ProjectRoots)
        {
            if (StartsWith(AbsPath, Root))
            {
                return true;
            }
        }
        return false;
    }

    void FHeaderIncludeGraph::BuildFromWorkspace(FReflectedWorkspace* Workspace)
    {
        Nodes.clear();
        ProjectRoots.clear();
        AllIncludeDirs.clear();

        if (Workspace == nullptr)
        {
            return;
        }

        // Aggregate roots + the union of include dirs once. Resolution then has
        // a single search list to walk for any header in the graph.
        std::vector<std::string> Seeds;
        for (const auto& Project : Workspace->ReflectedProjects)
        {
            const std::string ProjectRoot = Normalize(std::filesystem::path(Project->Path.c_str()));
            // Trailing '/' guarantees prefix-match doesn't false-positive on
            // sibling dirs that share a prefix (e.g. "Runtime" vs "RuntimeX").
            ProjectRoots.push_back(ProjectRoot + "/");

            for (const std::string& Dir : Project->IncludeDirs)
            {
                std::string Norm = Normalize(std::filesystem::path(Dir.c_str()));
                if (std::find(AllIncludeDirs.begin(), AllIncludeDirs.end(), Norm) == AllIncludeDirs.end())
                {
                    AllIncludeDirs.push_back(std::move(Norm));
                }
            }

            for (const auto& [PathHash, Header] : Project->Headers)
            {
                Seeds.push_back(Header->HeaderPath);
            }
        }

        // BFS-ish crawl from every reflected header, queuing files inside a project root.
        // Stops at non-project paths so third-party cycles aren't reported to the user.
        std::vector<std::string> Frontier = Seeds;
        while (!Frontier.empty())
        {
            const std::string Path = Frontier.back();
            Frontier.pop_back();

            if (Nodes.find(Path) != Nodes.end())
            {
                continue;
            }

            FNode Node;
            Node.Path = Path;

            std::vector<std::pair<std::string, uint32_t>> RawIncludes;
            const bool bRead = ScanHeader(Path, RawIncludes);
            if (!bRead)
            {
                Nodes.emplace(Path, std::move(Node));
                continue;
            }

            const std::string IncluderDir = ParentDir(Path);

            for (const auto& [IncludeText, IncludeLine] : RawIncludes)
            {
                std::string Resolved = ResolveInclude(IncludeText, IncluderDir, AllIncludeDirs);
                if (Resolved.empty())
                {
                    continue;
                }
                if (!IsInsideProjectRoots(Resolved))
                {
                    continue;
                }
                if (IsExtensionIgnored(Resolved))
                {
                    // .inl files are intentionally self-referential with their owning .h;
                    // skip the edge so the idiom isn't reported as a false-positive cycle.
                    continue;
                }

                Node.Includes.push_back({ Resolved, IncludeLine });

                if (Nodes.find(Resolved) == Nodes.end())
                {
                    Frontier.push_back(Resolved);
                }
            }

            Nodes.emplace(Path, std::move(Node));
        }
    }

    uint32_t FHeaderIncludeGraph::GetIncludeLine(const std::string& IncluderPath, const std::string& IncludeePath) const
    {
        const auto It = Nodes.find(IncluderPath);
        if (It == Nodes.end())
        {
            return 0;
        }
        for (const FResolvedInclude& Inc : It->second.Includes)
        {
            if (Inc.Path == IncludeePath)
            {
                return Inc.Line;
            }
        }
        return 0;
    }

    std::vector<FHeaderCycle> FHeaderIncludeGraph::DetectCycles() const
    {
        // Standard three-color DFS (White=unvisited, Gray=on stack, Black=done);
        // hitting a Gray node means the active stack holds a cycle's members.
        enum class EColor : uint8_t { White, Gray, Black };

        std::unordered_map<std::string, EColor> Color;
        Color.reserve(Nodes.size());
        for (const auto& [Path, _] : Nodes)
        {
            Color.emplace(Path, EColor::White);
        }

        std::vector<std::string> Stack;
        std::vector<FHeaderCycle> Cycles;
        std::unordered_map<std::string, bool> Reported; // dedup key

        // Iterative DFS to avoid blowing the C++ stack on giant graphs.
        struct FFrame
        {
            const FNode* Node;
            size_t       NextEdge;
        };

        for (const auto& [StartPath, _] : Nodes)
        {
            if (Color[StartPath] != EColor::White)
            {
                continue;
            }

            const auto StartIt = Nodes.find(StartPath);
            if (StartIt == Nodes.end())
            {
                continue;
            }

            std::vector<FFrame> Frames;
            Frames.push_back({ &StartIt->second, 0 });
            Color[StartPath] = EColor::Gray;
            Stack.push_back(StartPath);

            while (!Frames.empty())
            {
                FFrame& Top = Frames.back();
                if (Top.NextEdge >= Top.Node->Includes.size())
                {
                    Color[Top.Node->Path] = EColor::Black;
                    Stack.pop_back();
                    Frames.pop_back();
                    continue;
                }

                const std::string& Target = Top.Node->Includes[Top.NextEdge++].Path;

                auto ColorIt = Color.find(Target);
                if (ColorIt == Color.end())
                {
                    continue;
                }

                if (ColorIt->second == EColor::Black)
                {
                    continue;
                }

                if (ColorIt->second == EColor::Gray)
                {
                    // Walk the active stack back to find where Target first
                    // appeared -- that index marks the start of the cycle.
                    auto StackIt = std::find(Stack.begin(), Stack.end(), Target);
                    if (StackIt == Stack.end())
                    {
                        continue;
                    }

                    FHeaderCycle Cycle(StackIt, Stack.end());
                    Cycle.push_back(Target);   // close the loop: last == first

                    // Canonicalize: rotate so the smallest path is first, giving
                    // identical keys for any start node that walked the same loop.
                    auto MinIt = std::min_element(Cycle.begin(), Cycle.end() - 1);
                    std::vector<std::string> Canonical;
                    Canonical.reserve(Cycle.size());
                    Canonical.insert(Canonical.end(), MinIt, Cycle.end() - 1);
                    Canonical.insert(Canonical.end(), Cycle.begin(), MinIt);
                    Canonical.push_back(*MinIt);

                    std::string Key;
                    for (const std::string& Step : Canonical)
                    {
                        Key += Step;
                        Key += '|';
                    }

                    if (Reported.find(Key) == Reported.end())
                    {
                        Reported.emplace(Key, true);
                        Cycles.push_back(std::move(Canonical));
                    }
                    continue;
                }

                // White -- descend into Target.
                const auto TargetNodeIt = Nodes.find(Target);
                if (TargetNodeIt == Nodes.end())
                {
                    continue;
                }

                Color[Target] = EColor::Gray;
                Stack.push_back(Target);
                Frames.push_back({ &TargetNodeIt->second, 0 });
            }
        }

        return Cycles;
    }
}
