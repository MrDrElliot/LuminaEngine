#include "RuntimePCH.h"
#include "EntitySystem.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ScriptClass.h"
#include "Log/Log.h"
#include "World/Entity/Components/Component.h"
#include "World/World.h"

namespace Lumina
{
    const FSystemContext& CEntitySystem::GetContext() const
    {
        return OwningWorld->GetSystemContext();
    }

    void CEntitySystem::RequireUpdate(EUpdateStage Stage, int32 Priority)
    {
        if (Stage < EUpdateStage::Max)
        {
            const int32 Clamped = Priority < 0 ? 0 : (Priority > 255 ? 255 : Priority);
            Priorities.SetStagePriority(Stage, (EUpdatePriority)Clamped);
        }
    }

    void CEntitySystem::DeclareWrite(FName Component)
    {
        if (const FComponentOps* Ops = FindComponentOps(Component.ToString().View()))
        {
            Access.Writes.push_back((uint32)Ops->TypeId);
            Access.PoolAssurers.push_back(Ops->Assure);
        }
        else
        {
            LOG_WARN("Entity system '{}' declares a write of unknown component '{}'.", GetClass()->GetName(), Component);
        }
    }

    void CEntitySystem::DeclareRead(FName Component)
    {
        if (const FComponentOps* Ops = FindComponentOps(Component.ToString().View()))
        {
            Access.Reads.push_back((uint32)Ops->TypeId);
            Access.PoolAssurers.push_back(Ops->Assure);
        }
        else
        {
            LOG_WARN("Entity system '{}' declares a read of unknown component '{}'.", GetClass()->GetName(), Component);
        }
    }

    void CEntitySystem::ConfigureSystem()
    {
        Priorities.Reset();
        Access = FSystemAccess();

        Configure();

        // Declaring nothing means the system is not honest about what it touches, so it runs alone.
        if (Access.Writes.empty() && Access.Reads.empty())
        {
            Access = FSystemAccess::Exclusive();
        }
    }

    namespace EntitySystems
    {
        void ForEachSystemClass(TFunctionRef<void(CClass*)> Visitor)
        {
            CClass* BaseClass = CEntitySystem::StaticClass();

            // Collected before anything is visited, because creating a CDO mutates the object array.
            TVector<CClass*> Classes;
            GObjectArray.ForEachObject([&](CObjectBase* Object, int32)
            {
                if (Object == nullptr || !Object->IsA<CClass>())
                {
                    return;
                }

                CClass* Class = static_cast<CClass*>(Object);
                if (Class != BaseClass && Class->IsChildOf(BaseClass))
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
            bool AlreadyPresent(const TVector<TObjectPtr<CEntitySystem>>& Systems, const CClass* Class)
            {
                for (const TObjectPtr<CEntitySystem>& System : Systems)
                {
                    if (System != nullptr && System->GetClass() == Class)
                    {
                        return true;
                    }
                }
                return false;
            }
        }

        int32 CreateMissing(CWorld& World, const THashSet<FName>& Disabled, TVector<TObjectPtr<CEntitySystem>>& Out)
        {
            const size_t FirstNew = Out.size();

            ForEachSystemClass([&](CClass* Class)
            {
                if (Disabled.count(Class->GetName()) != 0 || AlreadyPresent(Out, Class))
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
            void DropWhere(TVector<TObjectPtr<CEntitySystem>>& Systems, TFunctionRef<bool(const CEntitySystem&)> Predicate)
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
                        System->OnTeardown();
                    }
                    System->SetOwningWorld(nullptr);
                    Systems.erase(Systems.begin() + (int64)(Index - 1));
                }
            }
        }

        void DropScripted(TVector<TObjectPtr<CEntitySystem>>& Systems)
        {
            DropWhere(Systems, [](const CEntitySystem& System)
            {
                return Cast<CScriptClass>(System.GetClass()) != nullptr;
            });
        }

        void DropDisabled(const THashSet<FName>& Disabled, TVector<TObjectPtr<CEntitySystem>>& Systems)
        {
            DropWhere(Systems, [&Disabled](const CEntitySystem& System)
            {
                return Disabled.count(System.GetClass()->GetName()) != 0;
            });
        }

        void StartupPending(TVector<TObjectPtr<CEntitySystem>>& Systems)
        {
            for (TObjectPtr<CEntitySystem>& System : Systems)
            {
                if (System != nullptr && !System->HasStarted())
                {
                    System->MarkStarted();
                    System->OnStartup();
                }
            }
        }

        CEntitySystem* Find(const TVector<TObjectPtr<CEntitySystem>>& Systems, const CClass* Class)
        {
            if (Class == nullptr)
            {
                return nullptr;
            }

            for (const TObjectPtr<CEntitySystem>& System : Systems)
            {
                if (System != nullptr && System->GetClass()->IsChildOf(Class))
                {
                    return System.Get();
                }
            }

            return nullptr;
        }

        void DestroyAll(TVector<TObjectPtr<CEntitySystem>>& Systems)
        {
            // Back to front, so a system created later can still reach an earlier one while it tears down.
            for (size_t Index = Systems.size(); Index > 0; --Index)
            {
                if (CEntitySystem* System = Systems[Index - 1].Get())
                {
                    if (System->HasStarted())
                    {
                        System->OnTeardown();
                    }
                    System->SetOwningWorld(nullptr);
                }
            }

            Systems.clear();
        }
    }
}
