#include "RuntimePCH.h"
#include "Hash.h"
#include "xxhash/xxhash.h"

namespace Lumina::Hash
{
    uint32 XXHash::GetHash32(const void* Data, size_t Size, uint32 Seed)
    {
        return XXH32(Data, Size, Seed);
    }

    uint64 XXHash::GetHash64(const void* Data, size_t Size, uint64 Seed)
    {
        return XXH64(Data, Size, Seed);
    }
}
