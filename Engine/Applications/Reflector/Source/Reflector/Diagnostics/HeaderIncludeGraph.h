#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <cstdint>

namespace Lumina::Reflection
{
    class FReflectedWorkspace;

    // A cycle is a chain of headers whose first and last entries are the same path.
    using FHeaderCycle = std::vector<std::string>;

    // Directed #include graph walked for cycles with a line-based regex, so quoted includes only and #if guards are ignored.
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

        // Returns false if the file could not be opened.
        bool ScanHeader(const std::string& AbsPath, std::vector<std::pair<std::string, uint32_t>>& OutIncludes) const;

        // Tries IncluderDir/Text first, then each include search dir, and returns empty when nothing resolves.
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
