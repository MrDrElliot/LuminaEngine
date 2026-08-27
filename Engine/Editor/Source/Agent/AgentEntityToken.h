#pragma once

#include "Containers/String.h"
#include "Containers/StringView.h"
#include "World/Entity/EntityHandle.h"

namespace Lumina::Agent
{
    // Opaque name an agent holds for an entity between calls, since a raw handle is a recycled slot.
    class EDITOR_API FEntityTokens
    {
    public:

        // Names one entity in one world. Empty for an entity that does not exist.
        NODISCARD static FString Mint(const FEntityRegistry& Registry, FEntity Entity);

        // False with a reason when the token is malformed, from an older world, or names a dead entity.
        NODISCARD static bool Resolve(const FEntityRegistry& Registry, FStringView Token,
            FEntity& OutEntity, FString& OutError);

        // Retires every token minted so far, which is what a world swap has to do to them.
        static void InvalidateAll();

        NODISCARD static uint64 GetEpoch();
    };
}
