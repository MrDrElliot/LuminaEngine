#pragma once

#include "Audio/SoundTypes.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "World/ECS/Registry.h"

namespace Umbral
{
    using namespace Lumina;

    inline constexpr float kWorldSize    = 14000.0f;
    inline constexpr float kViewWidth    = 1920.0f;
    inline constexpr float kViewHeight   = 1080.0f;

    inline constexpr int32 kMaxAgents    = 900000;
    inline constexpr int32 kGridChunks   = 16;
    inline constexpr float kGridCell     = 100.0f;
    inline constexpr int32 kGridSide     = int32(kWorldSize / kGridCell) + 1;
    inline constexpr int32 kGridCells    = kGridSide * kGridSide;

    inline constexpr float kFixedStep       = 1.0f / 60.0f;
    inline constexpr int32 kMaxCatchUpSteps = 3;
    inline constexpr int32 kMaxParticles    = 4000;
    inline constexpr float kBeatSeconds     = 60.0f / 96.0f;

    inline constexpr float kPlayerRadius    = 22.0f;
    inline constexpr float kPlayerSpeed     = 420.0f;
    inline constexpr float kPlayerMaxHealth = 100.0f;
    inline constexpr float kPickupRadius    = 190.0f;

    inline constexpr float kCardWidth  = 470.0f;
    inline constexpr float kCardHeight = 520.0f;
    inline constexpr float kCardGap    = 44.0f;
    inline constexpr float kCardY      = 700.0f;

    struct FCardRect
    {
        FVector2 Center { 0.0f, 0.0f };
        FVector2 Half   { 0.0f, 0.0f };
    };

    inline FCardRect LevelUpCardRect(int32 Slot, const FVector2& ViewSize)
    {
        const float Total = kCardWidth * 3.0f + kCardGap * 2.0f;
        const float FirstX = ViewSize.x * 0.5f - Total * 0.5f + kCardWidth * 0.5f;

        FCardRect Rect;
        Rect.Center = { FirstX + float(Slot) * (kCardWidth + kCardGap), kCardY };
        Rect.Half   = { kCardWidth * 0.5f, kCardHeight * 0.5f };
        return Rect;
    }

    enum class EPhase : uint8
    {
        Title,
        Playing,
        LevelUp,
        Dead,
    };

    enum class EAgentKind : uint8
    {
        Wisp,
        Crawler,
        Brute,
        Shade,

        Count
    };

    enum class EWeapon : uint8
    {
        Blades,
        Soulbolt,
        Nova,
        Pyre,
        Maw,
        Chain,
        Gloom,

        Count
    };

    enum class EQuadKind : uint32
    {
        Rect   = 0,
        Disc   = 1,
        Ring   = 2,
        Spark  = 3,
        Glow   = 4,
        Blade  = 5,
        Ribbon = 6,
        Sigil  = 7,
        Mote   = 8,
        Bolt   = 9,
        Panel  = 10,
    };

    inline FVector4 WeaponColor(EWeapon Weapon)
    {
        switch (Weapon)
        {
        case EWeapon::Soulbolt: return { 0.34f, 0.72f, 1.15f, 1.0f };
        case EWeapon::Nova:     return { 0.86f, 0.40f, 1.20f, 1.0f };
        case EWeapon::Pyre:     return { 1.15f, 0.52f, 0.16f, 1.0f };
        case EWeapon::Maw:      return { 0.62f, 0.20f, 0.95f, 1.0f };
        case EWeapon::Chain:    return { 0.95f, 0.95f, 0.42f, 1.0f };
        case EWeapon::Gloom:    return { 0.30f, 0.86f, 0.62f, 1.0f };
        default:                return { 0.42f, 1.00f, 1.15f, 1.0f };
        }
    }

    inline EQuadKind WeaponIcon(EWeapon Weapon)
    {
        switch (Weapon)
        {
        case EWeapon::Soulbolt: return EQuadKind::Bolt;
        case EWeapon::Nova:     return EQuadKind::Ring;
        case EWeapon::Pyre:     return EQuadKind::Glow;
        case EWeapon::Maw:      return EQuadKind::Ring;
        case EWeapon::Chain:    return EQuadKind::Mote;
        case EWeapon::Gloom:    return EQuadKind::Disc;
        default:                return EQuadKind::Blade;
        }
    }

    struct FAgentStats
    {
        float Health;
        float Speed;
        float Radius;
        float Damage;
        float Souls;
    };

    inline FAgentStats StatsFor(EAgentKind Kind)
    {
        switch (Kind)
        {
        case EAgentKind::Crawler: return { 14.0f, 132.0f, 11.0f, 9.0f,  2.0f };
        case EAgentKind::Brute:   return { 60.0f,  86.0f, 19.0f, 22.0f, 6.0f };
        case EAgentKind::Shade:   return { 26.0f, 178.0f, 13.0f, 14.0f, 4.0f };
        default:                  return {  6.0f, 108.0f,  9.0f, 5.0f,  1.0f };
        }
    }

    inline FVector4 ColorFor(EAgentKind Kind)
    {
        switch (Kind)
        {
        case EAgentKind::Crawler: return { 0.62f, 0.16f, 0.20f, 1.0f };
        case EAgentKind::Brute:   return { 0.48f, 0.20f, 0.08f, 1.0f };
        case EAgentKind::Shade:   return { 0.26f, 0.14f, 0.52f, 1.0f };
        default:                  return { 0.16f, 0.22f, 0.34f, 1.0f };
        }
    }


    //~ Components for everything that is not a swarm agent

    struct FBody
    {
        FVector2 Position { 0.0f, 0.0f };
        FVector2 HalfSize { 0.0f, 0.0f };
        float    Rotation = 0.0f;
    };

    struct FVisual
    {
        FVector4  Color  { 1.0f, 1.0f, 1.0f, 1.0f };
        FVector4  Accent { 1.0f, 1.0f, 1.0f, 1.0f };
        float     CornerRadius = 0.4f;
        float     Glow         = 1.0f;
        float     Lit          = 0.0f;
        EQuadKind Kind         = EQuadKind::Disc;
    };

    struct FLight
    {
        FVector4 Color  { 1.0f, 0.8f, 0.5f, 1.0f };
        float    Radius = 300.0f;
        float    Energy = 1.0f;
    };

    struct FProjectile
    {
        FVector2 Velocity { 0.0f, 0.0f };
        float    Life     = 2.0f;
        float    Damage   = 10.0f;
        float    Radius   = 26.0f;
        float    Homing   = 0.0f;
        int32    Pierce   = 1;
    };

    struct FNova
    {
        float Age       = 0.0f;
        float Duration  = 0.7f;
        float MaxRadius = 640.0f;
        float Damage    = 26.0f;
    };

    struct FPyre
    {
        float Age      = 0.0f;
        float Duration = 4.5f;
        float Radius   = 130.0f;
        float Damage   = 34.0f;
        float Tick     = 0.0f;
    };

    struct FMaw
    {
        float Age      = 0.0f;
        float Duration = 3.2f;
        float Radius   = 420.0f;
        float Pull     = 900.0f;
        float Damage   = 60.0f;
        float Spin     = 0.0f;
    };

    struct FArc
    {
        FVector2 From { 0.0f, 0.0f };
        FVector2 To   { 0.0f, 0.0f };
        float    Age  = 0.0f;
        float    Life = 0.20f;
    };

    struct FSoulMote
    {
        float Value  = 1.0f;
        float Age    = 0.0f;
        bool  bDrawn = false;
    };

    struct FParticle
    {
        FVector2 Velocity { 0.0f, 0.0f };
        FVector4 EndColor { 0.0f, 0.0f, 0.0f, 0.0f };
        float    Life      = 0.0f;
        float    MaxLife   = 1.0f;
        float    Drag      = 2.0f;
        float    StartSize = 6.0f;
        float    EndSize   = 0.0f;
    };

    struct FDamageNumber
    {
        FVector2 Position { 0.0f, 0.0f };
        float    Age      = 0.0f;
        int32    Value    = 0;
        FVector4 Color    { 1.0f, 1.0f, 1.0f, 1.0f };
    };


    //~ Registry singletons

    struct FPlayerState
    {
        FVector2 Position { kWorldSize * 0.5f, kWorldSize * 0.5f };
        FVector2 Facing   { 1.0f, 0.0f };
        float    Health   = kPlayerMaxHealth;
        float    HurtFlash = 0.0f;
        float    Invuln    = 0.0f;
        float    BladePhase = 0.0f;
        float    Torch      = 1.0f;
    };

    struct FWeaponState
    {
        int32 Level[int32(EWeapon::Count)] {};
        float Cooldown[int32(EWeapon::Count)] {};
    };

    struct FRunState
    {
        EPhase Phase       = EPhase::Title;
        float  Elapsed     = 0.0f;
        float  PhaseTimer  = 0.0f;
        int32  Level       = 1;
        float  Souls       = 0.0f;
        float  SoulsNeeded = 12.0f;
        int64  Kills       = 0;
        int64  BestKills   = 0;
        float  Danger      = 0.0f;
        float  HitStop     = 0.0f;
        float  ShakeTrauma = 0.0f;
        FVector2 ShakeOffset { 0.0f, 0.0f };
        int32  Choices[3] {};
        int32  ChoiceCount = 0;
        float  BannerTimer = 0.0f;
        int32  Hovered     = -1;
    };

    struct FFrameInput
    {
        FVector2 Move      { 0.0f, 0.0f };
        FVector2 MouseView { 0.0f, 0.0f };
        FVector2 ViewSize  { kViewWidth, kViewHeight };
        bool bUp = false, bDown = false, bLeft = false, bRight = false;
        bool bConfirm = false;
        bool bClick   = false;
        int32 Choice = -1;
    };

    struct FSoundQueue
    {
        TVector<FSoundRequest> Pending;
    };

    struct FRandom
    {
        FRandomStream Stream { 0xC0FFEE123456789ull, 3ull };

        float Range(float Min, float Max) { return Stream.RandRange(Min, Max); }
        float Unit() { return Stream.NextFloat(); }
    };

    struct FFrameStats
    {
        float FrameMilliseconds = 0.0f;
        float WorstMilliseconds = 0.0f;
        float SwarmMilliseconds = 0.0f;
        int32 SimSteps    = 0;
        int32 Agents      = 0;
        int32 Drawn       = 0;
        int32 Particles   = 0;
    };
}
