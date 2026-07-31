#pragma once
#include "ReflectedHeader.h"
#include "StringHash.h"

#include "EASTL/hash_map.h"
#include "EASTL/unique_ptr.h"
#include "eastl/vector.h"

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
