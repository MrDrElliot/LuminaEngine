#pragma once
#include "ReflectedHeader.h"
#include "StringHash.h"

#include "EASTL/hash_map.h"
#include "EASTL/unique_ptr.h"
#include "EASTL/vector.h"

namespace Lumina::Reflection
{
    class FReflectedWorkspace;

    class FReflectedProject
    {
    public:
        
        FReflectedProject& operator =(const FReflectedProject&) = delete;
        FReflectedProject(const FReflectedProject&) = delete;

        FReflectedProject(FReflectedWorkspace* InWorkspace);
        
        eastl::string                                                       Name;
        eastl::string                                                       Path;
        // Override destination for this project's generated C# bindings (.generated.cs). Empty = the default
        // Intermediates/CSharpBindings/<Name> (engine modules → LuminaSharp.dll). A plugin/game module sets
        // this to its <root>/Scripts/Generated so the per-plugin script gather compiles the bindings into the
        // plugin's OWN assembly, not LuminaSharp.dll.
        eastl::string                                                       CSharpBindingsDir;

        /// Where this project's generated C++ goes. Empty uses the workspace default under
        /// Intermediates/Reflection/<Project>. The build system sets it so two targets that
        /// share a module do not share one output directory and overwrite each other.
        eastl::string                                                       GeneratedDir;

        /// Precompiled header this project's generated sources must open with, or empty when the
        /// module has none. Supplied by the build system rather than assumed: a PCH is named after
        /// the module that owns it, so generated code cannot know the name, and guessing one that
        /// belongs to a different module only works while that module happens to be on the
        /// include path.
        eastl::string                                                       PrecompiledHeader;

        // Parsed for type discovery only -- no code is generated for it, and its output directories are
        // never swept. A game or plugin workspace pulls the engine's modules in this way so that its own
        // types can name engine types: a base class, or a property typed as an engine struct, needs the
        // engine type in the database to emit a SuperStruct and a cross-module declaration. Without it
        // the base is silently dropped and the property reference fails to link.
        bool                                                                bReferenceOnly = false;

        FReflectedWorkspace*                                                Workspace;
        eastl::hash_map<FStringHash, eastl::unique_ptr<FReflectedHeader>>   Headers;
        eastl::vector<eastl::string>                                        IncludeDirs;
    };
}
