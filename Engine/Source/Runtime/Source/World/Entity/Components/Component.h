#pragma once

#include <new>

#include "Core/Engine/Engine.h"
#include "Core/Engine/EngineMetaContext.h"
#include "Core/Object/Class.h"
#include "Core/Serialization/Archiver.h"
#include "Memory/Memory.h"
#include "Traits/ComponentTraits.h"
#include "World/Entity/Traits.h"

namespace Lumina
{
    // A managed listener bound to a registry signal: the C# UnmanagedCallersOnly thunk + its GCHandle
    // context (a pinned Action<Entity>). Heap-owned by the bridge; the listener's ADDRESS is both the entt
    // connection payload and the disconnect key, so one allocation == one subscription.
    using FManagedSignalThunk = void (*)(void* Ctx, uint32 Entity);
    struct FManagedSignalListener
    {
        FManagedSignalThunk Thunk = nullptr;
        void*               Ctx   = nullptr;
        entt::connection    Connection;   // the live sink connection; release() disconnects (type-erased).
    };

    // entt connects a free function whose first parameter is the payload instance; forward to managed.
    inline void ManagedSignalTrampoline(FManagedSignalListener& Listener, entt::registry&, entt::entity Entity)
    {
        if (Listener.Thunk)
        {
            Listener.Thunk(Listener.Ctx, static_cast<uint32>(Entity));
        }
    }

    // Signal kinds shared with the C# Registry.On* API.
    enum class EComponentSignal : int32 { Construct = 0, Destroy = 1, Update = 2 };

    // Direct per-component-type functions for the C# bridge's registry ops, bypassing entt::meta's
    // type-erased trampoline. Resolved once per type via FindComponentOps, then called directly.
    struct FComponentOps
    {
        void* (*Get)(entt::registry&, entt::entity);     // try_get -> ptr or null
        int32 (*Has)(entt::registry&, entt::entity);     // 0/1
        void* (*Emplace)(entt::registry&, entt::entity); // get-or-emplace -> live ptr (null for tags)
        int32 (*Remove)(entt::registry&, entt::entity);  // 0/1
        void* (*New)();                                  // detached engine-allocated default instance
        void  (*Delete)(void*);                          // free a New() instance
        void* (*EmplaceCopy)(entt::registry&, entt::entity, const void*); // emplace a COPY of *src -> live ptr
        uint64 TypeId;                                   // entt::type_hash<T>::value() -> registry.storage(id) for the View

        // Registry-signal bridge for C# listeners (on_construct/on_destroy/on_update). The listener pointer
        // is the connection payload + disconnect key. Patch fires on_update<T> on demand (no-op for tags).
        void (*ConnectSignal)(entt::registry&, EComponentSignal, FManagedSignalListener*);
        void (*DisconnectSignal)(entt::registry&, EComponentSignal, FManagedSignalListener*);
        void (*Patch)(entt::registry&, entt::entity);
    };

    RUNTIME_API void RegisterComponentOps(FStringView Name, const FComponentOps* Ops);
    RUNTIME_API const FComponentOps* FindComponentOps(FStringView Name);

    namespace Meta
    {
        template<typename T>
        concept EmptyComponent = eastl::is_empty_v<T>;
        
        template<typename T>
        concept NonEmptyComponent = !eastl::is_empty_v<T>;
        
        template<typename TComponent>
        bool HasComponent(entt::registry& Registry, entt::entity Entity)
        {
            return Registry.any_of<TComponent>(Entity);
        }

        template<typename TComponent>
        auto RemoveComponent(entt::registry& Registry, entt::entity Entity)
        {
            return Registry.remove<TComponent>(Entity);
        }

        template<typename TComponent>
        void ClearComponent(entt::registry& Registry)
        {
            Registry.clear<TComponent>();
        }

        template<NonEmptyComponent TComponent>
        TComponent& EmplaceComponent(entt::registry& Registry, entt::entity Entity, const entt::meta_any& Any)
        {
            return Registry.emplace_or_replace<TComponent>(Entity, Any ? Any.cast<const TComponent&>() : TComponent{});
        }
        
        template<EmptyComponent TComponent>
        void EmplaceComponent(entt::registry& Registry, entt::entity Entity, const entt::meta_any&)
        {
            Registry.emplace<TComponent>(Entity);
        }
        
        template<typename TComponent>
        TComponent& PatchComponent(entt::registry& Registry, entt::entity Entity, const entt::meta_any& Any)
        {
            return Registry.patch<TComponent>(Entity, [&](TComponent& Type)
            {
                if (Any)
                {
                    Type = Any.cast<const TComponent&>();
                }
            });
        }

        template<typename TComponent>
        TComponent& GetComponent(entt::registry& Registry, entt::entity Entity)
        {
            return Registry.get<TComponent>(Entity);
        }
        
        template<typename TComponent>
        void Serialize(FArchive& Ar, entt::meta_any& Any)
        {
            CStruct* Struct = TComponent::StaticStruct();
            TComponent& Instance = Any.cast<TComponent&>();
            Struct->SerializeTaggedProperties(Ar, &Instance);
        }
        
        template<typename TComponent>
        CStruct* GetStructType()
        {
            return TComponent::StaticStruct();
        }

        // The direct-call op table for one component type (captureless lambdas -> plain fn ptrs).
        template<typename TComponent>
        const FComponentOps& GetComponentOps()
        {
            static const FComponentOps Ops = {
                +[](entt::registry& R, entt::entity E) -> void*
                {
                    if constexpr (eastl::is_empty_v<TComponent>) { return nullptr; }
                    else { return R.try_get<TComponent>(E); }
                },
                +[](entt::registry& R, entt::entity E) -> int32 { return R.any_of<TComponent>(E) ? 1 : 0; },
                +[](entt::registry& R, entt::entity E) -> void*
                {
                    if constexpr (eastl::is_empty_v<TComponent>)
                    {
                        if (!R.any_of<TComponent>(E)) { R.emplace<TComponent>(E); }
                        return nullptr;
                    }
                    else { return &R.get_or_emplace<TComponent>(E); } // idempotent: never clobbers an existing one
                },
                +[](entt::registry& R, entt::entity E) -> int32 { return R.remove<TComponent>(E) > 0 ? 1 : 0; },
                +[]() -> void*
                {
                    void* Mem = Memory::Malloc(sizeof(TComponent), alignof(TComponent));
                    return new (Mem) TComponent();
                },
                +[](void* Ptr)
                {
                    static_cast<TComponent*>(Ptr)->~TComponent();
                    void* Mem = Ptr;
                    Memory::Free(Mem);
                },
                +[](entt::registry& R, entt::entity E, const void* Src) -> void*
                {
                    // emplace_or_replace from a configured instance: on the ADD path this constructs the
                    // component from *Src and THEN fires on_construct, so hooks see the configured value.
                    if constexpr (eastl::is_empty_v<TComponent>)
                    {
                        if (!R.any_of<TComponent>(E)) { R.emplace<TComponent>(E); }
                        return nullptr;
                    }
                    else
                    {
                        return &R.emplace_or_replace<TComponent>(E, *static_cast<const TComponent*>(Src));
                    }
                },
                (uint64)entt::type_hash<TComponent>::value(),
                +[](entt::registry& R, EComponentSignal Kind, FManagedSignalListener* L)
                {
                    switch (Kind)
                    {
                        case EComponentSignal::Construct: L->Connection = R.on_construct<TComponent>().template connect<&ManagedSignalTrampoline>(*L); break;
                        case EComponentSignal::Destroy:   L->Connection = R.on_destroy<TComponent>()  .template connect<&ManagedSignalTrampoline>(*L); break;
                        case EComponentSignal::Update:    L->Connection = R.on_update<TComponent>()   .template connect<&ManagedSignalTrampoline>(*L); break;
                    }
                },
                +[](entt::registry&, EComponentSignal, FManagedSignalListener* L)
                {
                    // entt::connection is type-erased, so release() disconnects without re-resolving the type.
                    L->Connection.release();
                },
                +[](entt::registry& R, entt::entity E)
                {
                    // patch fires on_update<T>; meaningful only for data components (tags carry no value).
                    if constexpr (!eastl::is_empty_v<TComponent>)
                    {
                        if (R.any_of<TComponent>(E)) { R.patch<TComponent>(E); }
                    }
                },
            };
            return Ops;
        }

        template<typename TComponent>
        void RegisterComponentMeta()
        {
            using namespace entt::literals;
            auto Meta = entt::meta_factory<TComponent>(GetEngineMetaContext())
                .type(TComponent::StaticStruct()->GetName().c_str())
                .traits(ECS::ETraits::Component)
                .template func<&GetStructType<TComponent>>("static_struct"_hs);

            Meta
            .template func<&RemoveComponent<TComponent>>("remove"_hs)
            .template func<&ClearComponent<TComponent>>("clear"_hs)
            .template func<&EmplaceComponent<TComponent>>("emplace"_hs)
            .template func<&HasComponent<TComponent>>("has"_hs);

            // Direct-call op table for the C# bridge (bypasses the meta trampoline above on the hot path).
            RegisterComponentOps(TComponent::StaticStruct()->GetName().c_str(), &GetComponentOps<TComponent>());
            
            if constexpr (!eastl::is_empty_v<TComponent>)
            {
                Meta.template func<&GetComponent<TComponent>>("get"_hs);
                Meta.template func<&PatchComponent<TComponent>>("patch"_hs);
                Meta.template func<&Serialize<TComponent>>("serialize"_hs);
            }
        }
    }
}
