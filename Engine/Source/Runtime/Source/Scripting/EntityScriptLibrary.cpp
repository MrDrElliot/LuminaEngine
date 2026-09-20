#include "RuntimePCH.h"

#include "EntityScriptLibrary.h"

#include "Scripting/EntityScript.h"
#include "World/World.h"

namespace Lumina
{
    CEntityScript* CEntityScriptLibrary::AddScript(CWorld* World, ECS::FEntity Entity,
        TSubclassOf<CEntityScript> ScriptClass)
    {
        if (World == nullptr || !ScriptClass.IsValid())
        {
            return nullptr;
        }
        return EntityScripts::Attach(ECS::GetWorldRegistry(*World), Entity, ScriptClass.Get());
    }

    CEntityScript* CEntityScriptLibrary::FindScript(CWorld* World, ECS::FEntity Entity,
        TSubclassOf<CEntityScript> ScriptClass)
    {
        if (World == nullptr || !ScriptClass.IsValid())
        {
            return nullptr;
        }
        return EntityScripts::Find(ECS::GetWorldRegistry(*World), Entity, ScriptClass.Get());
    }

    void CEntityScriptLibrary::FindScripts(CWorld* World, ECS::FEntity Entity,
        TSubclassOf<CEntityScript> ScriptClass, TVector<TObjectPtr<CEntityScript>>& OutScripts)
    {
        if (World == nullptr || !ScriptClass.IsValid())
        {
            return;
        }

        TVector<CEntityScript*> Found;
        EntityScripts::FindAll(ECS::GetWorldRegistry(*World), Entity, ScriptClass.Get(), Found);

        OutScripts.reserve(OutScripts.size() + Found.size());
        for (CEntityScript* Script : Found)
        {
            OutScripts.emplace_back(Script);
        }
    }

    bool CEntityScriptLibrary::RemoveScript(CWorld* World, ECS::FEntity Entity, CEntityScript* Script)
    {
        if (World == nullptr || Script == nullptr)
        {
            return false;
        }
        return EntityScripts::Remove(ECS::GetWorldRegistry(*World), Entity, Script);
    }
}
