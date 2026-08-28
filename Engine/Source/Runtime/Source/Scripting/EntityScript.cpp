#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
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

                // Length-prefixed, so a reader that cannot resolve this class can skip exactly this script.
                const int64 SizePos = Ar.Tell();
                int64 ScriptSize = 0;
                Ar << ScriptSize;

                const int64 DataStart = Ar.Tell();
                Script->GetClass()->SerializeTaggedProperties(Ar, Script);
                const int64 DataEnd = Ar.Tell();

                ScriptSize = DataEnd - DataStart;
                Ar.Seek(SizePos);
                Ar << ScriptSize;
                Ar.Seek(DataEnd);
            }
            return true;
        }

        if (Ar.IsReading())
        {
            int32 Count = 0;
            Ar << Count;

            // Older files have no per-script length, so an unresolvable class there still ends the record.
            const bool bLengthPrefixed =
                Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ENTITY_SCRIPT_LENGTH_PREFIX;

            Scripts.clear();
            for (int32 Index = 0; Index < Count; ++Index)
            {
                FName ClassName;
                Ar << ClassName;

                int64 ScriptSize = 0;
                int64 DataStart  = 0;
                if (bLengthPrefixed)
                {
                    Ar << ScriptSize;
                    DataStart = Ar.Tell();
                }

                // Resolved through the redirect registry, since an alias is what carries a renamed class across.
                CClass* ScriptClass = FScriptableRegistry::ResolveClass(ClassName);
                CEntityScript* Script = nullptr;
                if (ScriptClass != nullptr && ScriptClass->IsChildOf(CEntityScript::StaticClass()))
                {
                    Script = static_cast<CEntityScript*>(
                        NewObject(ScriptClass, nullptr, NAME_None, FGuid::New(), OF_Transient));
                }

                if (Script == nullptr)
                {
                    if (!bLengthPrefixed)
                    {
                        LOG_WARN("SEntityScriptComponent: script class '{}' no longer exists; the rest of this "
                                 "component's scripts were dropped.", ClassName.c_str());
                        break;
                    }

                    LOG_WARN("SEntityScriptComponent: script class '{}' no longer exists; skipping it and "
                             "keeping this entity's other scripts.", ClassName.c_str());
                    Ar.Seek(DataStart + ScriptSize);
                    continue;
                }

                ScriptClass->SerializeTaggedProperties(Ar, Script);

                // Trusted over wherever the property reader stopped, so drift cannot shift the next script.
                if (bLengthPrefixed)
                {
                    Ar.Seek(DataStart + ScriptSize);
                }

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

        // Both tick passes rebuild these every call, so they are parked per thread rather than reallocated.
        struct FTickScratch
        {
            TVector<ECS::FEntity> Entities;
            FScriptSnapshot       Scripts;
        };

        // A nested tick falls back to its own storage rather than walking the outer pass's buffers.
        class FTickScratchGuard
        {
        public:

            FTickScratchGuard()
            {
                if (Depth()++ == 0u)
                {
                    Parked = &ParkedStorage();
                }
            }

            ~FTickScratchGuard() { --Depth(); }

            LE_NO_COPYMOVE(FTickScratchGuard);

            FTickScratch& Get() { return (Parked != nullptr) ? *Parked : Local; }

        private:

            static uint32& Depth()               { static thread_local uint32 Value = 0; return Value; }
            static FTickScratch& ParkedStorage() { static thread_local FTickScratch Storage; return Storage; }

            FTickScratch* Parked = nullptr;
            FTickScratch  Local;
        };

        void SnapshotScripts(ECS::FRegistry& Registry, ECS::FEntity Entity, FScriptSnapshot& Out)
        {
            Out.clear();

            // try_get on a dead entity indexes the sparse set out of bounds rather than returning null.
            if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
            {
                return;
            }

            SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
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

        // Type-uniform, so it is one class-level byte rather than anything stored per script instance.
        EScriptUpdatePhase ScriptPhase(const CEntityScript* Script)
        {
            const CClass* Class = Script != nullptr ? Script->GetClass() : nullptr;
            return Class != nullptr ? static_cast<EScriptUpdatePhase>(Class->ScriptUpdatePhase)
                                    : EScriptUpdatePhase::PrePhysics;
        }

        // Re-resolved per dispatch, since an earlier callback may have removed this script or its entity.
        bool IsStillAttached(ECS::FRegistry& Registry, ECS::FEntity Entity, const CEntityScript* Script)
        {
            if (Script == nullptr || !Registry.IsValid(Entity))
            {
                return false;
            }

            const SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
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
            ECS::FRegistry* Registry = nullptr;
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

        ECS::FRegistry* ResolveScriptRegistry(CObject* Owner, bool bVariantDelta)
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
        void SnapshotScriptedEntities(ECS::FRegistry& Registry, TVector<ECS::FEntity>& Out)
        {
            Out.clear();

            auto View = Registry.View<SEntityScriptComponent>(ECS::TExclude<SDisabledTag, SScriptDisabledTag>{});
            Out.reserve(View.Num());
            for (ECS::FEntity Entity : View)
            {
                Out.push_back(Entity);
            }
        }
    }

    namespace EntityScripts
    {
        CEntityScript* Attach(ECS::FRegistry& Registry, ECS::FEntity Entity, CClass* ScriptClass)
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
            CWorld** WorldPtr = Registry.Ctx().Find<CWorld*>();
            Script->SetOwner(Entity, WorldPtr != nullptr ? *WorldPtr : nullptr);

            SEntityScriptComponent& Component = Registry.GetOrEmplace<SEntityScriptComponent>(Entity);
            Component.Scripts.push_back(Script);

            // By the first tick every sibling script added the same frame exists, so OnReady can reference them.
            Script->OnAttach();
            return Script;
        }

        void Tick(ECS::FRegistry& Registry, float DeltaTime, EScriptUpdatePhase Phase)
        {
            // Attach and OnReady are phase-independent, so a PostPhysics script is built in the same pass.
            const bool bDrainLifecycle = Phase == EScriptUpdatePhase::PrePhysics;

            // A C++ subclass runs its own override and a C# one runs the generated shim, indistinguishably.
            CWorld** WorldPtr = Registry.Ctx().Find<CWorld*>();
            CWorld* World = WorldPtr != nullptr ? *WorldPtr : nullptr;

            FTickScratchGuard    ScratchGuard;
            FTickScratch&        Scratch  = ScratchGuard.Get();
            TVector<ECS::FEntity>& Entities = Scratch.Entities;
            FScriptSnapshot&       Scripts  = Scratch.Scripts;

            SnapshotScriptedEntities(Registry, Entities);

            for (ECS::FEntity Entity : Entities)
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
                    if (bDrainLifecycle && Script->GetOwningEntity() == ECS::NullEntity)
                    {
                        Script->SetOwner(Entity, World);
                        Script->OnAttach();

                        // OnAttach is user code; it may have detached this very script.
                        if (!IsStillAttached(Registry, Entity, Script))
                        {
                            continue;
                        }
                    }

                    if (bDrainLifecycle && !Script->IsReady())
                    {
                        Script->MarkReady();
                        Script->OnReady();

                        if (!IsStillAttached(Registry, Entity, Script))
                        {
                            continue;
                        }
                    }

                    if (Script->IsReady() && ScriptPhase(Script) == Phase)
                    {
                        Script->OnUpdate(DeltaTime);
                    }
                }
            }

            // Parked storage would otherwise hold the last entity's scripts alive until the next tick.
            Scripts.clear();
        }

        void TickFixed(ECS::FRegistry& Registry, float FixedDeltaTime)
        {
            FTickScratchGuard    ScratchGuard;
            FTickScratch&        Scratch  = ScratchGuard.Get();
            TVector<ECS::FEntity>& Entities = Scratch.Entities;
            FScriptSnapshot&       Scripts  = Scratch.Scripts;

            SnapshotScriptedEntities(Registry, Entities);

            for (ECS::FEntity Entity : Entities)
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

            // Parked storage would otherwise hold the last entity's scripts alive until the next tick.
            Scripts.clear();
        }

        CEntityScript* Find(ECS::FRegistry& Registry, ECS::FEntity Entity, const CClass* ScriptClass)
        {
            if (ScriptClass == nullptr)
            {
                return nullptr;
            }
            SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
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

        void FindAll(ECS::FRegistry& Registry, ECS::FEntity Entity, const CClass* ScriptClass,
            TVector<CEntityScript*>& Out)
        {
            if (ScriptClass == nullptr)
            {
                return;
            }
            SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
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

        bool Remove(ECS::FRegistry& Registry, ECS::FEntity Entity, CEntityScript* Script)
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
            
            SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
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

        void DispatchCollision(ECS::FRegistry& Registry, ECS::FEntity Entity,
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

        void DispatchInput(ECS::FRegistry& Registry, ECS::FEntity Entity, const SInputEvent& Event)
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

        void PollInputBindings(ECS::FRegistry& Registry, ECS::FEntity Entity, const FInputActionState* States,
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

        void DispatchPerception(ECS::FRegistry& Registry, ECS::FEntity Perceiver,
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
                ECS::FRegistry& Registry = *Ref.Registry;

                // Serialize does not touch the registry, but clearing Scripts destroys user-reachable objects.
                TVector<ECS::FEntity> Affected;
                auto View = Registry.View<SEntityScriptComponent>();
                for (ECS::FEntity Entity : View)
                {
                    const SEntityScriptComponent& Component = View.Get<SEntityScriptComponent>(Entity);
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

                for (ECS::FEntity Entity : Affected)
                {
                    SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
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
                ECS::FRegistry* RegistryPtr = ResolveScriptRegistry(Entry.Owner.Get(), Entry.bVariantDelta);
                if (RegistryPtr == nullptr)
                {
                    continue;   // the world or prefab went away mid-reload
                }
                ECS::FRegistry& Registry = *RegistryPtr;
                if (!Registry.IsValid(Entry.Entity))
                {
                    continue;   // so did the entity
                }

                SEntityScriptComponent& Component = Registry.GetOrEmplace<SEntityScriptComponent>(Entry.Entity);
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

        void DetachAll(ECS::FRegistry& Registry, ECS::FEntity Entity)
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
            
            if (SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity))
            {
                Component->Scripts.clear();
            }
        }

        void DetachAllInRegistry(ECS::FRegistry& Registry)
        {
            // Disabled entities are included, since a disabled script still ran OnAttach and is owed its OnDetach.
            TVector<ECS::FEntity> Entities;
            auto View = Registry.View<SEntityScriptComponent>();
            Entities.reserve(View.Num());
            for (ECS::FEntity Entity : View)
            {
                Entities.push_back(Entity);
            }

            for (ECS::FEntity Entity : Entities)
            {
                if (Registry.IsValid(Entity))
                {
                    DetachAll(Registry, Entity);
                }
            }
        }
    }
}
