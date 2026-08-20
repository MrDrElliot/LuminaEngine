#include "RuntimePCH.h"
#include "SmartPtr.h"

#include "Platform/Time/PlatformTime.h"

namespace Lumina::SharedPtrDetail
{
    namespace
    {
        // Striped so unrelated pointers rarely collide, and padded so two slots never share a line.
        constexpr size_t kSlotCount = 64;

        struct alignas(Threading::kCacheLineSize) FSlot
        {
            TAtomic<uint32> Taken{ 0 };
        };

        FSlot GSlots[kSlotCount];

        FORCEINLINE FSlot& SlotFor(const void* Address) noexcept
        {
            const uintptr_t Value = reinterpret_cast<uintptr_t>(Address);
            return GSlots[(Value >> 4) & (kSlotCount - 1)];
        }
    }

    void LockAtomicSlot(const void* Address) noexcept
    {
        FSlot& Slot = SlotFor(Address);

        while (Slot.Taken.exchange(1, std::memory_order_acquire) != 0)
        {
            PlatformTime::YieldThread();
        }
    }

    void UnlockAtomicSlot(const void* Address) noexcept
    {
        SlotFor(Address).Taken.store(0, std::memory_order_release);
    }
}
