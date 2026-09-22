#include "WorldSubsystem.h"

#include "Core/Object/Class.h"
#include "Core/Object/ObjectArray.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ScriptClass.h"
#include "Log/Log.h"
#include "World/World.h"

namespace Lumina::WorldSubsystems
{
    namespace
    {
        bool AlreadyPresent(const TVector<TObjectPtr<CWorldSubsystem>>& Subsystems, const CClass* Class)
        {
            for (const TObjectPtr<CWorldSubsystem>& Subsystem : Subsystems)
            {
                if (Subsystem != nullptr && Subsystem->GetClass() == Class)
                {
                    return true;
                }
            }
            return false;
        }
    }

    int32 CreateMissing(CWorld& World, TVector<TObjectPtr<CWorldSubsystem>>& Out)
    {
        CClass* BaseClass = CWorldSubsystem::StaticClass();

        // Collected before anything is built, because creating a CDO or an instance mutates the object array.
        TVector<CClass*> Candidates;
        GObjectArray.ForEachObject([&](CObjectBase* Object, int32)
        {
            if (Object == nullptr || !Object->IsA<CClass>())
            {
                return;
            }

            CClass* Class = static_cast<CClass*>(Object);
            if (Class != BaseClass && Class->IsChildOf(BaseClass) && !AlreadyPresent(Out, Class))
            {
                Candidates.push_back(Class);
            }
        });

        const size_t FirstNew = Out.size();

        for (CClass* Class : Candidates)
        {
            CWorldSubsystem* Defaults = Class->GetDefaultObject<CWorldSubsystem>();
            if (Defaults == nullptr || !Defaults->ShouldCreate())
            {
                continue;
            }

            CWorldSubsystem* Subsystem = NewObject<CWorldSubsystem>(Class);
            if (Subsystem == nullptr)
            {
                LOG_WARN("World subsystem '{}' could not be created.", Class->GetName().ToString().c_str());
                continue;
            }

            Subsystem->SetOwningWorld(&World);
            Out.push_back(Subsystem);
        }

        // Separate passes, so a subsystem's OnInitialize cannot observe a half built list.
        for (size_t Index = FirstNew; Index < Out.size(); ++Index)
        {
            if (Out[Index] != nullptr)
            {
                Out[Index]->OnInitialize();
            }
        }

        for (size_t Index = FirstNew; Index < Out.size(); ++Index)
        {
            if (Out[Index] != nullptr)
            {
                Out[Index]->OnWorldReady();
            }
        }

        return (int32)(Out.size() - FirstNew);
    }

    void DropScripted(TVector<TObjectPtr<CWorldSubsystem>>& Subsystems)
    {
        // Back to front, so a subsystem created later can still reach an earlier one while it tears down.
        for (size_t Index = Subsystems.size(); Index > 0; --Index)
        {
            CWorldSubsystem* Subsystem = Subsystems[Index - 1].Get();
            if (Subsystem == nullptr || Cast<CScriptClass>(Subsystem->GetClass()) == nullptr)
            {
                continue;
            }

            Subsystem->OnTeardown();
            Subsystem->SetOwningWorld(nullptr);
            Subsystems.erase(Subsystems.begin() + (int64)(Index - 1));
        }
    }

    void Update(TVector<TObjectPtr<CWorldSubsystem>>& Subsystems, float DeltaTime)
    {
        for (TObjectPtr<CWorldSubsystem>& Subsystem : Subsystems)
        {
            if (Subsystem != nullptr)
            {
                Subsystem->OnUpdate(DeltaTime);
            }
        }
    }

    void DestroyAll(TVector<TObjectPtr<CWorldSubsystem>>& Subsystems)
    {
        for (size_t Index = Subsystems.size(); Index > 0; --Index)
        {
            if (CWorldSubsystem* Subsystem = Subsystems[Index - 1].Get())
            {
                Subsystem->OnTeardown();
                Subsystem->SetOwningWorld(nullptr);
            }
        }

        // The handles are the only strong reference the world holds, matching how a script pool is dropped.
        Subsystems.clear();
    }

    CWorldSubsystem* Find(const TVector<TObjectPtr<CWorldSubsystem>>& Subsystems, const CClass* Class)
    {
        if (Class == nullptr)
        {
            return nullptr;
        }

        for (const TObjectPtr<CWorldSubsystem>& Subsystem : Subsystems)
        {
            if (Subsystem != nullptr && Subsystem->GetClass()->IsChildOf(Class))
            {
                return Subsystem.Get();
            }
        }

        return nullptr;
    }
}
