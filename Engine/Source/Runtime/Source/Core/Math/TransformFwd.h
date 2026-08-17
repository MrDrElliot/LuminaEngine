#pragma once

namespace Lumina
{
#if defined(REFLECTION_PARSER)
    // Transform.h feeds the Reflector a hand-written FTransform stub, which an alias here would collide with.
    struct FTransform;
#else
    struct VTransform;
    using FTransform = VTransform;
#endif
}
