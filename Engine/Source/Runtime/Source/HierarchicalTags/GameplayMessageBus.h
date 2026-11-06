#pragma once

#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Core/Delegates/Delegate.h"
#include "Core/Object/Class.h"
#include "Core/Threading/Atomic.h"
#include "Core/Threading/Thread.h"
#include "HierarchicalTags/HierarchicalTag.h"
#include "HierarchicalTags/HierarchicalTagRegistry.h"
#include "World/ECS/Entity.h"
#include <type_traits>

namespace Lumina
{
    class CStruct;

    namespace ECS
    {
        class FRegistry;
        class FCommandBus;
    }

    // Values shared with LuminaSharp's HierarchicalTagMatch.
    enum class EHierarchicalTagMatch : uint8
    {
        // The channel and every descendant, so a listener on "Damage" hears "Damage.Fire".
        Partial,
        Exact,
    };

    enum class EGameplayMessageRoute : uint32
    {
        Broadcast,
        // The source, then each ancestor up to the root.
        Up,
        // The source, then its subtree in preorder.
        Down,
        To,
    };

    // How a payload is copied when a send is deferred to the command bus flush.
    enum class EGameplayMessagePayload : uint32
    {
        None,
        // A reflected struct, copied through its CStruct.
        Struct,
        // Trivially copyable bytes.
        Bytes,
        // A managed GCHandle the bus owns and frees once delivered.
        Managed,
    };

    // What a listener is handed. Plain C layout, since a managed listener reads it through a pointer.
    struct FGameplayMessage
    {
        uint64                  Type    = 0;
        const void*             Payload = nullptr;
        uint32                  Size    = 0;
        EGameplayMessagePayload Kind    = EGameplayMessagePayload::None;
        uint32                  Channel = 0;
        uint32                  Source  = ECS::NullEntity.GetPacked();
        // The entity a routed send reached, null for a broadcast.
        uint32                  Target  = ECS::NullEntity.GetPacked();
        uint32                  _Pad    = 0;
    };

    static_assert(sizeof(FGameplayMessage) == 40, "FGameplayMessage must match GameplayMessageBus.cs");

    struct FGameplayMessagePayloadRef
    {
        uint64                  Type   = 0;
        const void*             Data   = nullptr;
        uint32                  Size   = 0;
        EGameplayMessagePayload Kind   = EGameplayMessagePayload::None;
        const CStruct*          Struct = nullptr;
    };

    // Keys a listener on whatever a managed reference-type message carries; the managed side does the type test.
    inline constexpr uint64 kManagedGameplayMessageType = 1;

    // The stable key for a payload type name, the same from C++ and C#.
    RUNTIME_API uint64 MakeGameplayMessageType(FName TypeName);

    template<typename T>
    concept CGameplayMessagePayload = requires { T::StaticStruct(); };

    template<CGameplayMessagePayload T>
    uint64 GetGameplayMessageType()
    {
        static const uint64 Type = MakeGameplayMessageType(T::StaticStruct()->GetName());
        return Type;
    }

    // One per world. Channels are hierarchical tags, and routed sends walk the entity hierarchy to reach entity listeners.
    class RUNTIME_API FGameplayMessageBus
    {
    public:

        // Returns nonzero to mark a routed message handled, which stops it from going any further.
        using FInvoke  = uint32 (*)(void* Context, const FGameplayMessage* Message);
        using FDestroy = void (*)(void* Context);

        struct FSubscribeParams
        {
            uint32            Channel = 0;
            ECS::FEntity      Owner   = ECS::NullEntity;
            // Zero hears every type.
            uint64            Type    = 0;
            EHierarchicalTagMatch Match   = EHierarchicalTagMatch::Partial;
            FInvoke           Invoke  = nullptr;
            void*             Context = nullptr;
            FDestroy          Destroy = nullptr;
            // A managed listener lives in the script load context, so a reload drops it.
            bool              bManaged = false;
        };

        FGameplayMessageBus(ECS::FRegistry& InRegistry, ECS::FCommandBus& InCommands);
        ~FGameplayMessageBus();
        FGameplayMessageBus(const FGameplayMessageBus&) = delete;
        FGameplayMessageBus& operator = (const FGameplayMessageBus&) = delete;
        FGameplayMessageBus(FGameplayMessageBus&&) = delete;
        FGameplayMessageBus& operator=(FGameplayMessageBus&&) = delete;

        uint64 Subscribe(const FSubscribeParams& Params);
        bool   Unsubscribe(uint64 Id);
        void   UnsubscribeAll(ECS::FEntity Owner);
        void   Clear();

        // A managed handler lives in the script load context, so a reload drops them before it unloads.
        void RemoveManagedListeners();

        // Drops the listeners of entities that no longer exist.
        void PruneDeadOwners();

        // Delivers now on a thread that may, or copies the payload and delivers at the next command bus flush.
        void Send(EGameplayMessageRoute Route, ECS::FEntity Entity, uint32 Channel, const FGameplayMessagePayloadRef& Payload, bool bIncludeSelf = true);

        template<CGameplayMessagePayload T, typename TFunc>
        uint64 Subscribe(const SHierarchicalTag& Channel, TFunc&& Handler, EHierarchicalTagMatch Match = EHierarchicalTagMatch::Partial)
        {
            return SubscribeTyped<T>(ECS::NullEntity, Channel, Forward<TFunc>(Handler), Match);
        }

        // Handler may return bool, and true stops a routed send at this entity.
        template<CGameplayMessagePayload T, typename TFunc>
        uint64 Subscribe(ECS::FEntity Owner, const SHierarchicalTag& Channel, TFunc&& Handler, EHierarchicalTagMatch Match = EHierarchicalTagMatch::Partial)
        {
            return Owner.IsNull() ? 0 : SubscribeTyped<T>(Owner, Channel, Forward<TFunc>(Handler), Match);
        }

        template<CGameplayMessagePayload T>
        void Broadcast(const SHierarchicalTag& Channel, const T& Message)
        {
            Send(EGameplayMessageRoute::Broadcast, ECS::NullEntity, ChannelId(Channel), MakePayload(Message));
        }

        template<CGameplayMessagePayload T>
        void SendUp(ECS::FEntity Source, const SHierarchicalTag& Channel, const T& Message, bool bIncludeSelf = true)
        {
            Send(EGameplayMessageRoute::Up, Source, ChannelId(Channel), MakePayload(Message), bIncludeSelf);
        }

        template<CGameplayMessagePayload T>
        void SendDown(ECS::FEntity Source, const SHierarchicalTag& Channel, const T& Message, bool bIncludeSelf = true)
        {
            Send(EGameplayMessageRoute::Down, Source, ChannelId(Channel), MakePayload(Message), bIncludeSelf);
        }

        template<CGameplayMessagePayload T>
        void SendTo(ECS::FEntity Target, const SHierarchicalTag& Channel, const T& Message)
        {
            Send(EGameplayMessageRoute::To, Target, ChannelId(Channel), MakePayload(Message));
        }

        static uint32 ChannelId(const SHierarchicalTag& Tag)
        {
            return Tag.GetId();
        }

        NODISCARD uint32 NumListeners() const;

    private:

        template<CGameplayMessagePayload T>
        static FGameplayMessagePayloadRef MakePayload(const T& Message)
        {
            FGameplayMessagePayloadRef Payload;
            Payload.Type   = GetGameplayMessageType<T>();
            Payload.Data   = &Message;
            Payload.Size   = (uint32)sizeof(T);
            Payload.Kind   = std::is_trivially_copyable_v<T> ? EGameplayMessagePayload::Bytes : EGameplayMessagePayload::Struct;
            Payload.Struct = T::StaticStruct();
            return Payload;
        }

        template<CGameplayMessagePayload T, typename TFunc>
        uint64 SubscribeTyped(ECS::FEntity Owner, const SHierarchicalTag& Channel, TFunc&& Handler, EHierarchicalTagMatch Match)
        {
            using THandler = std::decay_t<TFunc>;

            FSubscribeParams Params;
            Params.Channel = ChannelId(Channel);
            Params.Owner   = Owner;
            Params.Type    = GetGameplayMessageType<T>();
            Params.Match   = Match;
            Params.Context = new THandler(Forward<TFunc>(Handler));
            Params.Destroy = [](void* Context) { delete static_cast<THandler*>(Context); };
            Params.Invoke  = [](void* Context, const FGameplayMessage* Message) -> uint32
            {
                THandler& Callable = *static_cast<THandler*>(Context);
                const T&  Value    = *static_cast<const T*>(Message->Payload);
                if constexpr (std::is_same_v<std::invoke_result_t<THandler&, const T&>, bool>)
                {
                    return Callable(Value) ? 1u : 0u;
                }
                else
                {
                    Callable(Value);
                    return 0u;
                }
            };
            return Subscribe(Params);
        }

        struct FListener
        {
            uint64            Id       = 0;
            uint64            Type     = 0;
            uint32            Channel  = 0;
            ECS::FEntity      Owner    = ECS::NullEntity;
            EHierarchicalTagMatch Match    = EHierarchicalTagMatch::Partial;
            FInvoke           Invoke   = nullptr;
            void*             Context  = nullptr;
            FDestroy          Destroy  = nullptr;
            bool              bManaged = false;
        };

        using FListenerSnapshot = TInlineVector<FListener, 8>;

        void Deliver(EGameplayMessageRoute Route, ECS::FEntity Entity, uint32 Channel, const FGameplayMessagePayloadRef& Payload, bool bIncludeSelf);
        uint64 SnapshotListeners(const THashMap<uint32, TVector<uint64>>& Index, uint32 Key, const FGameplayMessage& Message, TSpan<const uint32> ChannelChain, bool bRouted, FListenerSnapshot& Out) const;
        bool DeliverTo(const FListenerSnapshot& Listeners, uint64 SnapshotRemovals, const FGameplayMessage& Message, uint32 SentChannel, bool bRouted);
        bool RemoveLocked(uint64 Id);
        void DestroyRetired();

        ECS::FRegistry&    Registry;
        ECS::FCommandBus&  Commands;
        FDelegateHandle    ScriptReloadHandle;

        mutable FMutex                       Mutex;
        THashMap<uint64, FListener>          ById;
        // Global listeners by channel, and entity listeners by owner.
        THashMap<uint32, TVector<uint64>>    ByChannel;
        THashMap<uint32, TVector<uint64>>    ByOwner;
        // Kept until no delivery is running, since a handler may unsubscribe itself mid-call.
        TVector<FListener>                   Retired;
        uint32                               DispatchDepth = 0;
        // Bumped on every removal so a delivery only re-checks a snapshotted listener after one happened.
        TAtomic<uint64>                      Removals{0};
        uint64                               NextId = 1;
    };
}
