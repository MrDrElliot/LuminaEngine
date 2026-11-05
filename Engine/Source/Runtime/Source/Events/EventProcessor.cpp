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

        // Sorted descending; equal priorities keep registration order.
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
    }

    void FEventProcessor::UnregisterEventHandler(IEventHandler* InHandler)
    {
        for (auto It = EventHandlers.begin(); It != EventHandlers.end(); ++It)
        {
            if (It->Handler == InHandler)
            {
                EventHandlers.RemoveAt(static_cast<int32>(It - EventHandlers.begin()));
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
        // Handlers may unregister mid-dispatch; iterate a snapshot.
        TVector<FEntry> Snapshot = EventHandlers;
        for (const FEntry& Entry : Snapshot)
        {
            if (Entry.Handler->OnEvent(Event))
            {
                return;
            }
        }
    }
}
