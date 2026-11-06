#pragma once

#include "Entity.h"
#include "Containers/Invoke.h"
#include "Containers/Vector.h"

#include <type_traits>

namespace Lumina::ECS
{
    class FRegistry;

    // A component lifecycle listener. Erased to a free function plus a payload so the list stays flat.
    struct FComponentListener
    {
        using FCallback = void (*)(void* Payload, FRegistry& Registry, FEntity Entity);

        FCallback Callback = nullptr;
        void*     Payload  = nullptr;

        constexpr bool operator == (const FComponentListener& Other) const = default;
    };

    class FComponentSignal;

    // Releases a listener without knowing which component type owns it, the way the script bridge needs.
    struct FSignalConnection
    {
        FComponentSignal* Signal = nullptr;
        FComponentListener Listener;

        NODISCARD constexpr bool IsConnected() const { return Signal != nullptr; }

        void Release();
    };

    // Broadcasts to a flat list. The empty case is one load and a branch, which is what the hot path pays.
    class FComponentSignal
    {
    public:

        NODISCARD FORCEINLINE bool IsEmpty() const { return Listeners.IsEmpty(); }

        FSignalConnection Connect(FComponentListener::FCallback Callback, void* Payload = nullptr)
        {
            const FComponentListener Listener{ Callback, Payload };
            Listeners.Add(Listener);
            return FSignalConnection{ this, Listener };
        }

        void Disconnect(FComponentListener::FCallback Callback, void* Payload = nullptr)
        {
            const FComponentListener Target{ Callback, Payload };
            for (int32 Index = 0; Index < Listeners.Num(); ++Index)
            {
                if (Listeners[Index] == Target)
                {
                    Listeners.RemoveAt(Index);
                    return;
                }
            }
        }

        void DisconnectPayload(void* Payload)
        {
            for (int32 Index = Listeners.Num(); Index > 0; --Index)
            {
                if (Listeners[Index - 1].Payload == Payload)
                {
                    Listeners.RemoveAt((Index - 1));
                }
            }
        }

        // The handler is a template argument, so a call site binds and unbinds by identity.
        template<auto Callback, typename TInstance>
        FSignalConnection Connect(TInstance* Instance)
        {
            return Connect(&MemberThunk<Callback, TInstance>, Instance);
        }

        template<auto Callback, typename TInstance>
        void Disconnect(TInstance* Instance)
        {
            Disconnect(&MemberThunk<Callback, TInstance>, Instance);
        }

        template<auto Callback>
        FSignalConnection Connect()
        {
            return Connect(&FreeThunk<Callback>, nullptr);
        }

        template<auto Callback>
        void Disconnect()
        {
            Disconnect(&FreeThunk<Callback>, nullptr);
        }

        void Clear() { Listeners.Reset(); }

        FORCEINLINE void Broadcast(FRegistry& Registry, FEntity Entity) const
        {
            if (Listeners.IsEmpty())
            {
                return;
            }
            BroadcastSlow(Registry, Entity);
        }

    private:

        // A handler may take the registry and the entity, the entity alone, or nothing.
        template<auto Handler, typename TInstance>
        static void MemberThunk(void* Payload, FRegistry& Registry, FEntity Entity)
        {
            TInstance* Self = static_cast<TInstance*>(Payload);

            if constexpr (std::is_member_pointer_v<decltype(Handler)>)
            {
                if constexpr (std::is_invocable_v<decltype(Handler), TInstance&, FRegistry&, FEntity>)
                {
                    (Self->*Handler)(Registry, Entity);
                }
                else if constexpr (std::is_invocable_v<decltype(Handler), TInstance&, FEntity>)
                {
                    (Self->*Handler)(Entity);
                }
                else
                {
                    (Self->*Handler)();
                }
            }
            else
            {
                if constexpr (std::is_invocable_v<decltype(Handler), TInstance*, FRegistry&, FEntity>)
                {
                    Handler(Self, Registry, Entity);
                }
                else if constexpr (std::is_invocable_v<decltype(Handler), TInstance&, FRegistry&, FEntity>)
                {
                    Handler(*Self, Registry, Entity);
                }
                else if constexpr (std::is_invocable_v<decltype(Handler), TInstance*, FEntity>)
                {
                    Handler(Self, Entity);
                }
                else if constexpr (std::is_invocable_v<decltype(Handler), TInstance&, FEntity>)
                {
                    Handler(*Self, Entity);
                }
                else
                {
                    Handler(*Self);
                }
            }
        }

        template<auto Callback>
        static void FreeThunk(void*, FRegistry& Registry, FEntity Entity)
        {
            if constexpr (std::is_invocable_v<decltype(Callback), FRegistry&, FEntity>)
            {
                Invoke(Callback, Registry, Entity);
            }
            else if constexpr (std::is_invocable_v<decltype(Callback), FEntity>)
            {
                Invoke(Callback, Entity);
            }
            else
            {
                Invoke(Callback);
            }
        }

        // Out of line so an empty signal costs the caller nothing but the branch above.
        // Never inlined, so a pool with no listeners costs the empty check and nothing else.
        // Indexed and copied, since a handler may connect or disconnect on this very signal while it runs.
        FORCENOINLINE void BroadcastSlow(FRegistry& Registry, FEntity Entity) const
        {
            for (int32 Index = 0; Index < Listeners.Num();)
            {
                const FComponentListener Listener = Listeners[Index];
                Listener.Callback(Listener.Payload, Registry, Entity);

                // A handler that disconnected itself left the next listener in its slot.
                if (Index < Listeners.Num() && Listeners[Index] == Listener)
                {
                    ++Index;
                }
            }
        }

        TVector<FComponentListener> Listeners;
    };

    inline void FSignalConnection::Release()
    {
        if (Signal != nullptr)
        {
            Signal->Disconnect(Listener.Callback, Listener.Payload);
            Signal = nullptr;
        }
    }

    // The three lifecycle channels one component type owns, kept together so a storage carries one object.
    struct FComponentSignals
    {
        FComponentSignal OnConstruct;
        FComponentSignal OnDestroy;
        FComponentSignal OnUpdate;
    };
}
