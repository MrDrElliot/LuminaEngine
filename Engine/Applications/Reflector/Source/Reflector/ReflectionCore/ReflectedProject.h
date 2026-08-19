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
        FReflectedProject(const FReflectedProject&) = delete;

        FReflectedProject(FReflectedWorkspace* InWorkspace);
        
        std::string                                                       Name;
        std::string                                                       Path;
        // Override destination for this project's generated C# bindings (.generated.cs). Empty = the default
        // Intermediates/CSharpBindings/<Name> (engine modules → LuminaSharp.dll). A plugin/game module sets
        // this to its <root>/Scripts/Generated so the per-plugin script gather compiles the bindings into the
        // plugin's OWN assembly, not LuminaSharp.dll.
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

        // Parsed for type discovery only -- no code is generated for it, and its output directories are
        // never swept. A game or plugin workspace pulls the engine's modules in this way so that its own
        // types can name engine types: a base class, or a property typed as an engine struct, needs the
        // engine type in the database to emit a SuperStruct and a cross-module declaration. Without it
        // the base is silently dropped and the property reference fails to link.
        bool                                                                bReferenceOnly = false;

        FReflectedWorkspace*                                                Workspace;
        std::unordered_map<FStringHash, std::unique_ptr<FReflectedHeader>>   Headers;
        std::vector<std::string>                                        IncludeDirs;

        /// Real compile environment from the build system, so the parse matches what the compiler sees.
        std::vector<std::string>                                        Definitions;
        std::vector<std::string>                                        ForceIncludes;
    };
}
