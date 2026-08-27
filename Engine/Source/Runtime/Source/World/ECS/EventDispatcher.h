#pragma once

#include "ComponentType.h"
#include "Containers/Vector.h"
#include "Memory/SmartPtr.h"

#include <type_traits>

namespace Lumina::ECS
{
    // One event type's listeners. Naming the handler in a template argument lets a caller unbind by identity.
    template<typename TEvent>
    class TEventSink
    {
    public:

        using FCallback = void (*)(void* Payload, const TEvent& Event);

        struct FListener
        {
            FCallback Callback = nullptr;
            void*     Payload  = nullptr;

            constexpr bool operator == (const FListener& Other) const = default;
        };

        template<auto Handler, typename TInstance>
        void Connect(TInstance* Instance)
        {
            Listeners.push_back(FListener{ &MemberThunk<Handler, TInstance>, Instance });
        }

        template<auto Handler, typename TInstance>
        void Disconnect(TInstance* Instance)
        {
            Remove(FListener{ &MemberThunk<Handler, TInstance>, Instance });
        }

        template<auto Handler>
        void Connect()
        {
            Listeners.push_back(FListener{ &FreeThunk<Handler>, nullptr });
        }

        template<auto Handler>
        void Disconnect()
        {
            Remove(FListener{ &FreeThunk<Handler>, nullptr });
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

        void Clear() { Listeners.clear(); }

        NODISCARD bool IsEmpty() const { return Listeners.empty(); }

        // Copied first, so a handler that binds or unbinds during dispatch cannot invalidate the walk.
        void Broadcast(const TEvent& Event) const
        {
            if (Listeners.empty())
            {
                return;
            }

            TVector<FListener> Snapshot = Listeners;
            for (const FListener& Listener : Snapshot)
            {
                Listener.Callback(Listener.Payload, Event);
            }
        }

    private:

        void Remove(const FListener& Target)
        {
            for (size_t Index = 0; Index < Listeners.size(); ++Index)
            {
                if (Listeners[Index] == Target)
                {
                    Listeners.erase(Listeners.begin() + Index);
                    return;
                }
            }
        }

        template<auto Handler, typename TInstance>
        static void MemberThunk(void* Payload, const TEvent& Event)
        {
            TInstance* Self = static_cast<TInstance*>(Payload);

            if constexpr (std::is_invocable_v<decltype(Handler), TInstance&, const TEvent&>)
            {
                (Self->*Handler)(Event);
            }
            else
            {
                (Self->*Handler)();
            }
        }

        template<auto Handler>
        static void FreeThunk(void*, const TEvent& Event)
        {
            if constexpr (std::is_invocable_v<decltype(Handler), const TEvent&>)
            {
                Handler(Event);
            }
            else
            {
                Handler();
            }
        }

        TVector<FListener> Listeners;
    };

    // A world's typed event bus, keyed by the same dense type id the component pools use.
    class FEventDispatcher
    {
    public:

        FEventDispatcher() = default;

        FEventDispatcher(const FEventDispatcher&) = delete;
        FEventDispatcher& operator = (const FEventDispatcher&) = delete;

        template<typename TEvent>
        NODISCARD TEventSink<TEvent>& Sink()
        {
            const FComponentTypeID TypeID = GetComponentTypeID<TEvent>();
            if (TypeID >= Sinks.size())
            {
                Sinks.resize(static_cast<size_t>(TypeID) + 1u);
            }

            if (Sinks[TypeID] == nullptr)
            {
                Sinks[TypeID] = MakeUnique<TSinkHolder<TEvent>>();
            }

            return static_cast<TSinkHolder<TEvent>*>(Sinks[TypeID].get())->Sink;
        }

        template<typename TEvent>
        void Trigger(const TEvent& Event)
        {
            const FComponentTypeID TypeID = GetComponentTypeID<TEvent>();
            if (TypeID >= Sinks.size() || Sinks[TypeID] == nullptr)
            {
                return;
            }

            static_cast<TSinkHolder<TEvent>*>(Sinks[TypeID].get())->Sink.Broadcast(Event);
        }

        template<typename TEvent>
        void Trigger()
        {
            Trigger(TEvent{});
        }

        // Drops every listener that named this instance, whatever the event type.
        void DisconnectPayload(void* Payload)
        {
            for (TUniquePtr<FSinkHolder>& Holder : Sinks)
            {
                if (Holder != nullptr)
                {
                    Holder->DisconnectPayload(Payload);
                }
            }
        }

        void Clear() { Sinks.clear(); }

    private:

        struct FSinkHolder
        {
            virtual ~FSinkHolder() = default;
            virtual void DisconnectPayload(void* Payload) = 0;
        };

        template<typename TEvent>
        struct TSinkHolder final : FSinkHolder
        {
            void DisconnectPayload(void* Payload) override { Sink.DisconnectPayload(Payload); }

            TEventSink<TEvent> Sink;
        };

        TVector<TUniquePtr<FSinkHolder>> Sinks;
    };
}
