#pragma once

#include "World/ECS/Registry.h"
#include "Memory/Construct.h"


#include <new>

#include "Containers/Function.h"
#include "Core/Delegates/ScriptDelegate.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Class.h"
#include "Core/Serialization/Archiver.h"
#include "Memory/Memory.h"
#include "Traits/ComponentTraits.h"
#include "World/Entity/Systems/SystemAccess.h"

namespace Lumina
{
    // A managed listener bound to a registry signal. Heap-owned by the bridge; the listener's address is
    // both the signal payload and the disconnect key, so one allocation is one subscription.
    struct FManagedSignalListener
    {
        // Script binds here, and destroying this listener is what releases those binds.
        TScriptDelegate<uint32> Signal;

        ECS::FSignalConnection        Connection;   // the live sink connection; release() disconnects (type-erased).
    };

    // A signal binds a free function whose first parameter is the payload instance; forward to managed.
    inline void ManagedSignalTrampoline(FManagedSignalListener& Listener, ECS::FRegistry&, ECS::FEntity Entity)
    {
        Listener.Signal.Broadcast(static_cast<uint32>(Entity));
    }

    // Signal kinds shared with the C# Registry.On* API.
    enum class EComponentSignal : int32 { Construct = 0, Destroy = 1, Update = 2 };

    // Every type-erased operation the engine performs on one component type, resolved once via a Find helper.
    struct FComponentOps
    {
        void* (*Get)(ECS::FRegistry&, ECS::FEntity);     // try_get -> ptr or null
        int32 (*Has)(ECS::FRegistry&, ECS::FEntity);     // 0/1
        void* (*Emplace)(ECS::FRegistry&, ECS::FEntity); // get-or-emplace -> live ptr (null for tags)
        int32 (*Remove)(ECS::FRegistry&, ECS::FEntity);  // 0/1
        void* (*New)();                                  // detached engine-allocated default instance
        void  (*Delete)(void*);                          // free a New() instance
        void* (*EmplaceCopy)(ECS::FRegistry&, ECS::FEntity, const void*); // emplace a COPY of *src -> live ptr
        uint64 TypeId;                                   // ECS::GetComponentTypeID<T>() -> registry.FindStorage(id) for the View

        // The listener pointer is both the connection payload and the disconnect key.
        void (*ConnectSignal)(ECS::FRegistry&, EComponentSignal, FManagedSignalListener*);
        void (*DisconnectSignal)(ECS::FRegistry&, EComponentSignal, FManagedSignalListener*);
        void (*Patch)(ECS::FRegistry&, ECS::FEntity);    // fires on_update<T> on demand (no-op for tags)

        CStruct* (*StaticStruct)();
        void* (*EmplaceDefault)(ECS::FRegistry&, ECS::FEntity);   // emplace_or_replace a default instance
        void* (*EmplaceSerialized)(ECS::FRegistry&, ECS::FEntity, FArchive&); // read one, then emplace_or_replace

    };

    // The ops live on the CStruct; reach them with CStruct::GetComponentOps once you have the type.
    RUNTIME_API void RegisterComponentOps(CStruct* Struct, const FComponentOps* Ops);

    // Null when the name is unknown or names a struct that is not a component.
    RUNTIME_API CStruct* FindComponentStruct(FStringView Name);

    // Inverts the id that registry.GetActiveStorages() iteration hands back.
    RUNTIME_API CStruct* FindComponentStructByTypeId(uint64 TypeId);

    RUNTIME_API const FComponentOps* FindComponentOps(FStringView Name);

    // Visits every reflected component type, in unspecified order.
    RUNTIME_API void ForEachComponentStruct(const TFunction<void(CStruct*)>& Function);

    namespace Meta
    {
        // The direct-call op table for one component type (captureless lambdas -> plain fn ptrs).
        template<typename TComponent>
        const FComponentOps& GetComponentOps()
        {
            static const FComponentOps Ops = {
                +[](ECS::FRegistry& R, ECS::FEntity E) -> void*
                {
                    if constexpr (std::is_empty_v<TComponent>) { return nullptr; }
                    else { return R.TryGet<TComponent>(E); }
                },
                +[](ECS::FRegistry& R, ECS::FEntity E) -> int32 { return R.HasAny<TComponent>(E) ? 1 : 0; },
                +[](ECS::FRegistry& R, ECS::FEntity E) -> void*
                {
                    if constexpr (std::is_empty_v<TComponent>)
                    {
                        if (!R.HasAny<TComponent>(E)) { R.Emplace<TComponent>(E); }
                        return nullptr;
                    }
                    else { return &R.GetOrEmplace<TComponent>(E); } // idempotent, never clobbering an existing one
                },
                +[](ECS::FRegistry& R, ECS::FEntity E) -> int32 { return R.Remove<TComponent>(E) ? 1 : 0; },
                +[]() -> void*
                {
                    void* Mem = Memory::Malloc(sizeof(TComponent), alignof(TComponent));
                    return Memory::ConstructAt(static_cast<TComponent*>(Mem));
                },
                +[](void* Ptr)
                {
                    Memory::DestroyAt(static_cast<TComponent*>(Ptr));
                    void* Mem = Ptr;
                    Memory::Free(Mem);
                },
                +[](ECS::FRegistry& R, ECS::FEntity E, const void* Src) -> void*
                {
                    // emplace_or_replace from a configured instance: on the ADD path this constructs the
                    // component from *Src and THEN fires on_construct, so hooks see the configured value.
                    if constexpr (std::is_empty_v<TComponent>)
                    {
                        if (!R.HasAny<TComponent>(E)) { R.Emplace<TComponent>(E); }
                        return nullptr;
                    }
                    else
                    {
                        return &R.EmplaceOrReplace<TComponent>(E, *static_cast<const TComponent*>(Src));
                    }
                },
                (uint64)ECS::GetComponentTypeID<TComponent>(),
                +[](ECS::FRegistry& R, EComponentSignal Kind, FManagedSignalListener* L)
                {
                    switch (Kind)
                    {
                        case EComponentSignal::Construct: L->Connection = R.GetSignals<TComponent>().OnConstruct.template Connect<&ManagedSignalTrampoline>(L); break;
                        case EComponentSignal::Destroy:   L->Connection = R.GetSignals<TComponent>().OnDestroy  .template Connect<&ManagedSignalTrampoline>(L); break;
                        case EComponentSignal::Update:    L->Connection = R.GetSignals<TComponent>().OnUpdate   .template Connect<&ManagedSignalTrampoline>(L); break;
                    }
                },
                +[](ECS::FRegistry&, EComponentSignal, FManagedSignalListener* L)
                {
                    // ECS::FSignalConnection is type-erased, so release() disconnects without re-resolving the type.
                    L->Connection.Release();
                },
                +[](ECS::FRegistry& R, ECS::FEntity E)
                {
                    // patch fires on_update<T>; meaningful only for data components (tags carry no value).
                    if constexpr (!std::is_empty_v<TComponent>)
                    {
                        if (R.HasAny<TComponent>(E)) { R.Patch<TComponent>(E); }
                    }
                },
                +[]() -> CStruct* { return TComponent::StaticStruct(); },
                +[](ECS::FRegistry& R, ECS::FEntity E) -> void*
                {
                    if constexpr (std::is_empty_v<TComponent>)
                    {
                        if (!R.HasAny<TComponent>(E)) { R.Emplace<TComponent>(E); }
                        return nullptr;
                    }
                    else { return &R.EmplaceOrReplace<TComponent>(E, TComponent{}); }
                },
                +[](ECS::FRegistry& R, ECS::FEntity E, FArchive& Ar) -> void*
                {
                    if constexpr (std::is_empty_v<TComponent>)
                    {
                        if (!R.HasAny<TComponent>(E)) { R.Emplace<TComponent>(E); }
                        return nullptr;
                    }
                    else
                    {
                        // Read into a temporary first, so a corrupt archive cannot leave a live component half-written.
                        TComponent Value{};
                        TComponent::StaticStruct()->SerializeTaggedProperties(Ar, &Value);
                        return &R.EmplaceOrReplace<TComponent>(E, Move(Value));
                    }
                },
            };
            return Ops;
        }

#if !defined(LE_SHIPPING)
        // A signal hands the listener the registry and the entity; the type is what the executing system had to declare.
        template<typename TComponent>
        void ValidateComponentStructuralWrite(ECS::FRegistry&, ECS::FEntity)
        {
            ValidateSystemAccess(static_cast<uint32>(ECS::GetComponentTypeID<TComponent>()), true,
                "a Write<> of the component being added or removed");
        }

        template<typename TComponent>
        void ConnectComponentAccessValidator(ECS::FRegistry& Registry)
        {
            Registry.GetSignals<TComponent>().OnConstruct.template Connect<&ValidateComponentStructuralWrite<TComponent>>();
            Registry.GetSignals<TComponent>().OnDestroy.template Connect<&ValidateComponentStructuralWrite<TComponent>>();
        }
#endif

        template<typename TComponent>
        void RegisterComponentMeta()
        {
            RegisterComponentOps(TComponent::StaticStruct(), &GetComponentOps<TComponent>());

#if !defined(LE_SHIPPING)
            RegisterComponentAccessValidator(&ConnectComponentAccessValidator<TComponent>);
#endif
        }
    }
}
