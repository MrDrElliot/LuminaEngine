#pragma once
#include "ReflectedHeader.h"
#include "StringHash.h"

#include <unordered_map>
#include <memory>
#include <vector>

namespace Lumina::Reflection
{
    class FReflectedWorkspace;

    class FReflectedProject
    {
    public:
        
        FReflectedProject& operator =(const FReflectedProject&) = delete;
        ~FReflectedProject() = default;
        FReflectedProject(const FReflectedProject&) = delete;
        FReflectedProject(FReflectedProject&&) = delete;
        FReflectedProject& operator=(FReflectedProject&&) = delete;

        FReflectedProject(FReflectedWorkspace* InWorkspace);
        
        std::string                                                       Name;
        std::string                                                       Path;
        // Plugin and game modules set this so their bindings compile into their own assembly instead of LuminaSharp.dll.
        std::string                                                       CSharpBindingsDir;

        // Routes reflected-TYPE bindings into CSharpBindingsDir too, for a unit that compiles its own assembly.
        bool                                                                bRouteTypeBindings = false;

        /// Where this project's generated C++ goes. Empty uses the workspace default under
        /// Intermediates/Reflection/<Project>. The build system sets it so two targets that
        /// share a module do not share one output directory and overwrite each other.
        std::string                                                       GeneratedDir;

        /// Precompiled header this project's generated sources must open with, or empty when the
        /// module has none. Supplied by the build system rather than assumed: a PCH is named after
        /// the module that owns it, so generated code cannot know the name, and guessing one that
        /// belongs to a different module only works while that module happens to be on the
        /// include path.
        std::string                                                       PrecompiledHeader;

        // Parsed for type discovery only so game types can name engine bases and properties, which otherwise drop or fail to link.
        bool                                                                bReferenceOnly = false;

        FReflectedWorkspace*                                                Workspace;
        std::unordered_map<FStringHash, std::unique_ptr<FReflectedHeader>>   Headers;
        std::vector<std::string>                                        IncludeDirs;

        /// Real compile environment from the build system, so the parse matches what the compiler sees.
        std::vector<std::string>                                        Definitions;
        std::vector<std::string>                                        ForceIncludes;
    };
}
