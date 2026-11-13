#include "RuntimePCH.h"

#include "ManagedTypeRegistry.h"
#include "ScriptTypeRegistry.h"
#include "ScriptableObject.h"
#include "DotNet/DotNetStateTree.h"

namespace Lumina::Scripting
{
    namespace
    {
        // First, so a struct renamed with [Alias] is followed by every stage after, as a renamed class already was.
        class FRedirectStage final : public IManagedTypeCompiler
        {
        public:

            const char* GetName() const override { return "Redirects"; }

            void Compile(TSpan<const FManagedTypeDefinition>, TSpan<const FManagedTypeAlias> Aliases) override
            {
                FScriptableRegistry::SetRedirects(Aliases);
            }
        };

        // Members change in place, so every field already holding one of these enums sees the reload at once.
        class FEnumStage final : public IManagedTypeCompiler
        {
        public:

            const char* GetName() const override { return "Enums"; }

            void Compile(TSpan<const FManagedTypeDefinition> Definitions, TSpan<const FManagedTypeAlias>) override
            {
                FScriptEnumRegistry::Get().Refresh(Definitions);
            }
        };

        // Class objects before any layout, so a struct field can name a C# class minted this same reload.
        class FClassShellStage final : public IManagedTypeCompiler
        {
        public:

            const char* GetName() const override { return "ClassShells"; }

            void Compile(TSpan<const FManagedTypeDefinition> Definitions, TSpan<const FManagedTypeAlias>) override
            {
                FScriptableRegistry::MintNewClasses(Definitions);
            }
        };

        // Classes are reused by name across reloads, and come after the structs since a class holding one by value is laid out around it.
        class FScriptableClassStage final : public IManagedTypeCompiler
        {
        public:

            const char* GetName() const override { return "ScriptableClasses"; }

            void Compile(TSpan<const FManagedTypeDefinition> Definitions, TSpan<const FManagedTypeAlias> Aliases) override
            {
                FScriptableRegistry::RefreshMintedClasses(Definitions, Aliases);
            }
        };

        // After the class shells a struct field may name, and before the class layouts that hold a struct by value.
        class FStructStage final : public IManagedTypeCompiler
        {
        public:

            const char* GetName() const override { return "Structs"; }

            void Compile(TSpan<const FManagedTypeDefinition> Definitions, TSpan<const FManagedTypeAlias>) override
            {
                FScriptStructRegistry::Get().Refresh(Definitions);
            }
        };

    }

    // Registration order is execution order. Idempotent against the registry contents, so it reinstalls after a reset.
    void RegisterBuiltInManagedTypeStages()
    {
        static FRedirectStage        Redirects;
        static FEnumStage            Enums;
        static FClassShellStage      ClassShells;
        static FStructStage      Structs;
        static FScriptableClassStage ScriptableClasses;
        static StateTreeScript::FStateTreeStage StateTrees;

        FManagedTypeRegistry& Registry = FManagedTypeRegistry::Get();
        Registry.Register(&Redirects);
        Registry.Register(&Enums);
        Registry.Register(&ClassShells);
        Registry.Register(&Structs);
        Registry.Register(&ScriptableClasses);
        Registry.Register(&StateTrees);
    }
}
