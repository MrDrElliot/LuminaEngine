#pragma once

#include "Entity.h"
#include "Containers/Vector.h"

#include <functional>
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

        NODISCARD FORCEINLINE bool IsEmpty() const { return Listeners.empty(); }

        FSignalConnection Connect(FComponentListener::FCallback Callback, void* Payload = nullptr)
        {
            const FComponentListener Listener{ Callback, Payload };
            Listeners.push_back(Listener);
            return FSignalConnection{ this, Listener };
        }

        void Disconnect(FComponentListener::FCallback Callback, void* Payload = nullptr)
        {
            const FComponentListener Target{ Callback, Payload };
            for (size_t Index = 0; Index < Listeners.size(); ++Index)
            {
                if (Listeners[Index] == Target)
                {
                    Listeners.erase(Listeners.begin() + Index);
                    return;
                }
            }
        }

        void DisconnectPayload(void* Payload)
        {
            for (size_t Index = Listeners.size(); Index > 0; --Index)
            {
                if (Listeners[Index - 1].Payload == Payload)
                {
                    Listeners.erase(Listeners.begin() + (Index - 1));
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

        void Clear() { Listeners.clear(); }

        FORCEINLINE void Broadcast(FRegistry& Registry, FEntity Entity) const
        {
            if (Listeners.empty())
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
                std::invoke(Callback, Registry, Entity);
            }
            else if constexpr (std::is_invocable_v<decltype(Callback), FEntity>)
            {
                std::invoke(Callback, Entity);
            }
            else
            {
                std::invoke(Callback);
            }
        }

        // Out of line so an empty signal costs the caller nothing but the branch above.
        void BroadcastSlow(FRegistry& Registry, FEntity Entity) const
        {
            for (const FComponentListener& Listener : Listeners)
            {
                Listener.Callback(Listener.Payload, Registry, Entity);
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
