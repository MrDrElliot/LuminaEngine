#pragma once

#include "Core/Math/Hash/Hash.h"
#include "Platform/GenericPlatform.h"


namespace Lumina
{
    class CObject;
    class CObjectBase;
}

namespace Lumina
{
    /** Weak-reference handle for CObjects. */
    struct RUNTIME_API FObjectHandle
    {
        int32 Index = -1;
        int32 Generation = 0;

        FObjectHandle() = default;
        FObjectHandle(const CObjectBase* InObject);
        FObjectHandle(int32 InIndex, int32 InGeneration)
            : Index(InIndex), Generation(InGeneration)
        {}


        CObject* Resolve() const;
        
        bool IsValid() const
        {
            return Index >= 0;
        }

        bool operator==(const FObjectHandle& Other) const
        {
            return Index == Other.Index && Generation == Other.Generation;
        }

        bool operator!=(const FObjectHandle& Other) const
        {
            return !(*this == Other);
        }
    };
}

namespace Lumina
{
    NODISCARD FORCEINLINE uint64 GetTypeHash(const FObjectHandle& Object) noexcept
    {
        size_t Seed = 0;
        Hash::HashCombine(Seed, Object.Index);
        Hash::HashCombine(Seed, Object.Generation);
        return static_cast<uint64>(Seed);
    }
}
