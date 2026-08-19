#pragma once
#include <unordered_map>
#include <queue>
#include "Reflector/ReflectionCore/ReflectionDatabase.h"
#include "Reflector/ReflectionCore/ReflectionMacro.h"

namespace Lumina::Reflection
{
    class FReflectedWorkspace;

    // A REFLECT'd class template. Its instantiations become reflected structs when a property names one.
    struct FReflectedTemplate
    {
        std::string       QualifiedName;
        std::string       Namespace;
        std::string       MacroContents;
        std::string       HeaderPath;
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
        bool TryFindMacroForCursor(const std::string& HeaderID, const CXCursor& Cursor, FReflectionMacro& Macro, bool bConsume = true);

        bool TryFindGeneratedBodyMacro(const std::string& HeaderID, const CXCursor& Cursor, FReflectionMacro& Macro);



        void PushNamespace(const std::string& Namespace);
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
        
        std::unordered_map<FStringHash, FReflectedHeader*>                 AllHeaders;
        std::unordered_map<FStringHash, FReflectedTemplate>                ReflectedTemplates;

        // Canonical spelling of an instantiation a REFLECT'd alias named, to { qualified, display }.
        std::unordered_map<FStringHash, std::pair<std::string, std::string>> AliasedInstantiations;
        std::unordered_map<uint64_t, std::vector<FReflectionMacro>>      ReflectionMacros;
        std::unordered_map<uint64_t, std::queue<FReflectionMacro>>       GeneratedBodyMacros;
        
        std::vector<std::string>                                NamespaceStack;
        std::string                                               CurrentNamespace;

        // Header the walked alias target's PROPERTY macros were recorded under; empty when not walking one.
        std::string                                               AliasTargetMacroHeader;

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
