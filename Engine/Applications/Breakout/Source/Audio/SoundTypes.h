#pragma once

#include "Platform/GenericPlatform.h"

namespace Breakout
{
    using namespace Lumina;

    enum class ESound : uint8
    {
        WallHit,
        PaddleHit,
        BrickHit,
        BrickBreak,
        SteelHit,
        Explosion,
        ComboUp,
        Launch,
        Laser,
        FireballStart,
        Catch,
        Release,
        PowerUpDrop,
        PowerUpCollect,
        PowerDown,
        MultiBall,
        SlowTime,
        ExtraLife,
        Danger,
        LifeLost,
        LevelClear,
        GameOver,
        FeverStart,
        FeverEnd,
        ShieldReady,
        ShieldSave,
        BossHit,
        BossDeath,
        Hazard,
        Breach,
        UiMove,
        UiConfirm,
        Smash,
        VaultEnter,
        DroneHit,
        DroneDeath,
        Portal,
        Bumper,
        PerkOffer,
        PerkPick,
        Repair,
        Magnet,
        Bomb,
        WallUp,
        WallBounce,
        Freeze,
        Reverse,
        Blind,
        Gold,
        Regen,
        Grade,
        BossIntro,
        BossSplit,
        ArmorUp,

        Count
    };

    enum class EMusicMood : uint8
    {
        Menu,
        Play,
        Boss,
        Fever,
        Vault,
        Draft,
        Loss,
        Clear,
    };

    struct FSoundRequest
    {
        ESound Sound  = ESound::UiMove;
        float  Pitch  = 1.0f;
        float  Volume = 1.0f;
        float  Pan    = 0.0f;
    };
}
