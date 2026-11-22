#pragma once

#include "Containers/HashTable.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Core/Threading/Sync.h"
#include "World/ECS/ComponentType.h"

namespace Lumina
{
    struct FNetWorldState;
    namespace ECS { class FEventQueues; }

    // Where a C# message keeps its sender and its entity ids, since it has no reflection to say.
    struct FScriptNetMessageLayout
    {
        uint32          SenderOffset = 0;
        TVector<uint32> EntityOffsets;
    };

    // The C# message layouts the scripts registered, read by any thread that writes or reads one of their messages.
    class NETWORKINGRUNTIME_API FScriptNetMessages
    {
    public:

        void Register(ECS::FComponentTypeID TypeID, uint32 SenderOffset, TSpan<const uint32> EntityOffsets);

        // A copy, so a reload registering again cannot change it under the caller.
        bool Find(ECS::FComponentTypeID TypeID, FScriptNetMessageLayout& OutLayout) const;

    private:

        mutable FMutex                                             Lock;
        THashMap<ECS::FComponentTypeID, FScriptNetMessageLayout>   Layouts;
    };

    namespace NetMessages
    {
        // A framed message carrying the event, or false when the type is not a message or is too large to send.
        NETWORKINGRUNTIME_API bool Write(FNetWorldState& State, const FScriptNetMessages& Scripts, ECS::FComponentTypeID TypeID, const void* Message, TVector<uint8>& OutMsg);

        // Raises an arrived message as its event. Anything not derived from SNetMessage or registered as a C# message is dropped.
        NETWORKINGRUNTIME_API void Apply(FNetWorldState& State, const FScriptNetMessages& Scripts, uint32 Sender, const uint8* Data, SIZE_T Size);

        // Raises Message here as if it had arrived from Sender.
        NETWORKINGRUNTIME_API void Deliver(ECS::FEventQueues& Events, const FScriptNetMessages& Scripts, ECS::FComponentTypeID TypeID, const void* Message, uint32 Sender);
    }
}
