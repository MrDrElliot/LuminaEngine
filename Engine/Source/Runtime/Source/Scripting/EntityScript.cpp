#include "RuntimePCH.h"
#include "EntityScript.h"

#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectIterator.h"
#include "World/World.h"
#include "World/WorldManager.h"
#include "ScriptableObject.h"
#include "DotNet/DotNetHost.h"
#include "ScriptStruct.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "World/Entity/Components/EntityTags.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
    {
        // Cloned, never shared, since one object on two entities carries the first's state into the second.
        CEntityScript* CloneScript(CEntityScript* Source)
        {
            if (Source == nullptr || Source->GetClass() == nullptr)
            {
                return nullptr;
            }

            CObject* Created = NewObject(Source->GetClass(), nullptr, NAME_None, FGuid::New(), OF_Transient);
            CEntityScript* Clone = static_cast<CEntityScript*>(Created);
            if (Clone != nullptr)
            {
                Source->CopyPropertiesTo(Clone);
            }
            return Clone;
        }

        void CloneScripts(const TVector<TObjectPtr<CEntityScript>>& Source, TVector<TObjectPtr<CEntityScript>>& Out)
        {
            Out.clear();
            Out.reserve(Source.size());
            for (const TObjectPtr<CEntityScript>& Held : Source)
            {
                if (CEntityScript* Clone = CloneScript(Held.Get()))
                {
                    Out.push_back(Clone);
                }
            }
        }
    }

    SEntityScriptComponent::SEntityScriptComponent(const SEntityScriptComponent& Other)
    {
        CloneScripts(Other.Scripts, Scripts);
    }

    SEntityScriptComponent& SEntityScriptComponent::operator=(const SEntityScriptComponent& Other)
    {
        if (this != &Other)
        {
            CloneScripts(Other.Scripts, Scripts);
        }
        return *this;
    }

    bool SEntityScriptComponent::Serialize(FArchive& Ar)
    {
        if (Ar.IsWriting())
        {
            int32 Count = 0;
            for (const TObjectPtr<CEntityScript>& Held : Scripts)
            {
                if (Held.Get() != nullptr && Held.Get()->GetClass() != nullptr)
                {
                    ++Count;
                }
            }
            Ar << Count;

            for (const TObjectPtr<CEntityScript>& Held : Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (Script == nullptr || Script->GetClass() == nullptr)
                {
                    continue;
                }
                FName ClassName = Script->GetClass()->GetName();
                Ar << ClassName;
                Script->GetClass()->SerializeTaggedProperties(Ar, Script);
            }
            return true;
        }

        if (Ar.IsReading())
        {
            int32 Count = 0;
            Ar << Count;

            Scripts.clear();
            for (int32 Index = 0; Index < Count; ++Index)
            {
                FName ClassName;
                Ar << ClassName;

                // Resolved through the redirect registry, since an alias is what carries a renamed class across.
                CClass* ScriptClass = FScriptableRegistry::ResolveClass(ClassName);
                if (ScriptClass == nullptr || !ScriptClass->IsChildOf(CEntityScript::StaticClass()))
                {
                    LOG_WARN("SEntityScriptComponent: script class '{}' no longer exists; the rest of this "
                             "component's scripts were dropped.", ClassName.c_str());
                    break;
                }

                CObject* Created = NewObject(ScriptClass, nullptr, NAME_None, FGuid::New(), OF_Transient);
                CEntityScript* Script = static_cast<CEntityScript*>(Created);
                if (Script == nullptr)
                {
                    break;
                }

                ScriptClass->SerializeTaggedProperties(Ar, Script);
                Scripts.push_back(Script);
            }
            return true;
        }

        return true;
    }

    namespace
    {
        // Snapshotting as a strong ref keeps a script detached mid-pass alive until the snapshot dies.

        using FScriptSnapshot = TVector<TObjectPtr<CEntityScript>>;

        void SnapshotScripts(FEntityRegistry& Registry, entt::entity Entity, FScriptSnapshot& Out)
        {
            Out.clear();

            SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return;
            }

            Out.reserve(Component->Scripts.size());
            for (const TObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                if (Held.Get() != nullptr)
                {
                    Out.push_back(Held);
                }
            }
        }

        // Re-resolved per dispatch, since an earlier callback may have removed this script or its entity.
        bool IsStillAttached(FEntityRegistry& Registry, entt::entity Entity, const CEntityScript* Script)
        {
            if (Script == nullptr || !Registry.valid(Entity))
            {
                return false;
            }

            const SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return false;
            }

            for (const TObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                if (Held.Get() == Script)
                {
                    return true;
                }
            }
            return false;
        }

        // A loaded prefab owns a registry and is not a world, so the world sweep alone left its scripts.
        struct FScriptRegistryRef
        {
            CObject*         Owner = nullptr;
            FEntityRegistry* Registry = nullptr;
            bool             bVariantDelta = false;
        };

        void GatherScriptRegistries(TVector<FScriptRegistryRef>& Out)
        {
            if (GWorldManager != nullptr)
            {
                GWorldManager->ForEachWorld([&](CWorld& World)
                {
                    Out.push_back(FScriptRegistryRef{ &World, &ECS::GetWorldRegistry(World), false });
                });
            }

            for (TObjectIterator<CPrefab> It; It; ++It)
            {
                CPrefab* Prefab = *It;
                if (Prefab == nullptr || Prefab->HasAnyFlag(OF_MarkedDestroy) || Prefab->HasAnyFlag(OF_DefaultObject))
                {
                    continue;
                }
                Out.push_back(FScriptRegistryRef{ Prefab, &Prefab->Registry, false });
                Out.push_back(FScriptRegistryRef{ Prefab, &Prefab->VariantDelta, true });
            }
        }

        FEntityRegistry* ResolveScriptRegistry(CObject* Owner, bool bVariantDelta)
        {
            if (CWorld* World = Cast<CWorld>(Owner))
            {
                return &ECS::GetWorldRegistry(*World);
            }
            if (CPrefab* Prefab = Cast<CPrefab>(Owner))
            {
                return bVariantDelta ? &Prefab->VariantDelta : &Prefab->Registry;
            }
            return nullptr;
        }

        // A script attached to a new entity during the pass readies on the next tick like any other.
        void SnapshotScriptedEntities(FEntityRegistry& Registry, TVector<entt::entity>& Out)
        {
            Out.clear();

            auto View = Registry.view<SEntityScriptComponent>(entt::exclude<SDisabledTag, SScriptDisabledTag>);
            Out.reserve(View.size_hint());
            for (entt::entity Entity : View)
            {
                Out.push_back(Entity);
            }
        }
    }

    namespace EntityScripts
    {
        CEntityScript* Attach(FEntityRegistry& Registry, entt::entity Entity, CClass* ScriptClass)
        {
            if (ScriptClass == nullptr || !ScriptClass->IsChildOf(CEntityScript::StaticClass()))
            {
                LOG_WARN("EntityScripts::Attach: '{}' is not a CEntityScript.",
                    ScriptClass ? ScriptClass->GetName().c_str() : "(null)");
                return nullptr;
            }

            CObject* Created = NewObject(ScriptClass, nullptr, NAME_None, FGuid::New(), OF_Transient);
            CEntityScript* Script = static_cast<CEntityScript*>(Created);
            if (Script == nullptr)
            {
                return nullptr;
            }

            // find, not get, since a bare registry in a test or tool has no world and its scripts have none.
            CWorld** WorldPtr = Registry.ctx().find<CWorld*>();
            Script->SetOwner(Entity, WorldPtr != nullptr ? *WorldPtr : nullptr);

            SEntityScriptComponent& Component = Registry.get_or_emplace<SEntityScriptComponent>(Entity);
            Component.Scripts.push_back(Script);

            // By the first tick every sibling script added the same frame exists, so OnReady can reference them.
            Script->OnAttach();
            return Script;
        }

        void Tick(FEntityRegistry& Registry, float DeltaTime)
        {
            // A C++ subclass runs its own override and a C# one runs the generated shim, indistinguishably.
            CWorld** WorldPtr = Registry.ctx().find<CWorld*>();
            CWorld* World = WorldPtr != nullptr ? *WorldPtr : nullptr;

            TVector<entt::entity> Entities;
            SnapshotScriptedEntities(Registry, Entities);

            FScriptSnapshot Scripts;
            for (entt::entity Entity : Entities)
            {
                SnapshotScripts(Registry, Entity, Scripts);

                for (TObjectPtr<CEntityScript>& Held : Scripts)
                {
                    CEntityScript* Script = Held.Get();
                    if (!IsStillAttached(Registry, Entity, Script))
                    {
                        continue;
                    }

                    // The one place entity and registry are both in hand, and it runs before OnReady.
                    if (Script->GetOwningEntity() == entt::null)
                    {
                        Script->SetOwner(Entity, World);
                        Script->OnAttach();

                        // OnAttach is user code; it may have detached this very script.
                        if (!IsStillAttached(Registry, Entity, Script))
                        {
                            continue;
                        }
                    }

                    if (!Script->IsReady())
                    {
                        Script->MarkReady();
                        Script->OnReady();

                        if (!IsStillAttached(Registry, Entity, Script))
                        {
                            continue;
                        }
                    }
                    Script->OnUpdate(DeltaTime);
                }
            }
        }

        void TickFixed(FEntityRegistry& Registry, float FixedDeltaTime)
        {
            TVector<entt::entity> Entities;
            SnapshotScriptedEntities(Registry, Entities);

            FScriptSnapshot Scripts;
            for (entt::entity Entity : Entities)
            {
                SnapshotScripts(Registry, Entity, Scripts);

                for (TObjectPtr<CEntityScript>& Held : Scripts)
                {
                    // Fixed update only runs on a readied script, so none sees a fixed step before its OnReady.
                    CEntityScript* Script = Held.Get();
                    if (Script != nullptr && Script->IsReady() && IsStillAttached(Registry, Entity, Script))
                    {
                        Script->OnFixedUpdate(FixedDeltaTime);
                    }
                }
            }
        }

        CEntityScript* Find(FEntityRegistry& Registry, entt::entity Entity, const CClass* ScriptClass)
        {
            if (ScriptClass == nullptr)
            {
                return nullptr;
            }
            SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return nullptr;
            }
            for (TObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (Script != nullptr && Script->GetClass() != nullptr && Script->GetClass()->IsChildOf(ScriptClass))
                {
                    return Script;
                }
            }
            return nullptr;
        }

        void FindAll(FEntityRegistry& Registry, entt::entity Entity, const CClass* ScriptClass,
            TVector<CEntityScript*>& Out)
        {
            if (ScriptClass == nullptr)
            {
                return;
            }
            SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return;
            }
            for (TObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (Script != nullptr && Script->GetClass() != nullptr && Script->GetClass()->IsChildOf(ScriptClass))
                {
                    Out.push_back(Script);
                }
            }
        }

        bool Remove(FEntityRegistry& Registry, entt::entity Entity, CEntityScript* Script)
        {
            if (Script == nullptr || !IsStillAttached(Registry, Entity, Script))
            {
                return false;
            }
            
            TObjectPtr<CEntityScript> Pinned(Script);

            if (Script->IsAttached())
            {
                Script->OnDetach();
            }
            
            SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return true;   // OnDetach tore the component down; the script is gone either way
            }

            for (auto It = Component->Scripts.begin(); It != Component->Scripts.end(); ++It)
            {
                if (It->Get() == Script)
                {
                    Component->Scripts.erase(It);
                    return true;
                }
            }
            return true;
        }

        void DispatchCollision(FEntityRegistry& Registry, entt::entity Entity,
            ECollisionCallback Callback, const SCollisionEvent& Event)
        {
            FScriptSnapshot Scripts;
            SnapshotScripts(Registry, Entity, Scripts);

            for (TObjectPtr<CEntityScript>& Held : Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (!IsStillAttached(Registry, Entity, Script))
                {
                    continue;
                }
                switch (Callback)
                {
                case ECollisionCallback::ContactBegin:  Script->OnContactBegin(Event);  break;
                case ECollisionCallback::ContactEnd:    Script->OnContactEnd(Event);    break;
                case ECollisionCallback::OverlapBegin:  Script->OnOverlapBegin(Event);  break;
                case ECollisionCallback::OverlapEnd:    Script->OnOverlapEnd(Event);    break;
                }
            }
        }

        void DispatchInput(FEntityRegistry& Registry, entt::entity Entity, const SInputEvent& Event)
        {
            FScriptSnapshot Scripts;
            SnapshotScripts(Registry, Entity, Scripts);

            for (TObjectPtr<CEntityScript>& Held : Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (IsStillAttached(Registry, Entity, Script))
                {
                    Script->OnInput(Event);
                }
            }
        }

        void PollInputBindings(FEntityRegistry& Registry, entt::entity Entity, const FInputActionState* States,
            int32 Count, uint32 Serial, float DeltaTime)
        {
            FScriptSnapshot Scripts;
            SnapshotScripts(Registry, Entity, Scripts);

            for (TObjectPtr<CEntityScript>& Held : Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (IsStillAttached(Registry, Entity, Script))
                {
                    DotNet::PollScriptInput(Script, States, Count, Serial, DeltaTime);
                }
            }
        }

        void DispatchPerception(FEntityRegistry& Registry, entt::entity Perceiver,
            bool bSensed, const SPerceptionEvent& Event)
        {
            FScriptSnapshot Scripts;
            SnapshotScripts(Registry, Perceiver, Scripts);

            for (TObjectPtr<CEntityScript>& Held : Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (!IsStillAttached(Registry, Perceiver, Script))
                {
                    continue;
                }
                bSensed ? Script->OnTargetPerceived(Event) : Script->OnTargetLost(Event);
            }
        }


        int32 Evacuate(const THashSet<CClass*>& Classes, TVector<FEvacuatedScripts>& Out)
        {
            if (Classes.empty())
            {
                return 0;
            }

            TVector<FScriptRegistryRef> Registries;
            GatherScriptRegistries(Registries);

            int32 Evacuated = 0;
            for (const FScriptRegistryRef& Ref : Registries)
            {
                FEntityRegistry& Registry = *Ref.Registry;

                // Serialize does not touch the registry, but clearing Scripts destroys user-reachable objects.
                TVector<entt::entity> Affected;
                auto View = Registry.view<SEntityScriptComponent>();
                for (entt::entity Entity : View)
                {
                    const SEntityScriptComponent& Component = View.get<SEntityScriptComponent>(Entity);
                    for (const TObjectPtr<CEntityScript>& Held : Component.Scripts)
                    {
                        CEntityScript* Script = Held.Get();
                        if (Script != nullptr && Classes.find(Script->GetClass()) != Classes.end())
                        {
                            Affected.push_back(Entity);
                            break;
                        }
                    }
                }

                for (entt::entity Entity : Affected)
                {
                    SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity);
                    if (Component == nullptr)
                    {
                        continue;
                    }

                    FEvacuatedScripts Saved;
                    Saved.Owner         = Ref.Owner;
                    Saved.Entity        = Entity;
                    Saved.bVariantDelta = Ref.bVariantDelta;
                    {
                        FMemoryWriter Writer(Saved.Bytes);
                        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
                        Component->Serialize(Ar);
                    }
                    
                    Component->Scripts.clear();

                    Out.push_back(std::move(Saved));
                    ++Evacuated;
                }
            }

            return Evacuated;
        }

        int32 Restore(const TVector<FEvacuatedScripts>& Saved)
        {
            int32 Restored = 0;
            for (const FEvacuatedScripts& Entry : Saved)
            {
                FEntityRegistry* RegistryPtr = ResolveScriptRegistry(Entry.Owner.Get(), Entry.bVariantDelta);
                if (RegistryPtr == nullptr)
                {
                    continue;   // the world or prefab went away mid-reload
                }
                FEntityRegistry& Registry = *RegistryPtr;
                if (!Registry.valid(Entry.Entity))
                {
                    continue;   // so did the entity
                }

                SEntityScriptComponent& Component = Registry.get_or_emplace<SEntityScriptComponent>(Entry.Entity);
                {
                    FMemoryReader Reader(const_cast<TVector<uint8>&>(Entry.Bytes));
                    FObjectProxyArchiver Ar(Reader, /*bLoadIfFindFails*/ true);
                    Component.Serialize(Ar);
                }

                // Fields marked to skip hot reload asked for the opposite, so they return to their class default.
                for (const TObjectPtr<CEntityScript>& Held : Component.Scripts)
                {
                    Scripting::ResetSkipHotReloadProperties(Held.Get());
                }
                ++Restored;
            }
            return Restored;
        }

        void DetachAll(FEntityRegistry& Registry, entt::entity Entity)
        {
            FScriptSnapshot Scripts;
            SnapshotScripts(Registry, Entity, Scripts);
            if (Scripts.empty())
            {
                return;
            }

            for (TObjectPtr<CEntityScript>& Held : Scripts)
            {
                // For a C# script the call would mint a managed instance purely to tear it down again.
                if (CEntityScript* Script = Held.Get(); Script != nullptr && Script->IsAttached())
                {
                    Script->OnDetach();
                }
            }
            
            if (SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity))
            {
                Component->Scripts.clear();
            }
        }

        void DetachAllInRegistry(FEntityRegistry& Registry)
        {
            // Disabled entities are included, since a disabled script still ran OnAttach and is owed its OnDetach.
            TVector<entt::entity> Entities;
            auto View = Registry.view<SEntityScriptComponent>();
            Entities.reserve(View.size_hint());
            for (entt::entity Entity : View)
            {
                Entities.push_back(Entity);
            }

            for (entt::entity Entity : Entities)
            {
                if (Registry.valid(Entity))
                {
                    DetachAll(Registry, Entity);
                }
            }
        }
    }
}
