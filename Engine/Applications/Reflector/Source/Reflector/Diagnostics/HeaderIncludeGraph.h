#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <cstdint>

namespace Lumina::Reflection
{
    class FReflectedWorkspace;

    // A cycle is the chain of headers a -> b -> ... -> a, where the first and
    // last entries are the same path.
    using FHeaderCycle = std::vector<std::string>;

    // Directed #include graph across a workspace's headers, walked for cycles via a lightweight
    // line-based regex (not a full preprocessor): quoted includes only, ignores #if guards, stops at project roots.
    class FHeaderIncludeGraph
    {
    public:

        struct FResolvedInclude
        {
            std::string Path;     // normalized absolute path (lowercase, '/').
            uint32_t      Line = 0; // 1-based line in the includer.
        };

        struct FNode
        {
            std::string                   Path;       // owning normalized path
            std::vector<FResolvedInclude> Includes;   // outgoing edges
        };

        // Seeds from every reflected header and recursively walks includes resolving inside a project root.
        void BuildFromWorkspace(FReflectedWorkspace* Workspace);

        // One entry per cycle, deduplicated by canonical (lexicographically minimum) rotation.
        std::vector<FHeaderCycle> DetectCycles() const;

        // Line in IncluderPath that #includes IncludeePath, or 0 if no direct edge exists.
        uint32_t GetIncludeLine(const std::string& IncluderPath, const std::string& IncludeePath) const;

    private:

        // Pulls the textual #include "..." directives out of a single header.
        // Returns false if the file couldn't be opened.
        bool ScanHeader(const std::string& AbsPath, std::vector<std::pair<std::string, uint32_t>>& OutIncludes) const;

        // Tries IncluderDir/Text first, then each include search dir. Returns
        // an empty string when nothing resolves to an existing file.
        std::string ResolveInclude(
            const std::string& IncludeText,
            const std::string& IncluderDir,
            const std::vector<std::string>& IncludeDirs) const;

        // True when AbsPath sits beneath a project root; keeps the crawl bounded to the workspace.
        bool IsInsideProjectRoots(const std::string& AbsPath) const;

        std::unordered_map<std::string, FNode>   Nodes;
        // Resolution is deterministic per includer directory, so the search dirs are walked once per edge.
        mutable std::unordered_map<std::string, std::string> ResolvedIncludes;
        std::vector<std::string>            ProjectRoots;       // normalized lowercase prefixes
        std::vector<std::string>            AllIncludeDirs;     // union across projects
    };
}
