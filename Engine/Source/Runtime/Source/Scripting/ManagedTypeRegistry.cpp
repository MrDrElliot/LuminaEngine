#include "RuntimePCH.h"
#include "ManagedTypeRegistry.h"

#include "Core/Profiler/Profile.h"
#include "Log/Log.h"

namespace Lumina::Scripting
{
    FManagedTypeRegistry& FManagedTypeRegistry::Get()
    {
        static FManagedTypeRegistry Instance;
        return Instance;
    }

    void FManagedTypeRegistry::Register(IManagedTypeCompiler* Compiler)
    {
        if (Compiler == nullptr)
        {
            return;
        }

        // Idempotent, so installing the built-in set twice leaves one of each rather than running them twice.
        for (const IManagedTypeCompiler* Existing : Stages)
        {
            if (Existing == Compiler)
            {
                return;
            }
        }

        Stages.Add(Compiler);
    }

    void FManagedTypeRegistry::PreUnloadAll()
    {
        LUMINA_PROFILE_SCOPE();

        for (int32 Index = Stages.Num(); Index > 0; --Index)
        {
            Stages[Index - 1]->PreUnload();
        }
    }

    void FManagedTypeRegistry::UnloadAbortedAll()
    {
        LUMINA_PROFILE_SCOPE();

        for (IManagedTypeCompiler* Stage : Stages)
        {
            Stage->UnloadAborted();
        }
    }

    IScriptReloadListener::IScriptReloadListener()
    {
        FManagedTypeRegistry::Get().AddListener(this);
    }

    IScriptReloadListener::~IScriptReloadListener()
    {
        FManagedTypeRegistry::Get().RemoveListener(this);
    }

    void FManagedTypeRegistry::AddListener(IScriptReloadListener* Listener)
    {
        Listeners.Add(Listener);
    }

    void FManagedTypeRegistry::RemoveListener(IScriptReloadListener* Listener)
    {
        Listeners.Remove(Listener);
    }

    void FManagedTypeRegistry::NoteReplaced(const THashMap<CObject*, CObject*>& Replaced)
    {
        for (const auto& [Old, New] : Replaced)
        {
            PendingReplaced.TryAdd(Old, New);
        }
    }

    void FManagedTypeRegistry::BroadcastReload(int32 Generation, TSpan<const FManagedTypeDefinition> Types)
    {
        LUMINA_PROFILE_SCOPE();

        const THashMap<CObject*, CObject*> Replaced = std::exchange(PendingReplaced, {});

        FScriptReload Reload;
        Reload.Generation = Generation;
        Reload.Types = Types;
        Reload.Replaced = &Replaced;

        // A listener may close a tool and take other listeners with it, so each is checked against the live set first.
        const TVector<IScriptReloadListener*> Snapshot = Listeners;
        for (IScriptReloadListener* Listener : Snapshot)
        {
            if (Algo::Find(Listeners.begin(), Listeners.end(), Listener) != Listeners.end())
            {
                Listener->OnScriptsReloaded(Reload);
            }
        }
    }

    void FManagedTypeRegistry::CompileAll(TSpan<const FManagedTypeDefinition> Definitions, TSpan<const FManagedTypeAlias> Aliases)
    {
        LUMINA_PROFILE_SCOPE();

        for (IManagedTypeCompiler* Stage : Stages)
        {
            Stage->Compile(Definitions, Aliases);
        }
    }
}
