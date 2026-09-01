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
        LifeLost,
        GameOver,
    };

    enum class EBrickKind : uint8
    {
        Normal,
        Reinforced,
        Explosive,
        Steel,
        Mystery,
    };

    enum class EPowerUp : uint8
    {
        Widen,
        MultiBall,
        SlowTime,
        ExtraLife,
        Laser,
        Fireball,
        Catch,
        Shrink,
        SpeedUp,

        Count
    };

    inline bool IsHarmful(EPowerUp Type)
    {
        return Type == EPowerUp::Shrink || Type == EPowerUp::SpeedUp;
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
        float Velocity      = 0.0f;
        float HalfWidthGoal = kPaddleBaseHalfWidth;
        float WidenTimer    = 0.0f;
        float ShrinkTimer   = 0.0f;
        float LaserTimer    = 0.0f;
        float LaserCooldown = 0.0f;
        float CatchTimer    = 0.0f;
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
        float    Spin        = 0.0f;
        float    HeldOffset  = 0.0f;
        uint8    TrailCount  = 0;
        bool     bHeld       = true;
    };

    struct FBrick
    {
        int32      Health    = 1;
        int32      MaxHealth = 1;
        int32      Row       = 0;
        int32      Column    = 0;
        float      Flash     = 0.0f;
        float      Phase     = 0.0f;
        float      Shove     = 0.0f;
        EBrickKind Kind      = EBrickKind::Normal;
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
    };

    struct FLaserBolt
    {
        FVector2 Velocity { 0.0f, -kBoltSpeed };
        float    Life     = 1.2f;
        bool     bHostile = false;
    };

    struct FBoss
    {
        float Health     = 1.0f;
        float MaxHealth  = 1.0f;
        float DriftPhase = 0.0f;
        float FireTimer  = 2.4f;
        float Flash      = 0.0f;
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
    };


    //~ Registry singletons

    struct FGameState
    {
        EPhase Phase        = EPhase::Title;
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
        int32  BricksAlive  = 0;
        int32  BricksTotal  = 1;
        bool   bBossAlive   = false;

        NODISCARD bool IsFever() const { return FeverTimer > 0.0f; }
        NODISCARD int32 ScoreScale() const { return IsFever() ? 2 : 1; }
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
        float KeyAxis      = 0.0f;
        bool  bUsingMouse  = false;
        bool  bLaunch      = false;
        bool  bConfirm     = false;
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
    };
}
