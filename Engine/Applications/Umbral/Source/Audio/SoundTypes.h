#pragma once

#include "Platform/GenericPlatform.h"

namespace Umbral
{
    using namespace Lumina;

    enum class ESound : uint8
    {
        BladeHit,
        BoltFire,
        BoltHit,
        NovaCast,
        PyreLight,
        PyreBurn,
        AgentDie,
        BruteDie,
        PlayerHurt,
        PlayerDie,
        SoulPickup,
        LevelUp,
        UpgradePick,
        WaveWarn,
        UiMove,
        UiConfirm,

        Count
    };

    struct FSoundRequest
    {
        ESound Sound  = ESound::UiMove;
        float  Pitch  = 1.0f;
        float  Volume = 1.0f;
        float  Pan    = 0.0f;
    };
}
