#pragma once

#include "Audio/SoundTypes.h"
#include "Containers/Vector.h"
#include "Core/Math/Math.h"
#include "World/ECS/Registry.h"

namespace Breakout
{
    using namespace Lumina;

    // The play field is fixed; the window letterboxes onto it, so gameplay never depends on resolution.
    inline constexpr float kFieldWidth  = 1920.0f;
    inline constexpr float kFieldHeight = 1080.0f;

    inline constexpr int32 kBrickColumnCount = 11;
    inline constexpr int32 kBrickRowCount    = 8;
    inline constexpr float kBrickWidth       = 150.0f;
    inline constexpr float kBrickHeight      = 44.0f;
    inline constexpr float kBrickGap         = 12.0f;
    inline constexpr float kBrickTop         = 196.0f;
    inline constexpr float kBrickPitchX      = kBrickWidth + kBrickGap;
    inline constexpr float kBrickPitchY      = kBrickHeight + kBrickGap;

    inline constexpr float kPaddleY             = 985.0f;
    inline constexpr float kPaddleHalfHeight    = 13.0f;
    inline constexpr float kPaddleBaseHalfWidth = 118.0f;
    inline constexpr float kPaddleWideHalfWidth = 196.0f;
    inline constexpr float kPaddleThinHalfWidth = 72.0f;

    inline constexpr float kBallRadius    = 13.0f;
    inline constexpr float kBallBaseSpeed = 900.0f;
    inline constexpr float kBallMaxSpeed  = 1900.0f;

    inline constexpr float kBoltSpeed      = 2100.0f;
    inline constexpr float kLaserInterval  = 0.20f;
    inline constexpr float kExplosionRadius = 190.0f;

    inline constexpr float kFixedStep       = 1.0f / 120.0f;
    inline constexpr int32 kMaxCatchUpSteps  = 5;
    inline constexpr int32 kMaxParticles     = 3600;
    inline constexpr int32 kMaxChainStages   = 24;
    inline constexpr int32 kExplosionsPerStep = 3;
    inline constexpr float kMaxHitStop        = 0.09f;
    inline constexpr float kHitStopScale      = 0.10f;

    inline constexpr int32 kBallTrailNodes      = 12;
    inline constexpr float kTrailSampleStep     = 0.011f;
    inline constexpr float kShieldRechargeTime  = 14.0f;
    inline constexpr float kFeverDuration       = 9.0f;
    inline constexpr float kFeverPerBrick       = 0.075f;
    inline constexpr float kBreachY             = kPaddleY - 104.0f;
    inline constexpr float kBossTopY            = 150.0f;
    inline constexpr float kBeatSeconds = 60.0f / 128.0f;

    inline constexpr float kSmashArmWindow   = 0.14f;
    inline constexpr float kSmashGraceWindow = 0.09f;
    inline constexpr float kSmashDuration    = 1.3f;
    inline constexpr float kVaultLinger      = 0.4f;
    inline constexpr float kWallY            = kFieldHeight - 26.0f;
    inline constexpr int32 kDraftChoices     = 3;
    inline constexpr int32 kMaxPaddles       = 2;

    struct FFieldViewport
    {
        FVector2 OriginPixels  { 0.0f, 0.0f };
        FVector2 SizePixels    { 0.0f, 0.0f };
        float    UnitsToPixels = 1.0f;
    };

    inline FFieldViewport ComputeFieldViewport(const FUIntVector2& Extent)
    {
        const float Width = float(Math::Max(Extent.x, 1u));
        const float Height = float(Math::Max(Extent.y, 1u));
        const float FieldAspect = kFieldWidth / kFieldHeight;

        FFieldViewport Result;
        Result.SizePixels = Width / Height > FieldAspect
            ? FVector2 { Height * FieldAspect, Height }
            : FVector2 { Width, Width / FieldAspect };

        Result.OriginPixels = { (Width - Result.SizePixels.x) * 0.5f, (Height - Result.SizePixels.y) * 0.5f };
        Result.UnitsToPixels = Result.SizePixels.x / kFieldWidth;
        return Result;
    }

    enum class EPhase : uint8
    {
        Title,
        Serve,
        Playing,
        LevelClear,
        Draft,
        LifeLost,
        GameOver,
    };

    enum class EGameMode : uint8
    {
        Classic,
        Endless,
        BossRush,
        Daily,
        Coop,

        Count
    };

    inline const char* GameModeName(EGameMode Mode)
    {
        switch (Mode)
        {
        case EGameMode::Classic:  return "CLASSIC";
        case EGameMode::Endless:  return "ENDLESS";
        case EGameMode::BossRush: return "BOSS RUSH";
        case EGameMode::Daily:    return "DAILY";
        default:                  return "CO-OP";
        }
    }

    inline const char* GameModeBlurb(EGameMode Mode)
    {
        switch (Mode)
        {
        case EGameMode::Classic:  return "HANDMADE STAGES   BOSS EVERY 5   DRAFT A PERK AFTER EACH CLEAR";
        case EGameMode::Endless:  return "RANDOM STAGES FOREVER   THE WALL FALLS FASTER   HOW DEEP CAN YOU GO";
        case EGameMode::BossRush: return "A BOSS ON EVERY STAGE   THREE KINDS   THEY GET MEANER";
        case EGameMode::Daily:    return "TODAYS SEED   SAME DROPS FOR EVERYONE   ONE SHOT";
        default:                  return "TWO PADDLES   P1 MOUSE OR A/D   P2 ARROW KEYS   SHARED LIVES";
        }
    }

    enum class EBrickKind : uint8
    {
        Normal,
        Reinforced,
        Explosive,
        Steel,
        Mystery,
        Mover,
        Ghost,
        Regen,
        Portal,
        Gravity,
        Bumper,
        Gold,

        Count
    };

    inline bool BrickScores(EBrickKind Kind)
    {
        return Kind != EBrickKind::Steel && Kind != EBrickKind::Portal && Kind != EBrickKind::Bumper;
    }

    inline bool BrickIsFixture(EBrickKind Kind)
    {
        return Kind == EBrickKind::Portal || Kind == EBrickKind::Bumper;
    }

    enum class EPowerUp : uint8
    {
        Widen,
        MultiBall,
        SlowTime,
        ExtraLife,
        Laser,
        Fireball,
        Catch,
        Magnet,
        Shield,
        Bomb,
        BigBall,
        Wall,
        Freeze,
        Jackpot,

        Shrink,
        SpeedUp,
        Reverse,
        Blind,
        Drop,

        Count
    };

    inline bool IsHarmful(EPowerUp Type)
    {
        return Type >= EPowerUp::Shrink;
    }

    inline const char* PowerUpName(EPowerUp Type)
    {
        switch (Type)
        {
        case EPowerUp::Widen:     return "WIDE";
        case EPowerUp::MultiBall: return "MULTI";
        case EPowerUp::SlowTime:  return "SLOW";
        case EPowerUp::ExtraLife: return "1UP";
        case EPowerUp::Laser:     return "LASER";
        case EPowerUp::Fireball:  return "FIRE";
        case EPowerUp::Catch:     return "CATCH";
        case EPowerUp::Magnet:    return "MAGNET";
        case EPowerUp::Shield:    return "SHIELD";
        case EPowerUp::Bomb:      return "BOMB";
        case EPowerUp::BigBall:   return "BIG";
        case EPowerUp::Wall:      return "WALL";
        case EPowerUp::Freeze:    return "FREEZE";
        case EPowerUp::Jackpot:   return "JACKPOT";
        case EPowerUp::Shrink:    return "SHRINK";
        case EPowerUp::SpeedUp:   return "HASTE";
        case EPowerUp::Reverse:   return "REVERSE";
        case EPowerUp::Blind:     return "BLIND";
        default:                  return "DROP";
        }
    }

    enum class EPerk : uint8
    {
        WidePaddle,
        SoftBall,
        TwinLaser,
        BigBoom,
        QuickShield,
        Magnetic,
        Lucky,
        Greedy,
        Sticky,
        HeavyBall,
        Splitter,
        Overdrive,
        Bulwark,
        VaultHunter,
        Lifeline,

        Count
    };

    struct FPerkInfo
    {
        const char* Name;
        const char* Blurb;
        uint8       MaxStacks;
    };

    inline FPerkInfo PerkInfo(EPerk Perk)
    {
        switch (Perk)
        {
        case EPerk::WidePaddle:  return { "BROAD",     "PADDLE 15 PERCENT WIDER",          3 };
        case EPerk::SoftBall:    return { "SOFT BALL", "LOWER TOP SPEED",                  2 };
        case EPerk::TwinLaser:   return { "QUAD LASER", "FOUR BOLTS AND LONGER LASERS",    1 };
        case EPerk::BigBoom:     return { "BIG BOOM",  "EXPLOSIONS REACH FURTHER",         3 };
        case EPerk::QuickShield: return { "FAST SHIELD", "SHIELD RECHARGES SOONER",        2 };
        case EPerk::Magnetic:    return { "MAGNETIC",  "DROPS DRIFT TOWARD YOU",           2 };
        case EPerk::Lucky:       return { "LUCKY",     "MORE BRICKS DROP PICKUPS",         3 };
        case EPerk::Greedy:      return { "GREEDY",    "ALL SCORE PLUS 25 PERCENT",        3 };
        case EPerk::Sticky:      return { "STICKY",    "START EACH STAGE WITH CATCH",      1 };
        case EPerk::HeavyBall:   return { "HEAVY",     "BALLS HIT TWICE AS HARD",          1 };
        case EPerk::Splitter:    return { "SPLITTER",  "LAUNCH WITH AN EXTRA BALL",        2 };
        case EPerk::Overdrive:   return { "OVERDRIVE", "FEVER FILLS AND LASTS LONGER",     2 };
        case EPerk::Bulwark:     return { "BULWARK",   "ONE FREE BREACH PER STAGE",        1 };
        case EPerk::VaultHunter: return { "VAULTER",   "BIGGER BONUS BEHIND THE WALL",     2 };
        default:                 return { "LIFELINE",  "ONE EXTRA LIFE RIGHT NOW",         9 };
        }
    }

    struct FPerks
    {
        uint8 Stacks[uint32(EPerk::Count)] {};

        NODISCARD int32 Count(EPerk Perk) const { return Stacks[uint32(Perk)]; }
        NODISCARD bool  Has(EPerk Perk) const { return Stacks[uint32(Perk)] > 0; }
        NODISCARD bool  IsMaxed(EPerk Perk) const { return Stacks[uint32(Perk)] >= PerkInfo(Perk).MaxStacks; }

        void Add(EPerk Perk) { Stacks[uint32(Perk)] = uint8(Math::Min<int32>(Stacks[uint32(Perk)] + 1, 9)); }
        void Clear() { for (uint8& Stack : Stacks) { Stack = 0; } }

        NODISCARD float BasePaddleHalfWidth() const { return kPaddleBaseHalfWidth * (1.0f + 0.15f * float(Count(EPerk::WidePaddle))); }
        NODISCARD float MaxBallSpeed() const { return kBallMaxSpeed - 180.0f * float(Count(EPerk::SoftBall)); }
        NODISCARD float ExplosionRadius() const { return kExplosionRadius + 55.0f * float(Count(EPerk::BigBoom)); }
        NODISCARD float ShieldRechargeTime() const { return kShieldRechargeTime - 4.0f * float(Count(EPerk::QuickShield)); }
        NODISCARD float DropChanceBonus() const { return 0.07f * float(Count(EPerk::Lucky)); }
        NODISCARD float ScoreScale() const { return 1.0f + 0.25f * float(Count(EPerk::Greedy)); }
        NODISCARD int32 BallDamage() const { return Has(EPerk::HeavyBall) ? 2 : 1; }
        NODISCARD float FeverDuration() const { return kFeverDuration + 3.5f * float(Count(EPerk::Overdrive)); }
        NODISCARD float FeverPerBrick() const { return kFeverPerBrick * (1.0f + 0.35f * float(Count(EPerk::Overdrive))); }
    };

    enum class EBossKind : uint8
    {
        Warden,
        Architect,
        Hydra,

        Count
    };

    inline const char* BossName(EBossKind Kind)
    {
        switch (Kind)
        {
        case EBossKind::Warden:    return "THE WARDEN";
        case EBossKind::Architect: return "THE ARCHITECT";
        default:                   return "THE HYDRA";
        }
    }

    enum class EQuadKind : uint32
    {
        Rect  = 0,
        Brick = 1,
        Disc  = 2,
        Ring  = 3,
        Spark  = 4,
        Glow   = 5,
        Bolt   = 6,
        Ribbon = 7,
        Drone  = 8,
    };


    //~ Components

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
        float     CornerRadius = 0.35f;
        float     Glow         = 0.0f;
        EQuadKind Kind         = EQuadKind::Rect;
    };

    struct FPaddle
    {
        uint8 PlayerIndex   = 0;
        float Velocity      = 0.0f;
        float HalfWidthGoal = kPaddleBaseHalfWidth;
        float WidenTimer    = 0.0f;
        float ShrinkTimer   = 0.0f;
        float LaserTimer    = 0.0f;
        float LaserCooldown = 0.0f;
        float CatchTimer    = 0.0f;
        float MagnetTimer   = 0.0f;
        float ReverseTimer  = 0.0f;
        float SmashArm      = 0.0f;
        float SmashFlash    = 0.0f;
        float Tilt          = 0.0f;
        float HitFlash      = 0.0f;
        float ShieldCharge  = 0.0f;
        float ShieldFlash   = 0.0f;
    };

    struct FBall
    {
        FVector2 Velocity { 0.0f, 0.0f };
        FVector2 Trail[kBallTrailNodes] {};
        float    Speed       = kBallBaseSpeed;
        float    TrailBudget = 0.0f;
        float    GhostTimer  = 0.0f;
        float    Squash      = 0.0f;
        float    FireTimer   = 0.0f;
        float    BigTimer    = 0.0f;
        float    SmashTimer  = 0.0f;
        float    SmashGrace  = 0.0f;
        float    PortalCooldown = 0.0f;
        float    Spin        = 0.0f;
        float    HeldOffset  = 0.0f;
        uint8    HeldBy      = 0;
        uint8    TrailCount  = 0;
        bool     bHeld       = true;

        NODISCARD float Radius() const { return BigTimer > 0.0f ? kBallRadius * 1.8f : kBallRadius; }
    };

    struct FBrick
    {
        int32      Health    = 1;
        int32      MaxHealth = 1;
        int32      Row       = 0;
        int32      Column    = 0;
        int32      PortalPair = -1;
        float      Flash     = 0.0f;
        float      Phase     = 0.0f;
        float      Shove     = 0.0f;
        float      Timer     = 0.0f;
        float      Solidity  = 1.0f;
        float      Slide     = 0.0f;
        EBrickKind Kind      = EBrickKind::Normal;

        NODISCARD bool IsSolid() const { return Solidity > 0.5f; }
    };

    struct FParticle
    {
        FVector2 Velocity  { 0.0f, 0.0f };
        FVector4 EndColor  { 0.0f, 0.0f, 0.0f, 0.0f };
        float    Life      = 0.0f;
        float    MaxLife   = 1.0f;
        float    Drag      = 1.6f;
        float    Gravity   = 0.0f;
        float    Spin      = 0.0f;
        float    StartSize = 6.0f;
        float    EndSize   = 0.0f;
        float    Bounce    = 0.0f;
    };

    struct FPowerUpDrop
    {
        EPowerUp Type = EPowerUp::Widen;
        float    Bob  = 0.0f;
        float    DriftX = 0.0f;
    };

    struct FLaserBolt
    {
        FVector2 Velocity { 0.0f, -kBoltSpeed };
        float    Life     = 1.2f;
        bool     bHostile = false;
    };

    struct FBoss
    {
        EBossKind Kind       = EBossKind::Warden;
        float     Health     = 1.0f;
        float     MaxHealth  = 1.0f;
        float     DriftPhase = 0.0f;
        float     FireTimer  = 2.4f;
        float     RepairTimer = 3.0f;
        float     ArmorTimer = 0.0f;
        float     Flash      = 0.0f;
        float     Scale      = 1.0f;
        uint8     Generation = 0;
        bool      bArmored   = false;
    };

    enum class EDroneKind : uint8
    {
        Cone,
        Tri,
        Orb,

        Count
    };

    struct FDrone
    {
        EDroneKind Kind    = EDroneKind::Cone;
        FVector2   Velocity { 0.0f, 0.0f };
        int32      Health  = 1;
        float      Age     = 0.0f;
        float      Phase   = 0.0f;
        float      Flash   = 0.0f;
        float      Radius  = 28.0f;
    };

    struct FShockwave
    {
        float    Age       = 0.0f;
        float    Duration  = 0.45f;
        float    MaxRadius = 220.0f;
        float    Thickness = 0.22f;
        float    Warp      = 0.0f;
        FVector4 Color     { 1.0f, 1.0f, 1.0f, 1.0f };
    };

    struct FScorePop
    {
        float    Age      = 0.0f;
        float    Duration = 0.9f;
        int32    Value    = 0;
        FVector2 Position { 0.0f, 0.0f };
        FVector4 Color    { 1.0f, 1.0f, 1.0f, 1.0f };
        char     Label[14] {};
        float    Size     = 3.5f;
    };


    //~ Registry singletons

    struct FGameState
    {
        EPhase    Phase        = EPhase::Title;
        EGameMode Mode         = EGameMode::Classic;
        EGameMode MenuCursor   = EGameMode::Classic;
        float  PhaseTimer   = 0.0f;
        int32  Score        = 0;
        int32  DisplayScore = 0;
        int32  HighScore    = 0;
        int32  Lives        = 3;
        int32  Level        = 1;
        int32  Combo        = 0;
        int32  BestCombo    = 0;
        float  ComboTimer   = 0.0f;
        float  SlowTimer    = 0.0f;
        float  TimeScale    = 1.0f;
        float  Elapsed      = 0.0f;
        float  FlashPulse   = 0.0f;
        float  HitStop      = 0.0f;
        float  Danger       = 0.0f;
        float  Progress     = 0.0f;
        float  FireGlow     = 0.0f;
        float  FeverMeter   = 0.0f;
        float  FeverTimer   = 0.0f;
        float  FormationDrop  = 0.0f;
        float  FormationDrift = 0.0f;
        float  BreachWarning  = 0.0f;
        float  WallTimer    = 0.0f;
        float  FreezeTimer  = 0.0f;
        float  JackpotTimer = 0.0f;
        float  BlindTimer   = 0.0f;
        float  VaultTimer   = 0.0f;
        float  VaultGlow    = 0.0f;
        float  DroneTimer   = 6.0f;
        float  LevelTime    = 0.0f;
        float  BossIntro    = 0.0f;
        int32  LevelLivesLost = 0;
        int32  LevelBestCombo = 0;
        int32  LevelSmashes = 0;
        int32  Smashes      = 0;
        int32  Vaults       = 0;
        int32  DronesDowned = 0;
        int32  BricksBroken = 0;
        int32  BricksAlive  = 0;
        int32  BricksTotal  = 1;
        int32  GradeBonus   = 0;
        char   Grade        = 'C';
        bool   bBossAlive   = false;
        bool   bBulwarkSpent = false;
        bool   bAuthoredLevel = false;
        EBossKind BossKind  = EBossKind::Warden;
        EPerk  Draft[kDraftChoices] {};
        int32  DraftCursor  = 1;
        FPerks Perks;

        NODISCARD bool IsFever() const { return FeverTimer > 0.0f; }
        NODISCARD bool IsVault() const { return VaultTimer > 0.0f; }
        NODISCARD bool IsCoop() const { return Mode == EGameMode::Coop; }
        NODISCARD int32 ScoreScale() const { return IsFever() ? 2 : 1; }
        NODISCARD int32 VaultScale() const { return IsVault() ? 2 + Perks.Count(EPerk::VaultHunter) : 0; }
        NODISCARD int32 Scaled(int32 Points) const
        {
            return int32(float(Points * ScoreScale() + Points * VaultScale()) * Perks.ScoreScale());
        }
    };

    struct FCameraShake
    {
        FVector2 Offset { 0.0f, 0.0f };
        float    Trauma = 0.0f;
        float    Chroma = 0.0f;
        float    Seed   = 0.0f;
    };

    struct FFrameInput
    {
        float PaddleTarget = kFieldWidth * 0.5f;
        float KeyAxis[kMaxPaddles] { 0.0f, 0.0f };
        bool  bUsingMouse  = false;
        bool  bLaunch      = false;
        bool  bConfirm     = false;
        bool  bSmash       = false;
        bool  bMouseMoved  = false;
        int32 Nav          = 0;
        int32 Hotkey       = -1;
    };

    struct FSoundQueue
    {
        TVector<FSoundRequest> Pending;
    };

    // Explosions queue here instead of recursing, so a long chain costs no stack and stays bounded.
    struct FExplosionQueue
    {
        TVector<FVector2> Pending;
        int32             Cursor = 0;
    };

    struct FFrameStats
    {
        float FrameMilliseconds = 0.0f;
        float WorstMilliseconds = 0.0f;
        int32 SimSteps          = 0;
        int32 DroppedSteps      = 0;
        int32 Particles         = 0;
        int32 Entities          = 0;
        int32 AlphaQuads        = 0;
        int32 AdditiveQuads     = 0;
    };

    struct FRandom
    {
        FRandomStream Stream { 0x9E3779B97F4A7C15ull, 7ull };

        float Range(float Min, float Max) { return Stream.RandRange(Min, Max); }
        float Unit() { return Stream.NextFloat(); }
        int32 Below(int32 Bound) { return Bound > 0 ? int32(Stream.NextUInt32Below(uint32(Bound))) : 0; }
        void  Reseed(uint64 Seed) { Stream.Seed(Seed, 7ull); }
    };
}
