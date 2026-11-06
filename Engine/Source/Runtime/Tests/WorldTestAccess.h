#pragma once

#include "World/World.h"

namespace Lumina
{
    // What a test needs from a world's internals to run systems by hand, kept out of CWorld's public surface.
    struct FWorldTestAccess
    {
        static const SSystemContext& SystemContext(const CWorld& World) { return World.SystemContext; }
        static void RebuildSystemSchedule(CWorld& World) { World.RebuildSystemSchedule(); }
        static void TickFixedUpdate(CWorld& World, double FrameDeltaSeconds) { World.TickFixedUpdate(FrameDeltaSeconds); }
    };
}
