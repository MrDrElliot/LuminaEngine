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
