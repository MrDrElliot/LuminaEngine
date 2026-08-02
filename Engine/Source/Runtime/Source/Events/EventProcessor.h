#pragma once

#include "Containers/Array.h"
#include "Events/Event.h"

namespace Lumina
{
    /** Higher = runs first; first handler returning true stops propagation. */
    enum class EInputLayer : int32
    {
        Viewport     = 1000,
        EditorChrome = 500,
        Default      = 0,
    };

    class IEventHandler
    {
    public:
        virtual ~IEventHandler() = default;

        virtual bool OnEvent(FEvent& Event) { return false; }
    };

    class FEventProcessor
    {
    public:

        RUNTIME_API void RegisterEventHandler(IEventHandler* InHandler, int32 Priority = (int32)EInputLayer::Default);
        RUNTIME_API void UnregisterEventHandler(IEventHandler* InHandler);

        RUNTIME_API void Clear();

        template<typename TEvent, typename... Args>
        requires(eastl::is_base_of_v<FEvent, TEvent> && eastl::is_constructible_v<TEvent, Args&&...>)
        void Dispatch(Args&&... InArgs)
        {
            TEvent Event(eastl::forward<Args>(InArgs)...);
            DispatchEvent(Event);
        }

    private:

        struct FEntry
        {
            IEventHandler* Handler;
            int32          Priority;
        };

        void DispatchEvent(FEvent& Event);

        TVector<FEntry> EventHandlers;
    };
}
