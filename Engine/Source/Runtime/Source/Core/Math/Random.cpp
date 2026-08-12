#include "RuntimePCH.h"
#include "Random.h"

#include <atomic>
#include <random>

namespace Lumina::Math
{
    FRandomStream& ThreadRandomStream()
    {
        thread_local FRandomStream Stream = []
        {
            std::random_device Entropy;
            const uint64 Seed = (static_cast<uint64>(Entropy()) << 32u) | static_cast<uint64>(Entropy());
            
            static std::atomic<uint64> SequenceCounter{0};
            const uint64 Sequence = SequenceCounter.fetch_add(1, std::memory_order_relaxed);

            return FRandomStream(Seed, Sequence);
        }();

        return Stream;
    }
}
