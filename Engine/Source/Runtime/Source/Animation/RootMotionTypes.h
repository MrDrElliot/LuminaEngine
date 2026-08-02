#pragma once

#include "Core/Object/ObjectMacros.h"
#include "Platform/GenericPlatform.h"
#include "RootMotionTypes.generated.h"

namespace Lumina
{
    // Per-component override of an animation asset's root-motion lock.
    REFLECT()
    enum class ERootMotionLockMode : uint8
    {
        // Use the playing animation asset's bLockRootMotion flag.
        FromAsset,
        // Always pin the root to the bind pose, ignoring the asset.
        ForceLock,
        // Never lock the root, ignoring the asset.
        ForceUnlock,
    };
}
