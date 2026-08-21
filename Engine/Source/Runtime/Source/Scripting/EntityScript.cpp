#include "RuntimePCH.h"
#include "EntityScript.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
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
        // Clone, never share: one script object on two entities carries the first's state, and its world, into the second.
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

                // A class that no longer exists (script deleted, or a C# type removed from this generation)
                // drops its instance. There is nothing safe to construct, and the tagged-property block is
                // self-delimiting only within a known layout -- so bail rather than desynchronize the stream.
                // Through the redirect registry, not FindObject: a script class renamed in C# leaves every
                // saved scene (and every hot-reload evacuation buffer) naming the old one, and an `[Alias]`
                // on the new type is what carries them across.
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
        // Every callback below is user code, and user code is allowed to attach or detach scripts -- on this
        // entity or another one -- from inside it. Both are structurally hostile to a plain range-for:
        //
        //   - Attach pushes onto the SAME TVector being iterated, so it reallocates and dangles the loop's
        //     reference and end iterator.
        //   - Attach's get_or_emplace<SEntityScriptComponent> on a DIFFERENT entity grows entt's storage,
        //     which invalidates both the view being iterated and any held component pointer.
        //   - Remove erases from the vector, and dropping the last TObjectPtr destroys the script object
        //     immediately (FCObjectArray::ReleaseStrongRef -> ConditionalDestroy).
        //
        // So nothing dispatches straight off live storage. Snapshotting as TObjectPtr (a STRONG ref) is what
        // makes the third case safe: a script detached mid-pass stays alive until the snapshot dies, and the
        // IsStillAttached guard is what stops it receiving any further callback.

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

        // Re-resolved per dispatch rather than cached: an earlier callback in this same pass may have removed
        // this script, destroyed its entity, or dropped the whole component.
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

        // Entities carrying scripts, captured before any callback runs. Scripts attached to a NEW entity
        // during the pass are not ticked this frame; they have already had their OnAttach from Attach, and
        // they ready on the next tick like every other freshly attached script.
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

            // Resolve the world from the registry's context singleton rather than making every caller pass it.
            // find, not get: a bare registry (a test, a tool) has no world, and a script there simply has none.
            CWorld** WorldPtr = Registry.ctx().find<CWorld*>();
            Script->SetOwner(Entity, WorldPtr != nullptr ? *WorldPtr : nullptr);

            SEntityScriptComponent& Component = Registry.get_or_emplace<SEntityScriptComponent>(Entity);
            Component.Scripts.push_back(Script);

            // Attach immediately, ready on the first tick: by then every sibling script the same frame added
            // exists, so OnReady can reference them.
            Script->OnAttach();
            return Script;
        }

        void Tick(FEntityRegistry& Registry, float DeltaTime)
        {
            // One loop, one virtual call. A C++ subclass runs its own override directly; a C# subclass runs
            // the Reflector-generated shim, which dispatches into managed. Nothing here knows the difference.
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

                    // Adopt a script that arrived without an owner -- deserialization reconstructs the objects
                    // but cannot know their entity (see SEntityScriptComponent::Serialize). This is the one
                    // place the entity and the registry are both in hand, and it runs before OnReady, so a
                    // loaded script sees a valid Entity/World from its very first callback.
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
                    // Fixed update runs only on a script that has readied, so a script cannot see a fixed step
                    // before its OnReady.
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

            Script->OnDetach();
            
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
            if (Classes.empty() || GWorldManager == nullptr)
            {
                return 0;
            }

            int32 Evacuated = 0;
            GWorldManager->ForEachWorld([&](CWorld& World)
            {
                FEntityRegistry& Registry = ECS::GetWorldRegistry(World);

                // Collect first, mutate second. Serialize does not touch the registry, but clearing the
                // component's Scripts destroys CObjects, and a script's destructor is user-reachable code.
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
                    Saved.World  = &World;
                    Saved.Entity = Entity;
                    {
                        FMemoryWriter Writer(Saved.Bytes);
                        FObjectProxyArchiver Ar(Writer, /*bLoadIfFindFails*/ false);
                        Component->Serialize(Ar);
                    }
                    
                    Component->Scripts.clear();

                    Out.push_back(std::move(Saved));
                    ++Evacuated;
                }
            });

            return Evacuated;
        }

        int32 Restore(const TVector<FEvacuatedScripts>& Saved)
        {
            int32 Restored = 0;
            for (const FEvacuatedScripts& Entry : Saved)
            {
                CWorld* World = Entry.World.Get();
                if (World == nullptr)
                {
                    continue;   // the world went away mid-reload
                }
                FEntityRegistry& Registry = ECS::GetWorldRegistry(*World);
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

                // The replay put the authored value back on EVERY property. Fields marked [SkipHotReload]
                // asked for the opposite, so they are returned to their class default afterwards.
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
                if (CEntityScript* Script = Held.Get())
                {
                    Script->OnDetach();
                }
            }
            
            if (SEntityScriptComponent* Component = Registry.try_get<SEntityScriptComponent>(Entity))
            {
                Component->Scripts.clear();
            }
        }
    }
}
