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

    // Every type-erased operation on one component type, run by the same code over the pool for a native or a C# type.
    struct RUNTIME_API FComponentOps
    {
        // The dense ECS type id, which FRegistry::FindStorage takes.
        uint64 TypeId = ECS::InvalidComponentTypeID;

        NODISCARD ECS::FComponentTypeID GetTypeID() const { return static_cast<ECS::FComponentTypeID>(TypeId); }

        // Mutable access, so a tracked pool stamps the element and treats the pointer as one a holder may cache.
        NODISCARD void* Get(ECS::FRegistry& Registry, ECS::FEntity Entity) const;

        // Neither stamps nor marks the pointer cached, so a caller that writes through it records the write with Registry.MarkChangedRaw.
        NODISCARD void* GetUnmarked(ECS::FRegistry& Registry, ECS::FEntity Entity) const;

        NODISCARD int32 Has(ECS::FRegistry& Registry, ECS::FEntity Entity) const;

        // Get or emplace, never replacing a value already there. Null for a tag.
        void* Emplace(ECS::FRegistry& Registry, ECS::FEntity Entity) const;
        int32 Remove(ECS::FRegistry& Registry, ECS::FEntity Entity) const;

        // A detached default instance outside any pool, released with Delete.
        NODISCARD void* New() const;
        void Delete(void* Value) const;

        // Emplace or replace from Source, so hooks on the add path already see the copied value.
        void* EmplaceCopy(ECS::FRegistry& Registry, ECS::FEntity Entity, const void* Source) const;
        void* EmplaceDefault(ECS::FRegistry& Registry, ECS::FEntity Entity) const;

        // Reads one value into a temporary first, so a corrupt archive cannot leave a live component half written.
        void* EmplaceSerialized(ECS::FRegistry& Registry, ECS::FEntity Entity, FArchive& Ar) const;

        // The listener pointer is both the connection payload and the disconnect key.
        void ConnectSignal(ECS::FRegistry& Registry, EComponentSignal Kind, FManagedSignalListener* Listener) const;
        void DisconnectSignal(ECS::FRegistry& Registry, EComponentSignal Kind, FManagedSignalListener* Listener) const;

        void Patch(ECS::FRegistry& Registry, ECS::FEntity Entity) const;

        NODISCARD CStruct* StaticStruct() const;

        void Assure(ECS::FRegistry& Registry) const;
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
        template<typename TComponent>
        const FComponentOps& GetComponentOps()
        {
            static const FComponentOps Ops{ static_cast<uint64>(ECS::GetComponentTypeID<TComponent>()) };
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
