#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
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
        FReflectedHeader* Header = nullptr;
    };

    // Every reflection macro seen in one header, indexed by the line its invocation closes on.
    class FMacroPool
    {
    public:

        void Add(FReflectionMacro&& Macro);

        // Highest-Position unconsumed macro closing on Line, or nullptr. Position tiebreaks a shared line.
        FReflectionMacro* FindOnLine(uint32_t Line, int32_t BeforePosition);

    private:

        std::vector<FReflectionMacro>                       Macros;
        std::unordered_map<uint32_t, std::vector<uint32_t>> ByEndLine;
    };

    // GENERATED_BODY() invocations in source order. A vector head beats a queue's per-node allocation.
    class FGeneratedBodyQueue
    {
    public:

        void Push(FReflectionMacro&& Macro) { Macros.push_back(std::move(Macro)); }

        const FReflectionMacro* Pop()
        {
            return Head < Macros.size() ? &Macros[Head++] : nullptr;
        }

    private:

        std::vector<FReflectionMacro> Macros;
        size_t                        Head = 0;
    };

    class FClangParserContext
    {
    public:

        FClangParserContext() = default;
        ~FClangParserContext() = default;
        FClangParserContext(const FClangParserContext&) = delete;
        FClangParserContext(FClangParserContext&&) = delete;
        FClangParserContext& operator = (const FClangParserContext&) = delete;
        FClangParserContext& operator = (FClangParserContext&&) = delete;

        void AddReflectedMacro(FReflectionMacro&& Macro);
        void AddGeneratedBodyMacro(FReflectionMacro&& Macro);

        // bConsume=false leaves the macro in the pool: several aliases can reflect one shared template.
        bool TryFindMacroForCursor(const FReflectedHeader* Header, const CXCursor& Cursor, FReflectionMacro& Macro, bool bConsume = true);

        bool TryFindGeneratedBodyMacro(const FReflectedHeader* Header, const CXCursor& Cursor, FReflectionMacro& Macro);

        // One CXFile per file per translation unit, so a path normalizes once instead of once per cursor.
        FReflectedHeader* ResolveHeaderForFile(CXFile File);

        // The header a cursor was written in, resolved through the CXFile cache above.
        FReflectedHeader* ResolveHeaderForCursor(const CXCursor& Cursor);

        void PushNamespace(std::string_view Namespace);
        void PopNamespace();

    public:

        template<typename T>
        T* GetParentReflectedType();


        FReflectedType*                                             ParentReflectedType = nullptr;
        FReflectedType*                                             LastReflectedType = nullptr;

        FReflectionDatabase                                         ReflectionDatabase;

        FReflectedWorkspace*                                        Workspace = nullptr;
        FReflectedHeader*                                           ReflectedHeader = nullptr;

        std::unordered_map<FStringHash, FReflectedHeader*>          AllHeaders;
        std::unordered_map<FStringHash, FReflectedTemplate>         ReflectedTemplates;

        // Canonical spelling of an instantiation a REFLECT'd alias named, to { qualified, display }.
        std::unordered_map<FStringHash, std::pair<std::string, std::string>> AliasedInstantiations;

        // Header the walked alias target's PROPERTY macros were recorded under, null when it is unreflected.
        const FReflectedHeader*                                     AliasTargetMacroHeader = nullptr;
        // Distinguishes an unreflected alias target from not walking an alias target at all.
        bool                                                        bWalkingAliasTarget = false;

        // Anonymous union members overlap rather than hide state, so they leave the type fully reflected.
        bool                                                        bInAnonymousRecord = false;

        std::string                                                 CurrentNamespace;

        uint32_t                                                    NumHeadersReflected = 0;

    private:

        std::unordered_map<CXFile, FReflectedHeader*>               FileHeaders;
        std::unordered_map<const FReflectedHeader*, FMacroPool>     ReflectionMacros;
        std::unordered_map<const FReflectedHeader*, FGeneratedBodyQueue> GeneratedBodyMacros;

        // Length CurrentNamespace had before each push, so a pop is a resize rather than a rebuild.
        std::vector<size_t>                                         NamespaceLengths;
    };

    template <typename T>
    T* FClangParserContext::GetParentReflectedType()
    {
        return static_cast<T*>(ParentReflectedType);
    }
}
