#include "RuntimePCH.h"
#include "Networking/NetRealm.h"
#include "EntitySystem.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectIteration.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ScriptClass.h"
#include "Log/Log.h"
#include "World/ECS/EventQueue.h"
#include "World/Entity/Components/Component.h"
#include "World/World.h"

namespace Lumina
{
    // LuminaSharp's SSystemContext partial reads the world at offset zero.
    static_assert(offsetof(SSystemContext, World) == 0);

    void CEntitySystem::RequireUpdate(EUpdateStage Stage)
    {
        if (Stage < EUpdateStage::Max)
        {
            StageMask |= (uint8)(1u << (uint32)Stage);
        }
    }

    void CEntitySystem::RunsAfterSystem(FName System)
    {
        if (!Algo::Contains(After, System))
        {
            After.push_back(System);
        }
    }

    void CEntitySystem::RunsBeforeSystem(FName System)
    {
        if (!Algo::Contains(Before, System))
        {
            Before.push_back(System);
        }
    }

    void CEntitySystem::DeclareWrite(FName Component)
    {
        if (const FComponentOps* Ops = FindComponentOps(Component.ToString().View()))
        {
            Access.Writes.push_back((uint32)Ops->TypeId);
            Access.Pools.push_back(Ops->GetTypeID());
            Access.bDeclaresComponents = true;
        }
        else if (const ECS::FComponentTypeID Event = ECS::FindEventType(Component); Event != ECS::InvalidComponentTypeID)
        {
            Access.Writes.push_back((uint32)Event);
        }
        else
        {
            LOG_WARN("Entity system '{}' declares a write of unknown component or event '{}'.", GetClass()->GetName(), Component);
        }
    }

    void CEntitySystem::DeclareRead(FName Component)
    {
        if (const FComponentOps* Ops = FindComponentOps(Component.ToString().View()))
        {
            Access.Reads.push_back((uint32)Ops->TypeId);
            Access.Pools.push_back(Ops->GetTypeID());
            Access.bDeclaresComponents = true;
        }
        else if (const ECS::FComponentTypeID Event = ECS::FindEventType(Component); Event != ECS::InvalidComponentTypeID)
        {
            Access.Reads.push_back((uint32)Event);
        }
        else
        {
            LOG_WARN("Entity system '{}' declares a read of unknown component or event '{}'.", GetClass()->GetName(), Component);
        }
    }

    void CEntitySystem::ConfigureSystem()
    {
        StageMask = 0;
        After.clear();
        Before.clear();
        Access = FSystemAccess();

        Configure();

        // Declaring no components means the system is not honest about what it touches, so it runs alone, its events kept for interest.
        if (!Access.bDeclaresComponents)
        {
            Access.bExclusive = true;
        }
    }

    namespace EntitySystems
    {
        void ForEachSystemClass(TFunctionRef<void(CClass*)> Visitor)
        {
            CClass* BaseClass = CEntitySystem::StaticClass();

            // Collected before anything is visited, because creating a CDO mutates the object array.
            TVector<CClass*> Classes;
            ForEachObject([&](CObjectBase* Object, int32)
            {
                if (Object == nullptr || !Object->IsA<CClass>())
                {
                    return;
                }

                CClass* Class = static_cast<CClass*>(Object);
                if (Class != BaseClass && Class->IsChildOf(BaseClass) && Class->CanInstantiate())
                {
                    Classes.push_back(Class);
                }
            });

            for (CClass* Class : Classes)
            {
                Visitor(Class);
            }
        }

        namespace
        {
            // Exact rather than IsChildOf, so a system deriving from another still gets its own instance.
            bool AlreadyPresent(const TVector<TStrongObjectPtr<CEntitySystem>>& Systems, const CClass* Class)
            {
                for (const TStrongObjectPtr<CEntitySystem>& System : Systems)
                {
                    if (System != nullptr && System->GetClass() == Class)
                    {
                        return true;
                    }
                }
                return false;
            }
        }

        int32 CreateMissing(CWorld& World, const THashSet<FName>& Disabled, TVector<TStrongObjectPtr<CEntitySystem>>& Out)
        {
            const size_t FirstNew = Out.size();

            ForEachSystemClass([&](CClass* Class)
            {
                if (Disabled.count(Class->GetName()) != 0 || AlreadyPresent(Out, Class))
                {
                    return;
                }

                if (!NetRealm::Allows(Class, &World))
                {
                    return;
                }

                CEntitySystem* Defaults = Class->GetDefaultObject<CEntitySystem>();
                if (Defaults == nullptr || !Defaults->ShouldCreate())
                {
                    return;
                }

                CEntitySystem* System = NewObject<CEntitySystem>(Class);
                if (System == nullptr)
                {
                    LOG_WARN("Entity system '{}' could not be created.", Class->GetName());
                    return;
                }

                System->SetOwningWorld(&World);
                System->ConfigureSystem();
                Out.push_back(System);
            });

            return (int32)(Out.size() - FirstNew);
        }

        namespace
        {
            void DropWhere(const SSystemContext& Context, TVector<TStrongObjectPtr<CEntitySystem>>& Systems, TFunctionRef<bool(const CEntitySystem&)> Predicate)
            {
                // Back to front, so a system created later can still reach an earlier one while it tears down.
                for (size_t Index = Systems.size(); Index > 0; --Index)
                {
                    CEntitySystem* System = Systems[Index - 1].Get();
                    if (System == nullptr || !Predicate(*System))
                    {
                        continue;
                    }

                    // Paired with OnStartup, so a system dropped before the world started never sees one.
                    if (System->HasStarted())
                    {
                        System->OnTeardown(Context);
                    }
                    System->SetOwningWorld(nullptr);
                    Systems.erase(Systems.begin() + (int64)(Index - 1));
                }
            }
        }

        void DropScripted(const SSystemContext& Context, TVector<TStrongObjectPtr<CEntitySystem>>& Systems)
        {
            DropWhere(Context, Systems, [](const CEntitySystem& System)
            {
                return Cast<CScriptClass>(System.GetClass()) != nullptr;
            });
        }

        void DropOutsideRealm(const SSystemContext& Context, CWorld& World, TVector<TStrongObjectPtr<CEntitySystem>>& Systems)
        {
            DropWhere(Context, Systems, [&World](const CEntitySystem& System)
            {
                return !NetRealm::Allows(System.GetClass(), &World);
            });
        }

        void DropDisabled(const SSystemContext& Context, const THashSet<FName>& Disabled, TVector<TStrongObjectPtr<CEntitySystem>>& Systems)
        {
            DropWhere(Context, Systems, [&Disabled](const CEntitySystem& System)
            {
                return Disabled.count(System.GetClass()->GetName()) != 0;
            });
        }

        void StartupPending(const SSystemContext& Context, TVector<TStrongObjectPtr<CEntitySystem>>& Systems)
        {
            for (TStrongObjectPtr<CEntitySystem>& System : Systems)
            {
                if (System != nullptr && !System->HasStarted())
                {
                    System->MarkStarted();
                    System->OnStartup(Context);
                }
            }
        }

        CEntitySystem* Find(const TVector<TStrongObjectPtr<CEntitySystem>>& Systems, const CClass* Class)
        {
            if (Class == nullptr)
            {
                return nullptr;
            }

            for (const TStrongObjectPtr<CEntitySystem>& System : Systems)
            {
                if (System != nullptr && System->GetClass()->IsChildOf(Class))
                {
                    return System.Get();
                }
            }

            return nullptr;
        }

        void DestroyAll(const SSystemContext& Context, TVector<TStrongObjectPtr<CEntitySystem>>& Systems)
        {
            // Back to front, so a system created later can still reach an earlier one while it tears down.
            for (size_t Index = Systems.size(); Index > 0; --Index)
            {
                if (CEntitySystem* System = Systems[Index - 1].Get())
                {
                    if (System->HasStarted())
                    {
                        System->OnTeardown(Context);
                    }
                    System->SetOwningWorld(nullptr);
                }
            }

            Systems.clear();
        }
    }
}
