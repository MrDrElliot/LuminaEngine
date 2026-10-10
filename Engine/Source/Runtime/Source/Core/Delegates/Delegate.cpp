#include "RuntimePCH.h"
#include "Delegate.h"

namespace Lumina
{
    uint64 NewDelegateHandleID()
    {
        static TAtomic<uint64> NextID{1};
        return NextID.fetch_add(1, Atomic::MemoryOrderRelaxed);
    }
}
