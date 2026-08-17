#pragma once
#include "EASTL/hash_map.h"
#include "EASTL/queue.h"
#include "Reflector/ReflectionCore/ReflectionDatabase.h"
#include "Reflector/ReflectionCore/ReflectionMacro.h"

namespace Lumina::Reflection
{
    class FReflectedWorkspace;

    // A REFLECT'd class template. Its instantiations become reflected structs when a property names one.
    struct FReflectedTemplate
    {
        eastl::string       QualifiedName;
        eastl::string       Namespace;
        eastl::string       MacroContents;
        eastl::string       HeaderPath;
        FReflectedHeader*   Header = nullptr;
    };

    class FClangParserContext
    {
    public:

        FClangParserContext()
            : ParentReflectedType(nullptr)
            , LastReflectedType(nullptr)
        {}
        
        ~FClangParserContext() = default;
        FClangParserContext(const FClangParserContext&) = delete;
        FClangParserContext(FClangParserContext&&) = delete;
        FClangParserContext& operator = (const FClangParserContext&) = delete;
        FClangParserContext& operator = (FClangParserContext&&) = delete;
        
        void AddReflectedMacro(FReflectionMacro&& Macro);
        void AddGeneratedBodyMacro(FReflectionMacro&& Macro);
        
        // bConsume=false leaves the macro in the pool: several aliases can reflect one shared template.
        bool TryFindMacroForCursor(const eastl::string& HeaderID, const CXCursor& Cursor, FReflectionMacro& Macro, bool bConsume = true);

        bool TryFindGeneratedBodyMacro(const eastl::string& HeaderID, const CXCursor& Cursor, FReflectionMacro& Macro);



        void PushNamespace(const eastl::string& Namespace);
        void PopNamespace();

    private:

        void RebuildCurrentNamespace();

    public:

        template<typename T>
        T* GetParentReflectedType();

        
        FReflectedType*                                             ParentReflectedType;
        FReflectedType*                                             LastReflectedType;
                                                                    
        FReflectionDatabase                                         ReflectionDatabase;
        
        FReflectedWorkspace*                                        Workspace = nullptr;
        FReflectedHeader*                                           ReflectedHeader = nullptr;
        
        eastl::hash_map<FStringHash, FReflectedHeader*>                 AllHeaders;
        eastl::hash_map<FStringHash, FReflectedTemplate>                ReflectedTemplates;

        // Canonical spelling of an instantiation a REFLECT'd alias named, to { qualified, display }.
        eastl::hash_map<FStringHash, eastl::pair<eastl::string, eastl::string>> AliasedInstantiations;
        eastl::hash_map<uint64_t, eastl::vector<FReflectionMacro>>      ReflectionMacros;
        eastl::hash_map<uint64_t, eastl::queue<FReflectionMacro>>       GeneratedBodyMacros;
        
        eastl::vector<eastl::string>                                NamespaceStack;
        eastl::string                                               CurrentNamespace;

        // Header the walked alias target's PROPERTY macros were recorded under; empty when not walking one.
        eastl::string                                               AliasTargetMacroHeader;

        // Anonymous union members overlap rather than hide state, so they leave the type fully reflected.
        bool                                                        bInAnonymousRecord = false;
                                                                    
        uint32_t                                                    NumHeadersReflected = 0;
    };

    template <typename T>
    T* FClangParserContext::GetParentReflectedType()
    {
        return static_cast<T*>(ParentReflectedType);
    }
}
