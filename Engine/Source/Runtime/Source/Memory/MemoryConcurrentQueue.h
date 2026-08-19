#pragma once

#include "concurrentqueue.h"
#include "Memory/Memory.h"

namespace Lumina::Memory
{
    struct FTrackedConcurrentQueueTraits : moodycamel::ConcurrentQueueDefaultTraits
    {
        static void* malloc(size_t Size)
        {
            return Lumina::Memory::Malloc(Size);
        }

        static void free(void* Ptr)
        {
            Lumina::Memory::Free(Ptr);
        }
    };
}

namespace Lumina
{
    // Lives here rather than under Containers so its filename cannot shadow the vendored concurrentqueue.h.
    template <typename T, typename Traits = Memory::FTrackedConcurrentQueueTraits>
    using TConcurrentQueue = moodycamel::ConcurrentQueue<T, Traits>;
}
