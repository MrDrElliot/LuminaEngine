#pragma once
#include <atomic>


namespace Lumina
{
    template<typename T>
    using TAtomic = std::atomic<T>;

    namespace Atomic
    {
        constexpr auto MemoryOrderRelaxed       = std::memory_order_relaxed;
        constexpr auto MemoryOrderAcquire       = std::memory_order_acquire;
        constexpr auto MemoryOrderRelease       = std::memory_order_release;
        constexpr auto MemoryOrderAcqRel        = std::memory_order_acq_rel;
        constexpr auto MemoryOrderSeqCst        = std::memory_order_seq_cst;
    }
}
