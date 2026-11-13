#pragma once

#include "Containers/RingBuffer.h"

namespace Lumina
{
    inline void PushHistorySample(TRingBuffer<float>& History, float Value, int32 Capacity)
    {
        History.SetCapacity(Capacity);
        History.Add(Value);
    }

    // Plot getter for a TRingBuffer<float>, so plots read oldest to newest without flattening the ring.
    inline float HistorySampleAt(void* History, int Index)
    {
        return (*static_cast<const TRingBuffer<float>*>(History))[Index];
    }
}
