#pragma once

#include "World/Entity/Components/SceneFolderComponent.h"
#include "World/Subsystems/EngineSubsystems.h"

namespace Lumina
{
    //~ Components.

    template<typename T, typename... TArgs>
    decltype(auto) CWorld::EmplaceComponent(ECS::FEntity Entity, TArgs&&... Args)
    {
        return EntityRegistry.Emplace<T>(Entity, Forward<TArgs>(Args)...);
    }

    template<typename T, typename... TArgs>
    decltype(auto) CWorld::EmplaceOrReplaceComponent(ECS::FEntity Entity, TArgs&&... Args)
    {
        return EntityRegistry.EmplaceOrReplace<T>(Entity, Forward<TArgs>(Args)...);
    }

    template<typename T, typename... TArgs>
    T& CWorld::GetOrEmplaceComponent(ECS::FEntity Entity, TArgs&&... Args)
    {
        return EntityRegistry.GetOrEmplace<T>(Entity, Forward<TArgs>(Args)...);
    }

    template<typename T, typename TFunc>
    T& CWorld::PatchComponent(ECS::FEntity Entity, TFunc&& Func)
    {
        return EntityRegistry.Patch<T>(Entity, Forward<TFunc>(Func));
    }

    template<typename T> requires (!std::is_empty_v<T>)
    auto& CWorld::GetComponent(this auto& Self, ECS::FEntity Entity)
    {
        return Self.EntityRegistry.template Get<T>(Entity);
    }

    template<typename T>
    auto* CWorld::TryGetComponent(this auto& Self, ECS::FEntity Entity)
    {
        return Self.EntityRegistry.template TryGet<T>(Entity);
    }

    template<typename... T>
    bool CWorld::HasComponent(ECS::FEntity Entity) const
    {
        return EntityRegistry.HasAll<T...>(Entity);
    }

    template<typename... T>
    bool CWorld::HasAnyComponent(ECS::FEntity Entity) const
    {
        return EntityRegistry.HasAny<T...>(Entity);
    }

    template<typename... T>
    void CWorld::RemoveComponent(ECS::FEntity Entity)
    {
        (EntityRegistry.Remove<T>(Entity), ...);
    }

    template<typename T>
    void CWorld::ClearComponents()
    {
        EntityRegistry.ClearComponent<T>();
    }

    template<typename T>
    auto& CWorld::ComponentStorage()
    {
        return EntityRegistry.GetStorage<T>();
    }

    template<typename T>
    auto& CWorld::NamedStorage(uint32 Id)
    {
        return EntityRegistry.GetStorage<T>(Id);
    }

    //~ Entity scripts.

    template<typename T>
    T* CWorld::AddEntityScript(ECS::FEntity Entity)
    {
        return static_cast<T*>(AddEntityScript(Entity, T::StaticClass()));
    }

    //~ Views and component signals.

    template<typename... TGet>
    auto CWorld::View()
    {
        return EntityRegistry.View<TGet...>();
    }

    template<typename... TGet, typename... TExcluded>
    auto CWorld::View(ECS::TExclude<TExcluded...> Exclude)
    {
        return EntityRegistry.View<TGet...>(Exclude);
    }

    template<typename T>
    ECS::FComponentSignal& CWorld::OnConstruct()
    {
        return EntityRegistry.GetSignals<T>().OnConstruct;
    }

    template<typename T>
    ECS::FComponentSignal& CWorld::OnDestroy()
    {
        return EntityRegistry.GetSignals<T>().OnDestroy;
    }

    template<typename T>
    ECS::FComponentSignal& CWorld::OnUpdate()
    {
        return EntityRegistry.GetSignals<T>().OnUpdate;
    }

    //~ Singletons.

    template<typename T, typename... TArgs>
    T& CWorld::EmplaceSingleton(TArgs&&... Args)
    {
        return EntityRegistry.Ctx().Emplace<T>(Forward<TArgs>(Args)...);
    }

    template<typename T>
    auto& CWorld::GetSingleton(this auto& Self)
    {
        return Self.EntityRegistry.Ctx().template Get<T>();
    }

    template<typename T>
    T& CWorld::GetOrEmplaceSingleton()
    {
        return EntityRegistry.Ctx().GetOrEmplace<T>();
    }

    template<typename T>
    auto* CWorld::TryGetSingleton(this auto& Self)
    {
        return Self.EntityRegistry.Ctx().template Find<T>();
    }

    template<typename T>
    bool CWorld::HasSingleton() const
    {
        return EntityRegistry.Ctx().Contains<T>();
    }

    template<typename T>
    void CWorld::EraseSingleton()
    {
        EntityRegistry.Ctx().Erase<T>();
    }

    //~ Registry, deferred commands and events.

    auto& CWorld::GetEntityRegistry(this auto& Self)
    {
        return Self.EntityRegistry;
    }

    auto& CWorld::GetCommandBus(this auto& Self)
    {
        return Self.CommandBus;
    }

    auto& CWorld::GetEvents(this auto& Self)
    {
        return Self.EventQueues;
    }

    //~ Subsystems.

    template<typename T>
    T* CWorld::GetSubsystem() const
    {
        return static_cast<T*>(GetSubsystem(T::StaticClass()));
    }

    template<typename T>
    T* CWorld::EnsureSubsystem()
    {
        return static_cast<T*>(EnsureSubsystem(T::StaticClass()));
    }

    //~ Rendering and scene folders.

    auto& CWorld::GetRenderCallbacks(this auto& Self)
    {
        return Self.RenderCallbacks;
    }

    auto* CWorld::FindRenderSettings(this auto& Self)
    {
        return Self.EntityRegistry.IsValid(Self.SingletonEntity)
            ? Self.EntityRegistry.template TryGet<SWorldRenderSettings>(Self.SingletonEntity)
            : nullptr;
    }

    auto* CWorld::FindSceneFolders(this auto& Self)
    {
        return Self.EntityRegistry.IsValid(Self.SingletonEntity)
            ? Self.EntityRegistry.template TryGet<SSceneFolderComponent>(Self.SingletonEntity)
            : nullptr;
    }
}
