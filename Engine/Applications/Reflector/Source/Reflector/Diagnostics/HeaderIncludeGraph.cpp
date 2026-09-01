#include "HeaderIncludeGraph.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

#include "Reflector/ProjectSolution.h"
#include "Reflector/Utils/FileIO.h"
#include "Reflector/ReflectionCore/ReflectedHeader.h"
#include "Reflector/ReflectionCore/ReflectedProject.h"

namespace Lumina::Reflection
{
    namespace
    {
        // Mirrors NormalizeHeaderPath so these compare equal to AllHeaders entries on every filesystem.
        std::string NormalizeUncached(const std::filesystem::path& InPath)
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

        // weakly_canonical stats every component, and the crawl asks about the same paths over and over.
        const std::string& Normalize(const std::string& InPath)
        {
            static std::unordered_map<std::string, std::string> Cache;

            const auto Entry = Cache.find(InPath);
            if (Entry != Cache.end())
            {
                return Entry->second;
            }

            return Cache.emplace(InPath, NormalizeUncached(std::filesystem::path(InPath.c_str()))).first->second;
        }

        bool StartsWith(const std::string& Haystack, const std::string& Needle)
        {
            if (Needle.size() > Haystack.size())
            {
                return false;
            }
            return std::memcmp(Haystack.data(), Needle.data(), Needle.size()) == 0;
        }

        // Returns empty when the path has no parent. Path is already normalized, so this only slices it.
        std::string ParentDir(const std::string& Path)
        {
            const size_t Slash = Path.find_last_of('/');
            if (Slash == std::string::npos || Slash == 0)
            {
                return {};
            }
            return Path.substr(0, Slash);
        }

        constexpr bool SkipLiteral(std::string_view& Text, char Character)
        {
            while (!Text.empty() && (Text.front() == ' ' || Text.front() == '\t'))
            {
                Text.remove_prefix(1);
            }

            if (Text.empty() || Text.front() != Character)
            {
                return false;
            }

            Text.remove_prefix(1);
            return true;
        }

        // Angle-bracket includes are skipped, since system headers cannot be resolved into the workspace.
        constexpr bool TryMatchQuotedInclude(std::string_view LineText, std::string_view& OutQuoted)
        {
            if (!SkipLiteral(LineText, '#'))
            {
                return false;
            }

            while (!LineText.empty() && (LineText.front() == ' ' || LineText.front() == '\t'))
            {
                LineText.remove_prefix(1);
            }

            constexpr std::string_view Keyword = "include";
            if (!LineText.starts_with(Keyword))
            {
                return false;
            }
            LineText.remove_prefix(Keyword.size());

            if (!SkipLiteral(LineText, '"'))
            {
                return false;
            }

            const size_t Close = LineText.find('"');
            if (Close == std::string_view::npos)
            {
                return false;
            }

            OutQuoted = LineText.substr(0, Close);
            return true;
        }

        // The .inl idiom where X.h includes X.inl includes X.h is deliberate, not a cycle.
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
        std::string Contents;
        if (!ReadWholeFile(AbsPath, Contents))
        {
            return false;
        }

        const std::string_view Text(Contents);
        uint32_t Line = 0;

        for (size_t Cursor = 0; Cursor < Text.size(); )
        {
            ++Line;

            size_t LineEnd = Text.find('\n', Cursor);
            if (LineEnd == std::string_view::npos)
            {
                LineEnd = Text.size();
            }

            std::string_view Quoted;
            if (TryMatchQuotedInclude(Text.substr(Cursor, LineEnd - Cursor), Quoted))
            {
                OutIncludes.emplace_back(std::string(Quoted), Line);
            }

            Cursor = LineEnd + 1;
        }

        return true;
    }

    std::string FHeaderIncludeGraph::ResolveInclude(
        const std::string& IncludeText,
        const std::string& IncluderDir,
        const std::vector<std::string>& IncludeDirs) const
    {
        // One key per distinct edge, since the same include is written from the same directory many times.
        std::string Key = IncluderDir;
        Key.push_back('\n');
        Key.append(IncludeText);

        const auto Memo = ResolvedIncludes.find(Key);
        if (Memo != ResolvedIncludes.end())
        {
            return Memo->second;
        }

        std::string Candidate;

        auto TryDirectory = [&](const std::string& Dir) -> bool
        {
            Candidate.assign(Dir);
            Candidate.push_back('/');
            Candidate.append(IncludeText);

            std::error_code Ec;
            return std::filesystem::exists(std::filesystem::path(Candidate.c_str()), Ec) && !Ec;
        };

        auto Remember = [&](std::string Resolved) -> std::string
        {
            ResolvedIncludes.emplace(std::move(Key), Resolved);
            return Resolved;
        };

        // Relative to the includer first, matching `#include "Sibling.h"`.
        if (!IncluderDir.empty() && TryDirectory(IncluderDir))
        {
            return Remember(Normalize(Candidate));
        }

        // Then the include search dirs in order, same as clang would.
        for (const std::string& Dir : IncludeDirs)
        {
            if (TryDirectory(Dir))
            {
                return Remember(Normalize(Candidate));
            }
        }

        return Remember({});
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

        // Aggregated once so resolution has a single search list to walk for any header in the graph.
        std::vector<std::string> Seeds;
        for (const auto& Project : Workspace->ReflectedProjects)
        {
            const std::string ProjectRoot = Normalize(Project->Path);
            // The trailing slash stops a prefix match false-positiving on a sibling such as Runtime vs RuntimeX.
            ProjectRoots.push_back(ProjectRoot + "/");

            for (const std::string& Dir : Project->IncludeDirs)
            {
                std::string Norm = Normalize(Dir);
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

        // Stops at non-project paths so third-party cycles are never reported to the user.
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
                    // An .inl is intentionally self-referential with its owning header, so skip the edge.
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
        // Standard three-color DFS, where hitting a Gray node means the active stack holds a cycle.
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
                    // Walking the stack back to Target's first appearance marks where the cycle starts.
                    auto StackIt = std::find(Stack.begin(), Stack.end(), Target);
                    if (StackIt == Stack.end())
                    {
                        continue;
                    }

                    FHeaderCycle Cycle(StackIt, Stack.end());
                    Cycle.push_back(Target);   // close the loop, last == first

                    // Rotated so the smallest path is first, giving identical keys for any start node on the same loop.
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

                // White, so descend into Target.
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
