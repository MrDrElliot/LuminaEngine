#include "WorldSubsystem.h"
#include "Networking/NetRealm.h"

#include "Containers/FunctionRef.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectIteration.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ScriptClass.h"
#include "Log/Log.h"
#include "World/World.h"

namespace Lumina::WorldSubsystems
{
    namespace
    {
        bool AlreadyPresent(const TVector<TStrongObjectPtr<CWorldSubsystem>>& Subsystems, const CClass* Class)
        {
            for (const TStrongObjectPtr<CWorldSubsystem>& Subsystem : Subsystems)
            {
                if (Subsystem != nullptr && Subsystem->GetClass() == Class)
                {
                    return true;
                }
            }
            return false;
        }

        // Null when the class is abstract, the world's realm excludes it, or it declines the world.
        CWorldSubsystem* Instantiate(CWorld& World, CClass* Class)
        {
            if (!Class->CanInstantiate() || !NetRealm::Allows(Class, &World))
            {
                return nullptr;
            }

            CWorldSubsystem* Defaults = Class->GetDefaultObject<CWorldSubsystem>();
            if (Defaults == nullptr || !Defaults->ShouldCreate())
            {
                return nullptr;
            }

            CWorldSubsystem* Subsystem = NewObject<CWorldSubsystem>(Class);
            if (Subsystem == nullptr)
            {
                LOG_WARN("World subsystem '{}' could not be created.", *Class->GetName().ToString());
                return nullptr;
            }
            Subsystem->SetOwningWorld(&World);
            return Subsystem;
        }

        void DropWhere(TVector<TStrongObjectPtr<CWorldSubsystem>>& Subsystems, TFunctionRef<bool(const CWorldSubsystem&)> Predicate, bool bRemove = true)
        {
            // Back to front, so a subsystem created later can still reach an earlier one while it tears down.
            for (int32 Index = Subsystems.Num(); Index > 0; --Index)
            {
                CWorldSubsystem* Subsystem = Subsystems[Index - 1].Get();
                if (Subsystem == nullptr || !Predicate(*Subsystem))
                {
                    continue;
                }

                Subsystem->OnTeardown();
                Subsystem->SetOwningWorld(nullptr);
                if (bRemove)
                {
                    Subsystems.RemoveAt((int64)(Index - 1));
                }
            }
        }
    }

    CWorldSubsystem* Create(CWorld& World, TVector<TStrongObjectPtr<CWorldSubsystem>>& Out, CClass* Class)
    {
        CWorldSubsystem* Subsystem = Class != nullptr && Class->IsChildOf(CWorldSubsystem::StaticClass()) ? Instantiate(World, Class) : nullptr;
        if (Subsystem != nullptr)
        {
            Out.Add(Subsystem);
            Subsystem->OnInitialize();
            Subsystem->OnWorldReady();
        }
        return Subsystem;
    }

    int32 CreateMissing(CWorld& World, TVector<TStrongObjectPtr<CWorldSubsystem>>& Out)
    {
        CClass* BaseClass = CWorldSubsystem::StaticClass();

        // Collected before anything is built, because creating a CDO or an instance mutates the object array.
        TVector<CClass*> Candidates;
        ForEachObject([&](CObjectBase* Object, int32)
        {
            if (Object == nullptr || !Object->IsA<CClass>())
            {
                return;
            }

            CClass* Class = static_cast<CClass*>(Object);
            if (Class != BaseClass && Class->IsChildOf(BaseClass) && Class->CanInstantiate() && !AlreadyPresent(Out, Class))
            {
                Candidates.Add(Class);
            }
        });

        const int32 FirstNew = Out.Num();

        for (CClass* Class : Candidates)
        {
            if (CWorldSubsystem* Subsystem = Instantiate(World, Class))
            {
                Out.Add(Subsystem);
            }
        }

        // Separate passes, so a subsystem's OnInitialize cannot observe a half built list.
        for (int32 Index = FirstNew; Index < Out.Num(); ++Index)
        {
            if (Out[Index] != nullptr)
            {
                Out[Index]->OnInitialize();
            }
        }

        for (int32 Index = FirstNew; Index < Out.Num(); ++Index)
        {
            if (Out[Index] != nullptr)
            {
                Out[Index]->OnWorldReady();
            }
        }

        return Out.Num() - FirstNew;
    }

    void DropScripted(TVector<TStrongObjectPtr<CWorldSubsystem>>& Subsystems)
    {
        DropWhere(Subsystems, [](const CWorldSubsystem& Subsystem)
        {
            return Cast<CScriptClass>(Subsystem.GetClass()) != nullptr;
        });
    }

    void DropOutsideRealm(CWorld& World, TVector<TStrongObjectPtr<CWorldSubsystem>>& Subsystems)
    {
        DropWhere(Subsystems, [&World](const CWorldSubsystem& Subsystem)
        {
            return !NetRealm::Allows(Subsystem.GetClass(), &World);
        });
    }

    void Update(TVector<TStrongObjectPtr<CWorldSubsystem>>& Subsystems, float DeltaTime)
    {
        for (TStrongObjectPtr<CWorldSubsystem>& Subsystem : Subsystems)
        {
            if (Subsystem != nullptr)
            {
                Subsystem->OnUpdate(DeltaTime);
            }
        }
    }

    void DestroyAll(TVector<TStrongObjectPtr<CWorldSubsystem>>& Subsystems)
    {
        // Kept in the list until every teardown ran, so cached engine subsystem pointers and lookups stay valid throughout.
        DropWhere(Subsystems, [](const CWorldSubsystem&)
        {
            return true;
        }, false);

        Subsystems.Reset();
    }

    CWorldSubsystem* Find(const TVector<TStrongObjectPtr<CWorldSubsystem>>& Subsystems, const CClass* Class)
    {
        if (Class == nullptr)
        {
            return nullptr;
        }

        for (const TStrongObjectPtr<CWorldSubsystem>& Subsystem : Subsystems)
        {
            if (Subsystem != nullptr && Subsystem->GetClass()->IsChildOf(Class))
            {
                return Subsystem.Get();
            }
        }

        return nullptr;
    }
}
