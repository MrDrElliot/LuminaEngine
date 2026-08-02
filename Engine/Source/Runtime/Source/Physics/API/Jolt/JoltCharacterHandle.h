#pragma once

// <Jolt/Jolt.h> must precede CharacterVirtual; this header bundles them.
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

namespace Lumina
{
    // Pimpl wrapper so component headers can hold a CharacterVirtual without leaking Jolt headers.
    struct FJoltCharacterHandle
    {
        JPH::Ref<JPH::CharacterVirtual> Ref;
    };
}
