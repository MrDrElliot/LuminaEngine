#pragma once

#include "Containers/HashTable.h"
#include "Containers/Span.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "ScriptExports.h"

namespace Lumina
{
    class CObject;
}

namespace Lumina::Scripting
{
    /** What a managed type becomes on the native side. */
    enum class EManagedTypeKind : uint8
    {
        None = 0,
        // A [Reflect] C# class, minted as a CScriptClass under its native or C# parent.
        ScriptableClass,
        // A [Reflect] C# struct, minted as a CScriptStruct, under the native struct it extends when it names one.
        ScriptStruct,
        // A [Reflect] C# enum, minted as a CScriptEnum.
        Enum,
    };

    // One managed type a generation publishes, with everything needed to build it, read in one crossing.
    struct FManagedTypeDefinition
    {
        EManagedTypeKind    Kind = EManagedTypeKind::None;
        FName               TypeName;

        /** Empty when the type declares no [Property] members; bHasSchema says which. */
        FScriptExportSchema Schema;
        bool                bHasSchema = false;

        // The native type minted under, or the C# parent's name when that class is minted as well.
        FString             NativeBaseName;
        bool                bAbstract = false;
        TVector<FString>    OverriddenEvents;

        // The type's REFLECT specifiers from its C# [Reflect], as key and value.
        TVector<std::pair<FString, FString>> Meta;

        // Enum kind, its storage width and its members.
        EPropertyTypeFlags          EnumUnderlying = EPropertyTypeFlags::Int32;
        TVector<FScriptEnumEntry>   EnumEntries;
    };

    // One [Alias] on a script class, the name it used to have and the name it has now.
    struct FManagedTypeAlias
    {
        FString OldName;
        FString NewName;
    };

    // One stage of rebuilding the engine's view of the managed types after a script load, testable without a runtime.
    class IManagedTypeCompiler
    {
    public:

        virtual ~IManagedTypeCompiler() = default;

        /** Names the stage in logs and in the ordering test. */
        virtual const char* GetName() const = 0;

        // Tears down what this stage holds in the collectible context before it unloads, in reverse registration order.
        virtual void PreUnload() {}

        // The load failed with the previous generation still live, so put back what PreUnload tore down, in registration order.
        virtual void UnloadAborted() {}

        // Rebuilds this stage from the generation that just loaded, taking the kinds of definition it owns.
        virtual void Compile(TSpan<const FManagedTypeDefinition> Definitions, TSpan<const FManagedTypeAlias> Aliases) = 0;
    };

    // Everything one script reload changed, handed to every listener once the new generation is live.
    struct FScriptReload
    {
        int32                                   Generation = 0;
        TSpan<const FManagedTypeDefinition>     Types;

        // Originals to their replacements, classes included. An original may already be freed, so it is only ever compared.
        const THashMap<CObject*, CObject*>*     Replaced = nullptr;

        NODISCARD CObject* FindReplacement(const void* Original) const
        {
            if (Replaced == nullptr)
            {
                return nullptr;
            }
            const auto Found = Replaced->Find(static_cast<CObject*>(const_cast<void*>(Original)));
            return Found != nullptr ? *Found : nullptr;
        }
    };

    // Derive and every script reload calls OnScriptsReloaded, registered for exactly the listener's lifetime.
    class RUNTIME_API IScriptReloadListener
    {
    public:

        IScriptReloadListener();
        virtual ~IScriptReloadListener();

        IScriptReloadListener(const IScriptReloadListener&) = delete;
        IScriptReloadListener& operator=(const IScriptReloadListener&) = delete;
        IScriptReloadListener(IScriptReloadListener&&) = delete;
        IScriptReloadListener& operator=(IScriptReloadListener&&) = delete;

        // On the game thread, after every stage compiled and every entity script heard OnReloaded.
        virtual void OnScriptsReloaded(const FScriptReload& Reload) = 0;
    };

    /** Installs the engine's own stages, in the order they must run. Idempotent. */
    RUNTIME_API void RegisterBuiltInManagedTypeStages();

    // The stages in registration order, which is the order they compile in and the reverse of the order they unload in.
    class RUNTIME_API FManagedTypeRegistry
    {
    public:

        static FManagedTypeRegistry& Get();

        /** Appends a stage. Not owned: a stage is a process-lifetime singleton. */
        void Register(IManagedTypeCompiler* Compiler);

        /** Runs every stage's PreUnload, in reverse order, before the load context goes away. */
        void PreUnloadAll();

        /** Runs every stage's UnloadAborted, in order, after a load that failed. */
        void UnloadAbortedAll();

        /** Runs every stage, in order, over one generation's definitions. */
        void CompileAll(TSpan<const FManagedTypeDefinition> Definitions, TSpan<const FManagedTypeAlias> Aliases = {});

        NODISCARD TSpan<IManagedTypeCompiler* const> GetStages() const { return Stages; }

        void AddListener(IScriptReloadListener* Listener);
        void RemoveListener(IScriptReloadListener* Listener);

        // A stage that moved objects onto replacement classes records them here for the coming broadcast.
        void NoteReplaced(const THashMap<CObject*, CObject*>& Replaced);

        // Tells every listener the generation is live, handing over what the stages replaced on the way.
        void BroadcastReload(int32 Generation, TSpan<const FManagedTypeDefinition> Types);

        /** Test seam: drops every registered stage. */
        void ResetForTesting() { Stages.Reset(); }

    private:

        TVector<IManagedTypeCompiler*> Stages;
        TVector<IScriptReloadListener*> Listeners;
        THashMap<CObject*, CObject*>    PendingReplaced;
    };
}
