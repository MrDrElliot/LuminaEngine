#include "RuntimePCH.h"
#include "EventProcessor.h"

#include "Log/Log.h"

namespace Lumina
{
    void FEventProcessor::RegisterEventHandler(IEventHandler* InHandler, int32 Priority)
    {
        for (const FEntry& E : EventHandlers)
        {
            if (E.Handler == InHandler)
            {
                LOG_ERROR("Event Handler Already Registered!");
                return;
            }
        }

        // Sorted descending, equal priorities keep registration order.
        FEntry NewEntry{ InHandler, Priority };
        int32 Index = 0;
        for (; Index < EventHandlers.Num(); ++Index)
        {
            if (EventHandlers[Index].Priority < Priority)
            {
                break;
            }
        }
        EventHandlers.Insert(NewEntry, Index);

        if (Index <= DispatchCursor)
        {
            ++DispatchCursor;
        }
    }

    void FEventProcessor::UnregisterEventHandler(IEventHandler* InHandler)
    {
        for (auto It = EventHandlers.begin(); It != EventHandlers.end(); ++It)
        {
            if (It->Handler == InHandler)
            {
                const int32 RemovedIndex = static_cast<int32>(It - EventHandlers.begin());
                EventHandlers.RemoveAt(RemovedIndex);

                if (RemovedIndex <= DispatchCursor)
                {
                    --DispatchCursor;
                }
                return;
            }
        }
    }

    void FEventProcessor::Clear()
    {
        EventHandlers.Reset();
    }

    void FEventProcessor::DispatchEvent(FEvent& Event)
    {
        const int32 SavedCursor = DispatchCursor;

        for (DispatchCursor = 0; DispatchCursor < EventHandlers.Num(); ++DispatchCursor)
        {
            if (EventHandlers[DispatchCursor].Handler->OnEvent(Event))
            {
                break;
            }
        }

        DispatchCursor = SavedCursor;
    }
}
