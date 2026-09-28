#include "RuntimePCH.h"

#include "EntityScriptLibrary.h"

#include "Core/Object/ScriptClass.h"
#include "Scripting/EntityScript.h"
#include "Scripting/ScriptableObject.h"
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
        CEntityScript* Script = EntityScripts::Attach(ECS::GetWorldRegistry(*World), Entity, ScriptClass.Get());

        // A C# script's instance is otherwise made by its first event, so a script with none came back untyped.
        if (Script != nullptr && Cast<CScriptClass>(Script->GetClass()) != nullptr)
        {
            Scriptable::GetOrCreateInstance(Script);
        }

        return Script;
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
