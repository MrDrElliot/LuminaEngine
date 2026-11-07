#pragma once

#include "World/ECS/Registry.h"

#include "Containers/String.h"
#include "Containers/StringView.h"

namespace Lumina::Agent
{
    // Opaque name an agent holds for an entity between calls, since a raw handle is a recycled slot.
    class AGENTCORE_API FEntityTokens
    {
    public:

        // Names one entity in one registry. Empty for an entity that does not exist.
        NODISCARD static FString Mint(ECS::FRegistry& Registry, ECS::FEntity Entity);

        // False with a reason when the token is malformed, minted in another registry, or names a dead entity.
        NODISCARD static bool Resolve(const ECS::FRegistry& Registry, FStringView Token,
            ECS::FEntity& OutEntity, FString& OutError);
    };
}
