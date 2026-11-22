#include "RuntimePCH.h"

#include "World/World.h"

namespace Lumina
{
    namespace
    {
        template<typename... Ts>
        void FillEngineSubsystems(CWorld& World, TTuple<Ts*...>& Cache)
        {
            ((Cache.template Get<Ts*>() = World.EnsureSubsystem<Ts>()), ...);
        }
    }

    template<typename T>
    T* CWorld::GetCachedSubsystem()
    {
        T*& Slot = EngineSubsystems.template Get<T*>();
        if (Slot == nullptr)
        {
            Slot = EnsureSubsystem<T>();
        }
        return Slot;
    }

    void CWorld::RefreshEngineSubsystems()
    {
        EngineSubsystems = {};
        FillEngineSubsystems(*this, EngineSubsystems);
    }

    CWorldSubsystem* CWorld::EnsureSubsystem(TSubclassOf<CWorldSubsystem> Class)
    {
        if (!Class.IsValid())
        {
            return nullptr;
        }
        if (CWorldSubsystem* Found = WorldSubsystems::Find(Subsystems, Class.Get()))
        {
            return Found;
        }
        return WorldSubsystems::Create(*this, Subsystems, Class.Get());
    }

    CPhysicsSubsystem* CWorld::GetPhysics()
    {
        return GetCachedSubsystem<CPhysicsSubsystem>();
    }

    CInputSubsystem* CWorld::GetInput()
    {
        return GetCachedSubsystem<CInputSubsystem>();
    }

    CTweenSubsystem* CWorld::GetTweens()
    {
        return GetCachedSubsystem<CTweenSubsystem>();
    }

    CTimerSubsystem* CWorld::GetTimers()
    {
        return GetCachedSubsystem<CTimerSubsystem>();
    }

    CMessageSubsystem* CWorld::GetMessageBus()
    {
        return GetCachedSubsystem<CMessageSubsystem>();
    }

    CAnimationSubsystem* CWorld::GetAnimation()
    {
        return GetCachedSubsystem<CAnimationSubsystem>();
    }

    CCameraSubsystem* CWorld::GetCamera()
    {
        return GetCachedSubsystem<CCameraSubsystem>();
    }

    CNavigationSubsystem* CWorld::GetNavigation()
    {
        return GetCachedSubsystem<CNavigationSubsystem>();
    }

    CPerceptionSubsystem* CWorld::GetPerception()
    {
        return GetCachedSubsystem<CPerceptionSubsystem>();
    }

    CSequenceSubsystem* CWorld::GetSequences()
    {
        return GetCachedSubsystem<CSequenceSubsystem>();
    }

    CNetSubsystem* CWorld::GetNet()
    {
        return GetCachedSubsystem<CNetSubsystem>();
    }

    CUISubsystem* CWorld::GetUI()
    {
        return GetCachedSubsystem<CUISubsystem>();
    }

    CDebugSubsystem* CWorld::GetDebug()
    {
        return GetCachedSubsystem<CDebugSubsystem>();
    }

    CSpawnSubsystem* CWorld::GetSpawner()
    {
        return GetCachedSubsystem<CSpawnSubsystem>();
    }

    CEnvironmentSubsystem* CWorld::GetEnvironment()
    {
        return GetCachedSubsystem<CEnvironmentSubsystem>();
    }

    CSaveGameSubsystem* CWorld::GetSaveGames()
    {
        return GetCachedSubsystem<CSaveGameSubsystem>();
    }
}
