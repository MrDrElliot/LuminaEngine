#pragma once

#include "Platform/GenericPlatform.h"
#include "Core/Object/ObjectMacros.h"
#include "NetRole.generated.h"

namespace Lumina
{
    // Per-peer authority role for a networked entity.
    REFLECT()
    enum class ENetRole : uint8
    {
        None,            // not yet resolved
        SimulatedProxy,  // remote-owned; state arrives from the authority (interpolated)
        AutonomousProxy, // locally-controlled proxy of an authority entity (prediction later)
        Authority,       // this peer owns the truth (the server)
    };
}
