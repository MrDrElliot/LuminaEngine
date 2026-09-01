#include "Game.h"

#include "Containers/Vector.h"

namespace Breakout
{
    namespace
    {
        constexpr float kBrickPitchX = kBrickWidth + kBrickGap;
        constexpr float kBrickPitchY = kBrickHeight + kBrickGap;
        constexpr float kBrickLeft   = (kFieldWidth - (kBrickColumnCount * kBrickPitchX - kBrickGap)) * 0.5f + kBrickWidth * 0.5f;

        const FVector4 kRowColors[kBrickRowCount] =
        {
            { 1.00f, 0.18f, 0.58f, 1.0f },
            { 1.00f, 0.36f, 0.20f, 1.0f },
            { 1.00f, 0.72f, 0.16f, 1.0f },
            { 0.62f, 1.00f, 0.26f, 1.0f },
            { 0.16f, 1.00f, 0.72f, 1.0f },
            { 0.20f, 0.68f, 1.00f, 1.0f },
            { 0.42f, 0.44f, 1.00f, 1.0f },
            { 0.78f, 0.34f, 1.00f, 1.0f },
        };

        const FVector4 kFireColor { 1.00f, 0.42f, 0.10f, 1.0f };

        FVector4 PowerUpColor(EPowerUp Type)
        {
            switch (Type)
            {
            case EPowerUp::Widen:     return { 0.25f, 0.85f, 1.00f, 1.0f };
            case EPowerUp::MultiBall: return { 1.00f, 0.30f, 0.85f, 1.0f };
            case EPowerUp::SlowTime:  return { 1.00f, 0.80f, 0.22f, 1.0f };
            case EPowerUp::ExtraLife: return { 0.35f, 1.00f, 0.45f, 1.0f };
            case EPowerUp::Laser:     return { 1.00f, 0.24f, 0.34f, 1.0f };
            case EPowerUp::Fireball:  return { 1.00f, 0.48f, 0.08f, 1.0f };
            case EPowerUp::Catch:     return { 0.55f, 0.60f, 1.00f, 1.0f };
            default:                  return { 0.85f, 0.10f, 0.20f, 1.0f };
            }
        }

        FVector4 BrickBaseColor(const FBrick& Brick)
        {
            switch (Brick.Kind)
            {
            case EBrickKind::Explosive: return { 1.00f, 0.30f, 0.10f, 1.0f };
            case EBrickKind::Steel:     return { 0.62f, 0.68f, 0.80f, 1.0f };
            case EBrickKind::Mystery:   return { 0.85f, 0.40f, 1.00f, 1.0f };
            default:                    return kRowColors[Brick.Row];
            }
        }

        bool BrickPresent(int32 Level, int32 Row, int32 Column)
        {
            switch (Level % 6)
            {
            case 0:  return (Row + Column) % 3 != 2;
            case 1:  return true;
            case 2:  return (Row + Column) % 4 != 3;
            case 3:  return Math::Abs(Column - kBrickColumnCount / 2) <= (kBrickRowCount - Row) / 2 + 1;
            case 4:  return !(Row >= 2 && Row <= 4 && Column >= 4 && Column <= 6);
            default: return (Column % 2 == 0) || (Row % 2 == 0);
            }
        }

        EBrickKind BrickKindFor(FRandom& Rng, int32 Level, int32 Row, int32 Column)
        {
            if (Level >= 2 && Row <= 1 && (Row * 7 + Column * 3 + Level) % 11 == 0)
            {
                return EBrickKind::Steel;
            }
            if (Rng.Unit() < 0.055f + float(Level) * 0.004f)
            {
                return EBrickKind::Explosive;
            }
            if (Rng.Unit() < 0.045f)
            {
                return EBrickKind::Mystery;
            }
            return Row <= 4 ? EBrickKind::Reinforced : EBrickKind::Normal;
        }

        int32 BrickHealthFor(EBrickKind Kind, int32 Row, int32 Level)
        {
            if (Kind == EBrickKind::Steel)
            {
                return 9999;
            }
            if (Kind == EBrickKind::Explosive || Kind == EBrickKind::Mystery)
            {
                return 1;
            }

            const int32 Base = Row <= 1 ? 3 : (Row <= 4 ? 2 : 1);
            return Math::Min(Base + (Level - 1) / 3, 4);
        }

        FVector2 BrickCenter(int32 Row, int32 Column, const FGameState& State)
        {
            return { kBrickLeft + Column * kBrickPitchX + State.FormationDrift,
                     kBrickTop + Row * kBrickPitchY + State.FormationDrop };
        }

        float LowestBrickEdge(ECS::FRegistry& Registry)
        {
            int32 LowestRow = -1;
            for (auto [Entity, Brick] : Registry.View<FBrick>().Each())
            {
                if (Brick.Health > 0)
                {
                    LowestRow = Math::Max(LowestRow, Brick.Row);
                }
            }

            if (LowestRow < 0)
            {
                return 0.0f;
            }

            const FGameState& State = Registry.GetSingleton<FGameState>();
            return kBrickTop + float(LowestRow) * kBrickPitchY + kBrickHeight * 0.5f + State.FormationDrop;
        }

        float VectorLength(const FVector2& Value)
        {
            return Math::Sqrt(Value.x * Value.x + Value.y * Value.y);
        }

        FVector4 LerpColor(const FVector4& A, const FVector4& B, float Alpha)
        {
            return { A.x + (B.x - A.x) * Alpha, A.y + (B.y - A.y) * Alpha,
                     A.z + (B.z - A.z) * Alpha, A.w + (B.w - A.w) * Alpha };
        }

        float PanFor(float FieldX)
        {
            return Math::Clamp(FieldX / kFieldWidth * 2.0f - 1.0f, -1.0f, 1.0f) * 0.65f;
        }

        void PlaySound(ECS::FRegistry& Registry, ESound Sound, float Pitch = 1.0f, float Volume = 1.0f, float Pan = 0.0f)
        {
            FSoundQueue& Queue = Registry.GetSingleton<FSoundQueue>();
            if (Queue.Pending.size() < 96)
            {
                Queue.Pending.push_back(FSoundRequest{ Sound, Pitch, Volume, Pan });
            }
        }

        void AddHitStop(ECS::FRegistry& Registry, float Seconds)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            State.HitStop = Math::Min(Math::Max(State.HitStop, Seconds), kMaxHitStop);
        }


        //~ Spawning.

        void AddTrauma(ECS::FRegistry& Registry, float Amount, float ChromaAmount = 0.0f)
        {
            FCameraShake& Shake = Registry.GetSingleton<FCameraShake>();
            Shake.Trauma = Math::Min(Shake.Trauma + Amount, 1.0f);
            Shake.Chroma = Math::Min(Shake.Chroma + ChromaAmount, 1.0f);
        }

        int32 CountParticles(ECS::FRegistry& Registry)
        {
            return int32(Registry.View<FParticle>().NumCandidates());
        }

        int32 ParticleBudget(ECS::FRegistry& Registry, int32 Wanted)
        {
            const int32 Alive = CountParticles(Registry);
            const float Load = float(Alive) / float(kMaxParticles);
            const float Scale = Load > 0.45f ? Math::Max(0.12f, 1.0f - (Load - 0.45f) * 1.9f) : 1.0f;
            return Math::Clamp(int32(float(Wanted) * Scale), 0, kMaxParticles - Alive);
        }

        void SpawnParticle(ECS::FRegistry& Registry, const FVector2& Position, const FVector2& Velocity,
                           const FVector4& Start, const FVector4& End, float Size, float EndSize, float Life,
                           float Drag, float Gravity, EQuadKind Kind, float Bounce = 0.0f)
        {
            const ECS::FEntity Entity = Registry.Create();

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = Position;
            Body.HalfSize = { Size, Size };
            Body.Rotation = 0.0f;

            FVisual& Visual = Registry.Emplace<FVisual>(Entity);
            Visual.Color  = Start;
            Visual.Accent = Start;
            Visual.Kind   = Kind;
            Visual.Glow   = 1.0f;

            FParticle& Particle = Registry.Emplace<FParticle>(Entity);
            Particle.Velocity  = Velocity;
            Particle.EndColor  = End;
            Particle.MaxLife   = Life;
            Particle.Life      = Life;
            Particle.Drag      = Drag;
            Particle.Gravity   = Gravity;
            Particle.StartSize = Size;
            Particle.EndSize   = EndSize;
            Particle.Bounce    = Bounce;
        }

        void SpawnBurst(ECS::FRegistry& Registry, const FVector2& Position, const FVector4& Color, int32 Count,
                        float SpeedMin, float SpeedMax, float Gravity, float Bounce = 0.0f)
        {
            FRandom& Rng = Registry.GetSingleton<FRandom>();
            const int32 Budget = ParticleBudget(Registry, Count);

            for (int32 i = 0; i < Budget; ++i)
            {
                const float Angle = Rng.Range(0.0f, 6.2831853f);
                const float Speed = Rng.Range(SpeedMin, SpeedMax);
                const FVector2 Velocity { Math::Cos(Angle) * Speed, Math::Sin(Angle) * Speed };

                const float Bright = Rng.Range(0.85f, 1.75f);
                const FVector4 Start { Color.x * Bright, Color.y * Bright, Color.z * Bright, 1.0f };
                const FVector4 End   { Color.x * 0.25f, Color.y * 0.15f, Color.z * 0.35f, 0.0f };

                SpawnParticle(Registry, Position, Velocity, Start, End, Rng.Range(3.5f, 9.0f), 0.4f,
                    Rng.Range(0.35f, 0.95f), Rng.Range(1.1f, 2.4f), Gravity, EQuadKind::Spark, Bounce);
            }
        }

        void SpawnShockwave(ECS::FRegistry& Registry, const FVector2& Position, const FVector4& Color,
                            float MaxRadius, float Duration, float Warp = 0.0f)
        {
            const ECS::FEntity Entity = Registry.Create();

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = Position;
            Body.HalfSize = { 1.0f, 1.0f };
            Body.Rotation = 0.0f;

            FShockwave& Wave = Registry.Emplace<FShockwave>(Entity);
            Wave.MaxRadius = MaxRadius;
            Wave.Duration  = Duration;
            Wave.Color     = Color;
            Wave.Warp      = Warp;
        }

        void SpawnScorePop(ECS::FRegistry& Registry, const FVector2& Position, int32 Value, const FVector4& Color)
        {
            const ECS::FEntity Entity = Registry.Create();
            FScorePop& Pop = Registry.Emplace<FScorePop>(Entity);
            Pop.Position = Position;
            Pop.Value    = Value;
            Pop.Color    = Color;
        }

        ECS::FEntity SpawnBall(ECS::FRegistry& Registry, const FVector2& Position, const FVector2& Velocity,
                               float Speed, float FireTimer = 0.0f)
        {
            const ECS::FEntity Entity = Registry.Create();

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = Position;
            Body.HalfSize = { kBallRadius, kBallRadius };
            Body.Rotation = 0.0f;

            FVisual& Visual = Registry.Emplace<FVisual>(Entity);
            Visual.Color  = { 1.45f, 1.65f, 2.05f, 1.0f };
            Visual.Accent = { 0.18f, 0.45f, 0.85f, 1.0f };
            Visual.Kind   = EQuadKind::Disc;
            Visual.Glow   = 1.0f;

            FBall& Ball = Registry.Emplace<FBall>(Entity);
            Ball.Velocity  = Velocity;
            Ball.Speed     = Speed;
            Ball.FireTimer = FireTimer;
            Ball.bHeld     = false;

            for (int32 Node = 0; Node < kBallTrailNodes; ++Node)
            {
                Ball.Trail[Node] = Position;
            }
            return Entity;
        }

        void SpawnPowerUp(ECS::FRegistry& Registry, const FVector2& Position, EPowerUp Type)
        {
            const ECS::FEntity Entity = Registry.Create();
            const FVector4 Color = PowerUpColor(Type);

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = Position;
            Body.HalfSize = { 30.0f, 30.0f };
            Body.Rotation = 0.0f;

            FVisual& Visual = Registry.Emplace<FVisual>(Entity);
            Visual.Color  = { Color.x * 1.15f, Color.y * 1.15f, Color.z * 1.15f, 1.0f };
            Visual.Accent = { Color.x * 0.22f, Color.y * 0.22f, Color.z * 0.30f, 1.0f };
            Visual.Kind   = EQuadKind::Rect;
            Visual.Glow   = 0.85f;
            Visual.CornerRadius = IsHarmful(Type) ? 0.10f : 0.42f;

            FPowerUpDrop& Drop = Registry.Emplace<FPowerUpDrop>(Entity);
            Drop.Type = Type;
        }

        void SpawnBolt(ECS::FRegistry& Registry, const FVector2& Position, const FVector2& Velocity, bool bHostile)
        {
            const ECS::FEntity Entity = Registry.Create();

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = Position;
            Body.HalfSize = bHostile ? FVector2{ 7.0f, 20.0f } : FVector2{ 4.5f, 26.0f };
            Body.Rotation = bHostile ? 3.14159265f : 0.0f;

            FVisual& Visual = Registry.Emplace<FVisual>(Entity);
            Visual.Color  = bHostile ? FVector4{ 1.90f, 1.05f, 0.25f, 1.0f } : FVector4{ 2.10f, 0.55f, 0.80f, 1.0f };
            Visual.Accent = bHostile ? FVector4{ 2.40f, 1.90f, 0.90f, 1.0f } : FVector4{ 2.60f, 1.60f, 2.00f, 1.0f };
            Visual.Kind   = EQuadKind::Bolt;
            Visual.Glow   = 1.2f;

            FLaserBolt& Bolt = Registry.Emplace<FLaserBolt>(Entity);
            Bolt.Velocity = Velocity;
            Bolt.bHostile = bHostile;
            Bolt.Life = bHostile ? 4.0f : 1.2f;
        }

        void SpawnBoss(ECS::FRegistry& Registry, int32 Level)
        {
            const ECS::FEntity Entity = Registry.Create();

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = { kFieldWidth * 0.5f, kBossTopY };
            Body.HalfSize = { 178.0f, 54.0f };
            Body.Rotation = 0.0f;

            FVisual& Visual = Registry.Emplace<FVisual>(Entity);
            Visual.Color  = { 1.20f, 0.28f, 0.75f, 1.0f };
            Visual.Accent = { 0.28f, 0.05f, 0.22f, 1.0f };
            Visual.Kind   = EQuadKind::Brick;
            Visual.Glow   = 0.85f;
            Visual.CornerRadius = 0.30f;

            FBoss& Boss = Registry.Emplace<FBoss>(Entity);
            Boss.MaxHealth = 26.0f + float(Level) * 5.0f;
            Boss.Health    = Boss.MaxHealth;
        }


        //~ Level lifecycle

        template<typename TComponent>
        void DestroyAllWith(ECS::FRegistry& Registry)
        {
            TVector<ECS::FEntity> Doomed;
            for (const ECS::FEntity Entity : Registry.View<TComponent>())
            {
                Doomed.push_back(Entity);
            }
            for (const ECS::FEntity Entity : Doomed)
            {
                Registry.Destroy(Entity);
            }
        }

        void EnterFever(ECS::FRegistry& Registry);

        void ClearTransients(ECS::FRegistry& Registry)
        {
            DestroyAllWith<FParticle>(Registry);
            DestroyAllWith<FShockwave>(Registry);
            DestroyAllWith<FPowerUpDrop>(Registry);
            DestroyAllWith<FScorePop>(Registry);
            DestroyAllWith<FLaserBolt>(Registry);

            FExplosionQueue& Queue = Registry.GetSingleton<FExplosionQueue>();
            Queue.Pending.clear();
            Queue.Cursor = 0;
        }

        ECS::FEntity FindPaddle(ECS::FRegistry& Registry)
        {
            for (const ECS::FEntity Entity : Registry.View<FPaddle>())
            {
                return Entity;
            }
            return ECS::NullEntity;
        }

        void ApplyBrickVisual(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            const FBrick& Brick = Registry.Get<FBrick>(Entity);
            const FVector4 Base = BrickBaseColor(Brick);

            FVisual& Visual = Registry.Get<FVisual>(Entity);
            Visual.Color  = { Base.x * 0.88f, Base.y * 0.88f, Base.z * 0.88f, 1.0f };
            Visual.Accent = { Base.x * 0.20f, Base.y * 0.20f, Base.z * 0.30f, 1.0f };
            Visual.Kind   = EQuadKind::Brick;
            Visual.Glow   = Brick.Kind == EBrickKind::Explosive ? 0.55f : 0.30f;
            Visual.CornerRadius = Brick.Kind == EBrickKind::Steel ? 0.10f : 0.22f;
        }

        void BuildLevel(ECS::FRegistry& Registry, int32 Level)
        {
            DestroyAllWith<FBrick>(Registry);
            DestroyAllWith<FBoss>(Registry);

            FRandom& Rng = Registry.GetSingleton<FRandom>();
            FGameState& State = Registry.GetSingleton<FGameState>();
            State.BricksAlive = 0;
            State.bBossAlive = Level % 5 == 0;

            const int32 FirstRow = State.bBossAlive ? 3 : 0;
            if (State.bBossAlive)
            {
                SpawnBoss(Registry, Level);
            }

            for (int32 Row = FirstRow; Row < kBrickRowCount; ++Row)
            {
                for (int32 Column = 0; Column < kBrickColumnCount; ++Column)
                {
                    if (!BrickPresent(Level, Row, Column))
                    {
                        continue;
                    }

                    const ECS::FEntity Entity = Registry.Create();

                    FBody& Body = Registry.Emplace<FBody>(Entity);
                    Body.Position = BrickCenter(Row, Column, State);
                    Body.HalfSize = { kBrickWidth * 0.5f, kBrickHeight * 0.5f };
                    Body.Rotation = 0.0f;

                    FBrick& Brick = Registry.Emplace<FBrick>(Entity);
                    Brick.Row       = Row;
                    Brick.Column    = Column;
                    Brick.Kind      = BrickKindFor(Rng, Level, Row, Column);
                    Brick.MaxHealth = BrickHealthFor(Brick.Kind, Row, Level);
                    Brick.Health    = Brick.MaxHealth;
                    Brick.Phase     = float(Row) * 0.45f + float(Column) * 0.22f;

                    const bool bScores = Brick.Kind != EBrickKind::Steel;

                    Registry.Emplace<FVisual>(Entity);
                    ApplyBrickVisual(Registry, Entity);

                    if (bScores)
                    {
                        ++State.BricksAlive;
                    }
                }
            }

            State.BricksTotal = Math::Max(State.BricksAlive, 1);
            State.Progress = 0.0f;
        }

        void ResetBallToPaddle(ECS::FRegistry& Registry)
        {
            DestroyAllWith<FBall>(Registry);

            const ECS::FEntity PaddleEntity = FindPaddle(Registry);
            if (PaddleEntity.IsNull())
            {
                return;
            }

            const float PaddleX = Registry.Get<FBody>(PaddleEntity).Position.x;
            const FVector2 Position { PaddleX, kPaddleY - kPaddleHalfHeight - kBallRadius - 4.0f };

            const ECS::FEntity Entity = SpawnBall(Registry, Position, { 0.0f, 0.0f }, kBallBaseSpeed);
            Registry.Get<FBall>(Entity).bHeld = true;
        }

        void StartLevel(ECS::FRegistry& Registry, int32 Level)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            State.Level      = Level;
            State.Phase      = EPhase::Serve;
            State.PhaseTimer = 0.0f;
            State.Combo      = 0;
            State.SlowTimer  = 0.0f;
            State.FormationDrop  = 0.0f;
            State.FormationDrift = 0.0f;
            State.BreachWarning  = 0.0f;

            ClearTransients(Registry);
            BuildLevel(Registry, Level);

            const ECS::FEntity PaddleEntity = FindPaddle(Registry);
            if (!PaddleEntity.IsNull())
            {
                Registry.Get<FPaddle>(PaddleEntity) = FPaddle{};
                Registry.Get<FBody>(PaddleEntity).HalfSize = { kPaddleBaseHalfWidth, kPaddleHalfHeight };
            }

            ResetBallToPaddle(Registry);
        }

        void StartRun(ECS::FRegistry& Registry)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            State.Score        = 0;
            State.DisplayScore = 0;
            State.Lives        = 3;
            State.BestCombo    = 0;
            State.FeverMeter   = 0.0f;
            State.FeverTimer   = 0.0f;
            StartLevel(Registry, 1);
        }


        //~ Systems

        void PaddleSystem(ECS::FRegistry& Registry, float Delta)
        {
            const FFrameInput& Input = Registry.GetSingleton<FFrameInput>();
            const FGameState& State = Registry.GetSingleton<FGameState>();
            bool bShieldJustReady = false;

            for (auto [Entity, Body, Paddle] : Registry.View<FBody, FPaddle>().Each())
            {
                float Target = Body.Position.x;
                if (Input.bUsingMouse)
                {
                    Target = Input.PaddleTarget;
                }
                else if (Input.KeyAxis != 0.0f)
                {
                    Target = Body.Position.x + Input.KeyAxis * 2600.0f * Delta;
                }

                if (State.Phase == EPhase::Title || State.Phase == EPhase::GameOver)
                {
                    Target = kFieldWidth * 0.5f + Math::Sin(State.Elapsed * 0.9f) * 380.0f;
                }

                Paddle.HalfWidthGoal = Paddle.ShrinkTimer > 0.0f ? kPaddleThinHalfWidth
                                     : (Paddle.WidenTimer > 0.0f ? kPaddleWideHalfWidth : kPaddleBaseHalfWidth);

                Body.HalfSize.x += (Paddle.HalfWidthGoal - Body.HalfSize.x) * Math::Min(1.0f, Delta * 9.0f);
                Body.HalfSize.y = kPaddleHalfHeight;

                const float Clamped = Math::Clamp(Target, Body.HalfSize.x + 8.0f, kFieldWidth - Body.HalfSize.x - 8.0f);
                const float Previous = Body.Position.x;

                Body.Position.x += (Clamped - Body.Position.x) * Math::Min(1.0f, Delta * 26.0f);
                Body.Position.y = kPaddleY;

                Paddle.Velocity = Delta > 0.0f ? (Body.Position.x - Previous) / Delta : 0.0f;
                Paddle.Tilt += (Math::Clamp(Paddle.Velocity * 0.00007f, -0.10f, 0.10f) - Paddle.Tilt) * Math::Min(1.0f, Delta * 12.0f);
                Body.Rotation = Paddle.Tilt;

                Paddle.WidenTimer  = Math::Max(0.0f, Paddle.WidenTimer - Delta);
                Paddle.ShrinkTimer = Math::Max(0.0f, Paddle.ShrinkTimer - Delta);
                Paddle.LaserTimer  = Math::Max(0.0f, Paddle.LaserTimer - Delta);
                Paddle.CatchTimer  = Math::Max(0.0f, Paddle.CatchTimer - Delta);
                Paddle.HitFlash    = Math::Max(0.0f, Paddle.HitFlash - Delta * 4.0f);
                Paddle.ShieldFlash = Math::Max(0.0f, Paddle.ShieldFlash - Delta * 2.2f);

                if (State.Phase == EPhase::Playing || State.Phase == EPhase::Serve)
                {
                    const float Before = Paddle.ShieldCharge;
                    Paddle.ShieldCharge = Math::Min(1.0f, Paddle.ShieldCharge + Delta / kShieldRechargeTime);
                    if (Before < 1.0f && Paddle.ShieldCharge >= 1.0f)
                    {
                        bShieldJustReady = true;
                    }
                }
            }

            if (bShieldJustReady)
            {
                PlaySound(Registry, ESound::ShieldReady, 1.0f, 0.8f);
            }
        }

        void LaserSystem(ECS::FRegistry& Registry, float Delta)
        {
            const ECS::FEntity PaddleEntity = FindPaddle(Registry);
            if (PaddleEntity.IsNull())
            {
                return;
            }

            float LaserTimer = 0.0f;
            {
                FPaddle& Paddle = Registry.Get<FPaddle>(PaddleEntity);
                Paddle.LaserCooldown = Math::Max(0.0f, Paddle.LaserCooldown - Delta);

                if (Paddle.LaserTimer <= 0.0f || Paddle.LaserCooldown > 0.0f)
                {
                    return;
                }

                LaserTimer = Paddle.LaserTimer;
                Paddle.LaserCooldown = kLaserInterval;
            }

            const FVector2 Position = Registry.Get<FBody>(PaddleEntity).Position;
            const float HalfWidth = Registry.Get<FBody>(PaddleEntity).HalfSize.x;

            const FVector2 Up { 0.0f, -kBoltSpeed };
            SpawnBolt(Registry, { Position.x - HalfWidth * 0.82f, Position.y - 26.0f }, Up, false);
            SpawnBolt(Registry, { Position.x + HalfWidth * 0.82f, Position.y - 26.0f }, Up, false);

            const float Pitch = LaserTimer < 3.0f ? 1.18f : 1.0f;
            PlaySound(Registry, ESound::Laser, Pitch * Registry.GetSingleton<FRandom>().Range(0.94f, 1.06f), 0.8f);
        }

        void HeldBallSystem(ECS::FRegistry& Registry)
        {
            const ECS::FEntity PaddleEntity = FindPaddle(Registry);
            if (PaddleEntity.IsNull())
            {
                return;
            }

            const float PaddleX = Registry.Get<FBody>(PaddleEntity).Position.x;

            for (auto [Entity, Body, Ball] : Registry.View<FBody, FBall>().Each())
            {
                if (!Ball.bHeld)
                {
                    continue;
                }
                Body.Position.x = PaddleX + Ball.HeldOffset;
                Body.Position.y = kPaddleY - kPaddleHalfHeight - kBallRadius - 4.0f;
                Body.HalfSize = { kBallRadius, kBallRadius };

                for (int32 Node = 0; Node < kBallTrailNodes; ++Node)
                {
                    Ball.Trail[Node] = Body.Position;
                }
            }
        }

        void LaunchHeldBalls(ECS::FRegistry& Registry)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            FRandom& Rng = Registry.GetSingleton<FRandom>();
            bool bLaunched = false;

            for (auto [Entity, Ball] : Registry.View<FBall>().Each())
            {
                if (!Ball.bHeld)
                {
                    continue;
                }
                const float Angle = Rng.Range(-0.42f, 0.42f);
                Ball.Velocity = { Math::Sin(Angle) * Ball.Speed, -Math::Cos(Angle) * Ball.Speed };
                Ball.bHeld = false;
                Ball.HeldOffset = 0.0f;
                bLaunched = true;
            }

            if (bLaunched)
            {
                PlaySound(Registry, State.Phase == EPhase::Serve ? ESound::Launch : ESound::Release);
            }

            if (State.Phase == EPhase::Serve)
            {
                State.Phase = EPhase::Playing;
            }
        }

        void ApplyPowerUp(ECS::FRegistry& Registry, EPowerUp Type, const FVector2& Position)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            FRandom& Rng = Registry.GetSingleton<FRandom>();
            const ECS::FEntity PaddleEntity = FindPaddle(Registry);

            switch (Type)
            {
            case EPowerUp::Widen:
                if (!PaddleEntity.IsNull())
                {
                    FPaddle& Paddle = Registry.Get<FPaddle>(PaddleEntity);
                    Paddle.WidenTimer = 15.0f;
                    Paddle.ShrinkTimer = 0.0f;
                }
                break;

            case EPowerUp::MultiBall:
            {
                PlaySound(Registry, ESound::MultiBall, 1.0f, 1.0f, PanFor(Position.x));

                TVector<ECS::FEntity> Sources;
                for (const ECS::FEntity Entity : Registry.View<FBall>())
                {
                    Sources.push_back(Entity);
                }

                for (const ECS::FEntity Source : Sources)
                {
                    const FBall Ball = Registry.Get<FBall>(Source);
                    const FVector2 Origin = Registry.Get<FBody>(Source).Position;
                    if (Ball.bHeld)
                    {
                        continue;
                    }

                    for (int32 i = 0; i < 2; ++i)
                    {
                        const float Angle = (i == 0 ? 0.55f : -0.55f) + Rng.Range(-0.12f, 0.12f);
                        const float Cos = Math::Cos(Angle);
                        const float Sin = Math::Sin(Angle);
                        const FVector2 Rotated { Ball.Velocity.x * Cos - Ball.Velocity.y * Sin,
                                                 Ball.Velocity.x * Sin + Ball.Velocity.y * Cos };
                        SpawnBall(Registry, Origin, Rotated, Ball.Speed, Ball.FireTimer);
                    }
                }
                break;
            }

            case EPowerUp::SlowTime:
                State.SlowTimer = 7.0f;
                PlaySound(Registry, ESound::SlowTime, 1.0f, 1.0f, PanFor(Position.x));
                break;

            case EPowerUp::ExtraLife:
                State.Lives = Math::Min(State.Lives + 1, 9);
                PlaySound(Registry, ESound::ExtraLife);
                break;

            case EPowerUp::Laser:
                if (!PaddleEntity.IsNull())
                {
                    Registry.Get<FPaddle>(PaddleEntity).LaserTimer = 12.0f;
                }
                break;

            case EPowerUp::Fireball:
                for (auto [Entity, Ball] : Registry.View<FBall>().Each())
                {
                    Ball.FireTimer = 9.0f;
                }
                State.FireGlow = 1.0f;
                PlaySound(Registry, ESound::FireballStart, 1.0f, 1.0f, PanFor(Position.x));
                break;

            case EPowerUp::Catch:
                if (!PaddleEntity.IsNull())
                {
                    Registry.Get<FPaddle>(PaddleEntity).CatchTimer = 14.0f;
                }
                break;

            case EPowerUp::Shrink:
                if (!PaddleEntity.IsNull())
                {
                    FPaddle& Paddle = Registry.Get<FPaddle>(PaddleEntity);
                    Paddle.ShrinkTimer = 9.0f;
                    Paddle.WidenTimer = 0.0f;
                }
                break;

            default:
                for (auto [Entity, Ball] : Registry.View<FBall>().Each())
                {
                    Ball.Speed = Math::Min(Ball.Speed * 1.35f, kBallMaxSpeed);
                }
                break;
            }

            const FVector4 Color = PowerUpColor(Type);
            SpawnShockwave(Registry, Position, Color, 280.0f, 0.55f, IsHarmful(Type) ? 0.5f : 0.8f);
            SpawnBurst(Registry, Position, Color, 30, 180.0f, 660.0f, 0.0f);
            AddTrauma(Registry, IsHarmful(Type) ? 0.30f : 0.18f, IsHarmful(Type) ? 0.55f : 0.25f);

            PlaySound(Registry, IsHarmful(Type) ? ESound::PowerDown : ESound::PowerUpCollect,
                1.0f, 1.0f, PanFor(Position.x));
        }

        void PowerUpSystem(ECS::FRegistry& Registry, float Delta)
        {
            const ECS::FEntity PaddleEntity = FindPaddle(Registry);
            if (PaddleEntity.IsNull())
            {
                return;
            }

            const FVector2 PaddlePosition = Registry.Get<FBody>(PaddleEntity).Position;
            const FVector2 PaddleHalfSize = Registry.Get<FBody>(PaddleEntity).HalfSize;

            TVector<ECS::FEntity> Collected;
            TVector<ECS::FEntity> Missed;

            for (auto [Entity, Body, Drop, Visual] : Registry.View<FBody, FPowerUpDrop, FVisual>().Each())
            {
                Drop.Bob += Delta;
                Body.Position.y += (IsHarmful(Drop.Type) ? 470.0f : 340.0f) * Delta;
                Body.Rotation = Math::Sin(Drop.Bob * 2.4f) * 0.35f;
                Body.HalfSize = { 30.0f + Math::Sin(Drop.Bob * 7.0f) * 2.5f, 30.0f - Math::Sin(Drop.Bob * 7.0f) * 2.5f };
                Visual.Glow = 1.2f + Math::Sin(Drop.Bob * 9.0f) * 0.4f;

                const bool bTouchesPaddle =
                    Math::Abs(Body.Position.x - PaddlePosition.x) < PaddleHalfSize.x + Body.HalfSize.x &&
                    Math::Abs(Body.Position.y - PaddlePosition.y) < PaddleHalfSize.y + Body.HalfSize.y;

                if (bTouchesPaddle)
                {
                    Collected.push_back(Entity);
                }
                else if (Body.Position.y > kFieldHeight + 80.0f)
                {
                    Missed.push_back(Entity);
                }
            }

            for (const ECS::FEntity Entity : Collected)
            {
                const EPowerUp Type = Registry.Get<FPowerUpDrop>(Entity).Type;
                const FVector2 Position = Registry.Get<FBody>(Entity).Position;
                Registry.Destroy(Entity);

                ApplyPowerUp(Registry, Type, Position);

                if (!IsHarmful(Type))
                {
                    Registry.GetSingleton<FGameState>().Score += 250;
                    SpawnScorePop(Registry, Position, 250, PowerUpColor(Type));
                }
            }

            for (const ECS::FEntity Entity : Missed)
            {
                Registry.Destroy(Entity);
            }
        }

        EPowerUp RollPowerUp(FRandom& Rng)
        {
            const int32 Roll = int32(Rng.Range(0.0f, 99.99f));
            if (Roll < 14) { return EPowerUp::Widen; }
            if (Roll < 28) { return EPowerUp::MultiBall; }
            if (Roll < 42) { return EPowerUp::Laser; }
            if (Roll < 54) { return EPowerUp::Fireball; }
            if (Roll < 64) { return EPowerUp::Catch; }
            if (Roll < 74) { return EPowerUp::SlowTime; }
            if (Roll < 78) { return EPowerUp::ExtraLife; }
            if (Roll < 89) { return EPowerUp::Shrink; }
            return EPowerUp::SpeedUp;
        }

        void MaybeDropPowerUp(ECS::FRegistry& Registry, const FVector2& Position, bool bGuaranteed)
        {
            FRandom& Rng = Registry.GetSingleton<FRandom>();
            if (!bGuaranteed && Rng.Unit() > 0.16f)
            {
                return;
            }

            EPowerUp Type = RollPowerUp(Rng);
            for (int32 Attempt = 0; bGuaranteed && IsHarmful(Type) && Attempt < 8; ++Attempt)
            {
                Type = RollPowerUp(Rng);
            }

            SpawnPowerUp(Registry, Position, Type);
            PlaySound(Registry, ESound::PowerUpDrop, 1.0f, 1.0f, PanFor(Position.x));
        }

        void DamageBrick(ECS::FRegistry& Registry, ECS::FEntity Entity, int32 Damage, bool bOverkill);

        void ResolveExplosions(ECS::FRegistry& Registry)
        {
            FExplosionQueue& Queue = Registry.GetSingleton<FExplosionQueue>();
            if (Queue.Cursor >= int32(Queue.Pending.size()))
            {
                Queue.Pending.clear();
                Queue.Cursor = 0;
                return;
            }

            TVector<FVector2> Batch;
            const int32 Last = Math::Min(Queue.Cursor + kExplosionsPerStep,
                Math::Min(int32(Queue.Pending.size()), kMaxChainStages));

            for (int32 Index = Queue.Cursor; Index < Last; ++Index)
            {
                Batch.push_back(Queue.Pending[Index]);
            }
            Queue.Cursor = Last;

            TVector<ECS::FEntity> Caught;

            for (const FVector2& Origin : Batch)
            {

                Caught.clear();
                for (auto [Entity, Body, Brick] : Registry.View<FBody, FBrick>().Each())
                {
                    if (Brick.Health > 0 && VectorLength(Body.Position - Origin) <= kExplosionRadius)
                    {
                        Caught.push_back(Entity);
                    }
                }

                SpawnShockwave(Registry, Origin, kFireColor, kExplosionRadius * 2.1f, 0.5f, 1.0f);
                SpawnBurst(Registry, Origin, kFireColor, 48, 220.0f, 1500.0f, 900.0f, 0.35f);
                SpawnBurst(Registry, Origin, { 1.0f, 0.85f, 0.35f, 1.0f }, 20, 90.0f, 520.0f, 200.0f);
                AddTrauma(Registry, 0.42f, 0.55f);
                AddHitStop(Registry, 0.038f);
                PlaySound(Registry, ESound::Explosion, Registry.GetSingleton<FRandom>().Range(0.9f, 1.1f), 1.0f, PanFor(Origin.x));

                for (const ECS::FEntity Entity : Caught)
                {
                    if (Registry.IsValid(Entity) && Registry.HasAll<FBrick>(Entity))
                    {
                        DamageBrick(Registry, Entity, 9999, true);
                    }
                }
            }
        }

        void DamageBrick(ECS::FRegistry& Registry, ECS::FEntity Entity, int32 Damage, bool bOverkill)
        {
            int32 Row = 0;
            int32 MaxHealth = 1;
            EBrickKind Kind = EBrickKind::Normal;
            bool bDestroyed = false;
            FVector2 Position { 0.0f, 0.0f };
            FVector4 Base { 1.0f, 1.0f, 1.0f, 1.0f };
            FVector4 PopColor { 1.0f, 1.0f, 1.0f, 1.0f };

            {
                FBrick& Brick = Registry.Get<FBrick>(Entity);
                Row       = Brick.Row;
                MaxHealth = Brick.MaxHealth;
                Kind      = Brick.Kind;
                Base      = BrickBaseColor(Brick);
                Position  = Registry.Get<FBody>(Entity).Position;
                PopColor  = Registry.Get<FVisual>(Entity).Color;

                if (Kind == EBrickKind::Steel && !bOverkill)
                {
                    Brick.Flash = 1.0f;
                    Brick.Shove = 1.0f;
                    PlaySound(Registry, ESound::SteelHit, 1.0f, 1.0f, PanFor(Position.x));
                    SpawnBurst(Registry, Position, { 0.75f, 0.82f, 1.00f, 1.0f }, 10, 140.0f, 520.0f, 900.0f, 0.3f);
                    AddTrauma(Registry, 0.07f);
                    return;
                }

                Brick.Health -= Damage;
                Brick.Flash = 1.0f;
                Brick.Shove = 1.0f;
                bDestroyed = Brick.Health <= 0;
            }

            FGameState& State = Registry.GetSingleton<FGameState>();
            State.Combo += 1;
            State.BestCombo = Math::Max(State.BestCombo, State.Combo);
            State.ComboTimer = 2.4f;

            const int32 Multiplier = Math::Max(1, State.Combo / 3 + 1) * State.ScoreScale();
            const float RowPitch = 1.0f + float(kBrickRowCount - 1 - Row) * 0.085f;

            if (!bDestroyed)
            {
                PlaySound(Registry, ESound::BrickHit, RowPitch, 1.0f, PanFor(Position.x));
                State.Score += 25 * Multiplier;
                SpawnBurst(Registry, Position, Base, 10, 120.0f, 480.0f, 900.0f, 0.25f);
                AddTrauma(Registry, 0.05f);
                ApplyBrickVisual(Registry, Entity);
                return;
            }

            const int32 Points = 100 * Math::Min(MaxHealth, 4) * Multiplier;
            State.Score += Points;
            State.BricksAlive = Math::Max(0, State.BricksAlive - 1);
            State.FlashPulse = Math::Min(1.0f, State.FlashPulse + 0.35f);
            State.Progress = 1.0f - float(State.BricksAlive) / float(Math::Max(State.BricksTotal, 1));

            const bool bFeverReady = !State.IsFever() && State.FeverMeter + kFeverPerBrick >= 1.0f;
            if (!State.IsFever())
            {
                State.FeverMeter = Math::Min(1.0f, State.FeverMeter + kFeverPerBrick);
            }

            Registry.Destroy(Entity);

            if (bFeverReady)
            {
                EnterFever(Registry);
            }

            SpawnScorePop(Registry, Position, Points, PopColor);
            SpawnBurst(Registry, Position, Base, 34, 140.0f, 900.0f, 1100.0f, 0.30f);
            SpawnShockwave(Registry, Position, Base, 200.0f + MaxHealth * 40.0f, 0.42f, 0.35f);
            AddTrauma(Registry, 0.16f + Math::Min(MaxHealth, 4) * 0.03f, 0.18f);
            AddHitStop(Registry, 0.014f);

            if (State.Combo >= 3 && State.Combo % 3 == 0)
            {
                const float ComboPitch = 1.0f + float(Math::Min(State.Combo, 24)) * 0.045f;
                PlaySound(Registry, ESound::ComboUp, ComboPitch, 0.9f, PanFor(Position.x));
            }

            if (Kind == EBrickKind::Explosive)
            {
                Registry.GetSingleton<FExplosionQueue>().Pending.push_back(Position);
                return;
            }

            PlaySound(Registry, ESound::BrickBreak, RowPitch, 1.0f, PanFor(Position.x));
            MaybeDropPowerUp(Registry, Position, Kind == EBrickKind::Mystery);
        }

        void EnterFever(ECS::FRegistry& Registry)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            State.FeverTimer = kFeverDuration;
            State.FeverMeter = 1.0f;

            const FVector2 Center { kFieldWidth * 0.5f, kFieldHeight * 0.55f };
            SpawnShockwave(Registry, Center, { 1.60f, 0.45f, 1.30f, 1.0f }, 1400.0f, 0.9f, 0.9f);
            SpawnBurst(Registry, Center, { 1.50f, 0.55f, 1.40f, 1.0f }, 70, 260.0f, 1500.0f, 220.0f, 0.3f);
            AddTrauma(Registry, 0.45f, 0.7f);
            PlaySound(Registry, ESound::FeverStart);
        }

        void DamageBoss(ECS::FRegistry& Registry, ECS::FEntity Entity, float Damage)
        {
            float Remaining = 0.0f;
            float MaxHealth = 1.0f;
            FVector2 Position { 0.0f, 0.0f };

            {
                FBoss& Boss = Registry.Get<FBoss>(Entity);
                Boss.Health -= Damage;
                Boss.Flash = 1.0f;
                Remaining = Boss.Health;
                MaxHealth = Boss.MaxHealth;
                Position = Registry.Get<FBody>(Entity).Position;
            }

            FGameState& State = Registry.GetSingleton<FGameState>();

            if (Remaining > 0.0f)
            {
                State.Score += 60 * State.ScoreScale();
                SpawnBurst(Registry, Position, { 1.40f, 0.35f, 0.90f, 1.0f }, 16, 160.0f, 700.0f, 700.0f, 0.3f);
                AddTrauma(Registry, 0.08f);
                PlaySound(Registry, ESound::BossHit, 1.0f + (1.0f - Remaining / MaxHealth) * 0.35f, 1.0f,
                    PanFor(Position.x));
                return;
            }

            Registry.Destroy(Entity);
            State.bBossAlive = false;
            State.Score += 5000 * State.ScoreScale();
            SpawnScorePop(Registry, Position, 5000 * State.ScoreScale(), { 1.60f, 0.50f, 1.30f, 1.0f });

            for (int32 Ring = 0; Ring < 4; ++Ring)
            {
                SpawnShockwave(Registry, Position, { 1.50f, 0.40f, 1.10f, 1.0f },
                    420.0f + float(Ring) * 260.0f, 0.75f, 1.0f);
            }
            SpawnBurst(Registry, Position, { 1.50f, 0.45f, 1.20f, 1.0f }, 120, 260.0f, 1900.0f, 900.0f, 0.4f);
            AddTrauma(Registry, 1.0f, 1.0f);
            AddHitStop(Registry, 0.075f);
            PlaySound(Registry, ESound::BossDeath);
        }

        void BossSystem(ECS::FRegistry& Registry, float Delta)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            FRandom& Rng = Registry.GetSingleton<FRandom>();

            TVector<FVector2> Shots;

            for (auto [Entity, Body, Boss, Visual] : Registry.View<FBody, FBoss, FVisual>().Each())
            {
                Boss.DriftPhase += Delta * 0.55f;
                Boss.Flash = Math::Max(0.0f, Boss.Flash - Delta * 3.0f);

                const float Health = Math::Clamp(Boss.Health / Math::Max(Boss.MaxHealth, 1.0f), 0.0f, 1.0f);
                const float Reach = 520.0f + (1.0f - Health) * 180.0f;

                Body.Position.x = kFieldWidth * 0.5f + Math::Sin(Boss.DriftPhase) * Reach;
                Body.Position.y = kBossTopY + Math::Sin(Boss.DriftPhase * 1.7f) * 26.0f + State.FormationDrop * 0.5f;

                Visual.Color = { 1.20f + Boss.Flash * 1.4f, 0.28f + Boss.Flash * 1.4f,
                                 0.75f + Boss.Flash * 1.4f, 1.0f };
                Visual.Glow = 0.85f + Boss.Flash * 1.6f + (1.0f - Health) * 0.5f;

                Boss.FireTimer -= Delta;
                if (Boss.FireTimer <= 0.0f)
                {
                    Boss.FireTimer = Math::Max(0.55f, 1.9f - (1.0f - Health) * 1.1f);
                    Shots.push_back({ Body.Position.x, Body.Position.y + Body.HalfSize.y });
                }
            }

            for (const FVector2& Origin : Shots)
            {
                const int32 Count = 1 + int32(Rng.Range(0.0f, 2.4f));
                for (int32 Index = 0; Index < Count; ++Index)
                {
                    const float Spread = (float(Index) - float(Count - 1) * 0.5f) * 70.0f;
                    SpawnBolt(Registry, { Origin.x + Spread, Origin.y },
                        { Rng.Range(-90.0f, 90.0f), 540.0f + Rng.Range(0.0f, 200.0f) }, true);
                }
                PlaySound(Registry, ESound::Hazard, Rng.Range(0.9f, 1.1f), 0.9f, PanFor(Origin.x));
            }
        }

        void BoltSystem(ECS::FRegistry& Registry, float Delta)
        {
            const ECS::FEntity PaddleEntity = FindPaddle(Registry);
            const FVector2 PaddlePosition = PaddleEntity.IsNull()
                ? FVector2{ -1000.0f, -1000.0f } : Registry.Get<FBody>(PaddleEntity).Position;
            const FVector2 PaddleHalfSize = PaddleEntity.IsNull()
                ? FVector2{ 0.0f, 0.0f } : Registry.Get<FBody>(PaddleEntity).HalfSize;

            TVector<ECS::FEntity> Doomed;
            TVector<ECS::FEntity> Struck;
            TVector<ECS::FEntity> BossHits;
            bool bPaddleStruck = false;

            for (auto [Entity, Body, Bolt] : Registry.View<FBody, FLaserBolt>().Each())
            {
                Bolt.Life -= Delta;
                Body.Position += Bolt.Velocity * Delta;

                if (Bolt.Life <= 0.0f || Body.Position.y < -40.0f || Body.Position.y > kFieldHeight + 60.0f)
                {
                    Doomed.push_back(Entity);
                    continue;
                }

                if (Bolt.bHostile)
                {
                    if (Math::Abs(Body.Position.x - PaddlePosition.x) < PaddleHalfSize.x + Body.HalfSize.x &&
                        Math::Abs(Body.Position.y - PaddlePosition.y) < PaddleHalfSize.y + Body.HalfSize.y)
                    {
                        bPaddleStruck = true;
                        Doomed.push_back(Entity);
                    }
                    continue;
                }

                bool bConsumed = false;
                for (auto [BossEntity, BossBody, Boss] : Registry.View<FBody, FBoss>().Each())
                {
                    const FVector2 Offset = Body.Position - BossBody.Position;
                    if (Math::Abs(Offset.x) < BossBody.HalfSize.x + Body.HalfSize.x &&
                        Math::Abs(Offset.y) < BossBody.HalfSize.y + Body.HalfSize.y)
                    {
                        BossHits.push_back(BossEntity);
                        Doomed.push_back(Entity);
                        bConsumed = true;
                        break;
                    }
                }

                if (bConsumed)
                {
                    continue;
                }

                for (auto [BrickEntity, BrickBody, Brick] : Registry.View<FBody, FBrick>().Each())
                {
                    if (Brick.Health <= 0)
                    {
                        continue;
                    }

                    const FVector2 Offset = Body.Position - BrickBody.Position;
                    if (Math::Abs(Offset.x) < BrickBody.HalfSize.x + Body.HalfSize.x &&
                        Math::Abs(Offset.y) < BrickBody.HalfSize.y + Body.HalfSize.y)
                    {
                        Struck.push_back(BrickEntity);
                        Doomed.push_back(Entity);
                        break;
                    }
                }
            }

            for (const ECS::FEntity Entity : Doomed)
            {
                if (!Registry.IsValid(Entity))
                {
                    continue;
                }
                const FVector2 Position = Registry.Get<FBody>(Entity).Position;
                Registry.Destroy(Entity);
                SpawnBurst(Registry, Position, { 1.0f, 0.35f, 0.55f, 1.0f }, 7, 90.0f, 380.0f, 400.0f);
            }

            for (const ECS::FEntity Entity : Struck)
            {
                if (Registry.IsValid(Entity) && Registry.HasAll<FBrick>(Entity))
                {
                    DamageBrick(Registry, Entity, 1, false);
                }
            }

            for (const ECS::FEntity Entity : BossHits)
            {
                if (Registry.IsValid(Entity) && Registry.HasAll<FBoss>(Entity))
                {
                    DamageBoss(Registry, Entity, 1.0f);
                }
            }

            if (bPaddleStruck && !PaddleEntity.IsNull())
            {
                FPaddle& Paddle = Registry.Get<FPaddle>(PaddleEntity);
                const bool bAbsorbed = Paddle.ShieldCharge >= 1.0f;

                Paddle.ShieldCharge = 0.0f;
                Paddle.ShieldFlash = 1.0f;
                if (!bAbsorbed)
                {
                    Paddle.ShrinkTimer = Math::Max(Paddle.ShrinkTimer, 6.0f);
                    Paddle.WidenTimer = 0.0f;
                }

                SpawnBurst(Registry, PaddlePosition, { 1.60f, 0.90f, 0.25f, 1.0f }, 24, 160.0f, 700.0f, 400.0f);
                AddTrauma(Registry, 0.32f, 0.45f);
                PlaySound(Registry, bAbsorbed ? ESound::ShieldSave : ESound::PowerDown);
            }
        }

        // Local copies, so the ball state written back is the state this step actually simulated.
        void BallSystem(ECS::FRegistry& Registry, float Delta)
        {
            const ECS::FEntity PaddleEntity = FindPaddle(Registry);
            if (PaddleEntity.IsNull())
            {
                return;
            }

            const FVector2 PaddlePosition = Registry.Get<FBody>(PaddleEntity).Position;
            const FVector2 PaddleHalfSize = Registry.Get<FBody>(PaddleEntity).HalfSize;
            const float PaddleVelocity = Registry.Get<FPaddle>(PaddleEntity).Velocity;
            const bool bCatchArmed = Registry.Get<FPaddle>(PaddleEntity).CatchTimer > 0.0f;

            FRandom& Rng = Registry.GetSingleton<FRandom>();

            TVector<ECS::FEntity> Balls;
            for (const ECS::FEntity Entity : Registry.View<FBall>())
            {
                Balls.push_back(Entity);
            }

            TVector<ECS::FEntity> DoomedBricks;
            TVector<ECS::FEntity> BossTargets;
            TVector<ECS::FEntity> LostBalls;
            bool bPaddleWasHit = false;

            for (const ECS::FEntity Entity : Balls)
            {
                FBall Ball = Registry.Get<FBall>(Entity);
                if (Ball.bHeld)
                {
                    continue;
                }

                FVector2 Position = Registry.Get<FBody>(Entity).Position;
                FVector2 Velocity = Ball.Velocity;

                Ball.Squash = Math::Max(0.0f, Ball.Squash - Delta * 6.0f);
                Ball.FireTimer = Math::Max(0.0f, Ball.FireTimer - Delta);
                Ball.Spin *= 1.0f - Math::Min(1.0f, Delta * 0.9f);

                Velocity.x += Ball.Spin * Delta;

                const float Travel = Math::Max(Math::Abs(Velocity.x), Math::Abs(Velocity.y)) * Delta;
                const int32 Steps = Math::Clamp(int32(Travel / (kBallRadius * 0.55f)) + 1, 1, 10);
                const float StepDelta = Delta / float(Steps);
                bool bCaught = false;

                for (int32 Step = 0; Step < Steps; ++Step)
                {
                    Position += Velocity * StepDelta;

                    bool bHitWall = false;
                    if (Position.x < kBallRadius && Velocity.x < 0.0f)
                    {
                        Position.x = kBallRadius;
                        Velocity.x = -Velocity.x;
                        bHitWall = true;
                    }
                    else if (Position.x > kFieldWidth - kBallRadius && Velocity.x > 0.0f)
                    {
                        Position.x = kFieldWidth - kBallRadius;
                        Velocity.x = -Velocity.x;
                        bHitWall = true;
                    }

                    if (Position.y < kBallRadius && Velocity.y < 0.0f)
                    {
                        Position.y = kBallRadius;
                        Velocity.y = -Velocity.y;
                        bHitWall = true;
                    }

                    if (bHitWall)
                    {
                        Ball.Squash = 1.0f;
                        Ball.Spin = 0.0f;
                        PlaySound(Registry, ESound::WallHit, Rng.Range(0.92f, 1.10f), 1.0f, PanFor(Position.x));
                        SpawnBurst(Registry, Position, { 0.45f, 0.90f, 1.00f, 1.0f }, 9, 100.0f, 460.0f, 700.0f);
                        AddTrauma(Registry, 0.06f);
                    }

                    const bool bHitsPaddle =
                        Velocity.y > 0.0f &&
                        Math::Abs(Position.x - PaddlePosition.x) < PaddleHalfSize.x + kBallRadius &&
                        Math::Abs(Position.y - PaddlePosition.y) < PaddleHalfSize.y + kBallRadius;

                    if (bHitsPaddle)
                    {
                        Position.y = PaddlePosition.y - PaddleHalfSize.y - kBallRadius;

                        const float Offset = Math::Clamp((Position.x - PaddlePosition.x) / PaddleHalfSize.x, -1.0f, 1.0f);
                        const float Angle = Math::Clamp(Offset * 1.0996f + PaddleVelocity * 0.00012f, -1.2217f, 1.2217f);

                        Ball.Speed = Math::Min(Ball.Speed + 14.0f, kBallMaxSpeed);
                        Velocity = { Math::Sin(Angle) * Ball.Speed, -Math::Cos(Angle) * Ball.Speed };
                        Ball.Spin = Math::Clamp(PaddleVelocity * 0.55f, -900.0f, 900.0f);
                        Ball.Squash = 1.0f;

                        bPaddleWasHit = true;
                        Registry.GetSingleton<FGameState>().Combo = 0;
                        AddTrauma(Registry, 0.07f);
                        SpawnBurst(Registry, Position, { 0.40f, 0.95f, 1.00f, 1.0f }, 14, 140.0f, 540.0f, 600.0f);
                        SpawnShockwave(Registry, Position, { 0.40f, 0.90f, 1.00f, 1.0f }, 130.0f, 0.30f, 0.18f);

                        if (bCatchArmed)
                        {
                            bCaught = true;
                            Ball.HeldOffset = Math::Clamp(Position.x - PaddlePosition.x,
                                -PaddleHalfSize.x * 0.85f, PaddleHalfSize.x * 0.85f);
                            PlaySound(Registry, ESound::Catch, 1.0f, 1.0f, PanFor(Position.x));
                            break;
                        }

                        PlaySound(Registry, ESound::PaddleHit, Rng.Range(0.94f, 1.08f), 1.0f, PanFor(Position.x));
                    }

                    ECS::FEntity BossStruck = ECS::NullEntity;
                    for (auto [BossEntity, BossBody, Boss] : Registry.View<FBody, FBoss>().Each())
                    {
                        const FVector2 Offset = Position - BossBody.Position;
                        const float OverlapX = BossBody.HalfSize.x + kBallRadius - Math::Abs(Offset.x);
                        const float OverlapY = BossBody.HalfSize.y + kBallRadius - Math::Abs(Offset.y);
                        if (OverlapX <= 0.0f || OverlapY <= 0.0f)
                        {
                            continue;
                        }

                        if (OverlapX < OverlapY)
                        {
                            const float Sign = Offset.x >= 0.0f ? 1.0f : -1.0f;
                            Position.x += OverlapX * Sign;
                            Velocity.x = Math::Abs(Velocity.x) * Sign;
                        }
                        else
                        {
                            const float Sign = Offset.y >= 0.0f ? 1.0f : -1.0f;
                            Position.y += OverlapY * Sign;
                            Velocity.y = Math::Abs(Velocity.y) * Sign;
                        }

                        BossStruck = BossEntity;
                        break;
                    }

                    if (!BossStruck.IsNull())
                    {
                        BossTargets.push_back(BossStruck);
                        Ball.Squash = 1.0f;
                        break;
                    }

                    ECS::FEntity Struck = ECS::NullEntity;
                    bool bPassThrough = false;

                    for (auto [BrickEntity, BrickBody, Brick] : Registry.View<FBody, FBrick>().Each())
                    {
                        if (Brick.Health <= 0)
                        {
                            continue;
                        }

                        const FVector2 Offset = Position - BrickBody.Position;
                        const float OverlapX = BrickBody.HalfSize.x + kBallRadius - Math::Abs(Offset.x);
                        const float OverlapY = BrickBody.HalfSize.y + kBallRadius - Math::Abs(Offset.y);
                        if (OverlapX <= 0.0f || OverlapY <= 0.0f)
                        {
                            continue;
                        }

                        bPassThrough = Ball.FireTimer > 0.0f;
                        if (!bPassThrough)
                        {
                            if (OverlapX < OverlapY)
                            {
                                const float Sign = Offset.x >= 0.0f ? 1.0f : -1.0f;
                                Position.x += OverlapX * Sign;
                                Velocity.x = Math::Abs(Velocity.x) * Sign;
                            }
                            else
                            {
                                const float Sign = Offset.y >= 0.0f ? 1.0f : -1.0f;
                                Position.y += OverlapY * Sign;
                                Velocity.y = Math::Abs(Velocity.y) * Sign;
                            }
                        }

                        Struck = BrickEntity;
                        break;
                    }

                    if (!Struck.IsNull())
                    {
                        DoomedBricks.push_back(Struck);
                        Ball.Speed = Math::Min(Ball.Speed + 6.0f, kBallMaxSpeed);
                        Ball.Squash = 1.0f;
                        Velocity = Velocity * (Ball.Speed / Math::Max(VectorLength(Velocity), 1.0f));

                        if (!bPassThrough)
                        {
                            break;
                        }
                    }

                    if (Position.y > kFieldHeight + kBallRadius * 3.0f)
                    {
                        break;
                    }
                }

                // A near-horizontal path never clears a row, so bend it back toward vertical.
                if (Math::Abs(Velocity.y) < Ball.Speed * 0.22f)
                {
                    Velocity.y = (Velocity.y >= 0.0f ? 1.0f : -1.0f) * Ball.Speed * 0.22f;
                    Velocity = Velocity * (Ball.Speed / Math::Max(VectorLength(Velocity), 1.0f));
                }

                Ball.Velocity = Velocity;
                Ball.bHeld = bCaught;

                Ball.GhostTimer += Delta;
                while (Ball.GhostTimer > 0.022f)
                {
                    Ball.GhostTimer -= 0.022f;
                    for (int32 Node = kBallTrailNodes - 1; Node > 0; --Node)
                    {
                        Ball.Trail[Node] = Ball.Trail[Node - 1];
                    }
                    Ball.Trail[0] = Position;
                    Ball.TrailCount = uint8(Math::Min<int32>(Ball.TrailCount + 1, kBallTrailNodes));
                }

                const bool bFiery = Ball.FireTimer > 0.0f;
                const float TrailStep = bFiery ? 0.0035f : 0.006f;

                Ball.TrailBudget += Delta;
                while (Ball.TrailBudget > TrailStep)
                {
                    Ball.TrailBudget -= TrailStep;
                    if (ParticleBudget(Registry, 1) <= 0)
                    {
                        break;
                    }

                    const FVector2 Jitter { Rng.Range(-4.0f, 4.0f), Rng.Range(-4.0f, 4.0f) };
                    if (bFiery)
                    {
                        SpawnParticle(Registry, Position + Jitter, Velocity * -0.05f + FVector2{ 0.0f, -70.0f },
                            { 1.90f, 0.85f, 0.25f, 1.0f }, { 0.55f, 0.05f, 0.02f, 0.0f },
                            kBallRadius * 1.25f, 0.0f, 0.42f, 2.2f, -220.0f, EQuadKind::Glow);
                    }
                    else
                    {
                        SpawnParticle(Registry, Position + Jitter, Velocity * -0.06f,
                            { 0.60f, 1.00f, 1.55f, 1.0f }, { 0.18f, 0.05f, 0.35f, 0.0f },
                            kBallRadius * 0.95f, 0.0f, 0.30f, 3.0f, 0.0f, EQuadKind::Glow);
                    }
                }

                const float Stretch = 1.0f + Math::Min(VectorLength(Velocity) / kBallMaxSpeed, 1.0f) * 0.22f - Ball.Squash * 0.35f;

                Registry.Get<FBall>(Entity) = Ball;

                FBody& Body = Registry.Get<FBody>(Entity);
                Body.Position = Position;
                Body.Rotation = Math::Atan2(Velocity.y, Velocity.x);
                Body.HalfSize = { kBallRadius * Stretch, kBallRadius * (2.0f - Stretch) };

                FVisual& Visual = Registry.Get<FVisual>(Entity);
                Visual.Glow   = (bFiery ? 1.5f : 1.0f) + Ball.Squash * 0.7f;
                Visual.Color  = bFiery ? FVector4{ 2.10f, 1.05f, 0.35f, 1.0f } : FVector4{ 1.45f, 1.65f, 2.05f, 1.0f };
                Visual.Accent = bFiery ? FVector4{ 0.85f, 0.16f, 0.03f, 1.0f } : FVector4{ 0.18f, 0.45f, 0.85f, 1.0f };

                if (Position.y > kFieldHeight + kBallRadius * 3.0f)
                {
                    LostBalls.push_back(Entity);
                }
            }

            if (bPaddleWasHit)
            {
                Registry.Get<FPaddle>(PaddleEntity).HitFlash = 1.0f;
            }

            for (const ECS::FEntity Entity : DoomedBricks)
            {
                if (Registry.IsValid(Entity) && Registry.HasAll<FBrick>(Entity))
                {
                    DamageBrick(Registry, Entity, 1, false);
                }
            }

            for (const ECS::FEntity Entity : BossTargets)
            {
                if (Registry.IsValid(Entity) && Registry.HasAll<FBoss>(Entity))
                {
                    DamageBoss(Registry, Entity, 1.0f);
                }
            }

            for (const ECS::FEntity Entity : LostBalls)
            {
                const float LostX = Registry.Get<FBody>(Entity).Position.x;
                Registry.Destroy(Entity);
                SpawnBurst(Registry, { LostX, kFieldHeight - 6.0f }, { 1.0f, 0.25f, 0.35f, 1.0f }, 30, 200.0f, 720.0f, -400.0f);
            }
        }

        void ParticleSystem(ECS::FRegistry& Registry, float Delta)
        {
            TVector<ECS::FEntity> Doomed;

            for (auto [Entity, Body, Particle, Visual] : Registry.View<FBody, FParticle, FVisual>().Each())
            {
                Particle.Life -= Delta;
                if (Particle.Life <= 0.0f)
                {
                    Doomed.push_back(Entity);
                    continue;
                }

                Particle.Velocity.y += Particle.Gravity * Delta;
                Particle.Velocity -= Particle.Velocity * Math::Min(1.0f, Particle.Drag * Delta);
                Body.Position += Particle.Velocity * Delta;

                if (Particle.Bounce > 0.0f && Body.Position.y > kFieldHeight - 4.0f && Particle.Velocity.y > 0.0f)
                {
                    Body.Position.y = kFieldHeight - 4.0f;
                    Particle.Velocity.y = -Particle.Velocity.y * Particle.Bounce;
                    Particle.Velocity.x *= 0.82f;
                }

                const float Alpha = 1.0f - Particle.Life / Particle.MaxLife;
                const float Size = Math::Lerp(Particle.StartSize, Particle.EndSize, Alpha);
                Body.HalfSize = { Size, Size };
                Body.Rotation += Particle.Spin * Delta;

                Visual.Color = LerpColor(Visual.Accent, Particle.EndColor, Alpha);
            }

            for (const ECS::FEntity Entity : Doomed)
            {
                Registry.Destroy(Entity);
            }
        }

        void ShockwaveSystem(ECS::FRegistry& Registry, float Delta)
        {
            TVector<ECS::FEntity> Doomed;

            for (auto [Entity, Wave] : Registry.View<FShockwave>().Each())
            {
                Wave.Age += Delta;
                if (Wave.Age >= Wave.Duration)
                {
                    Doomed.push_back(Entity);
                }
            }

            for (const ECS::FEntity Entity : Doomed)
            {
                Registry.Destroy(Entity);
            }
        }

        void ScorePopSystem(ECS::FRegistry& Registry, float Delta)
        {
            TVector<ECS::FEntity> Doomed;

            for (auto [Entity, Pop] : Registry.View<FScorePop>().Each())
            {
                Pop.Age += Delta;
                Pop.Position.y -= 80.0f * Delta;
                if (Pop.Age >= Pop.Duration)
                {
                    Doomed.push_back(Entity);
                }
            }

            for (const ECS::FEntity Entity : Doomed)
            {
                Registry.Destroy(Entity);
            }
        }

        void BrickIdleSystem(ECS::FRegistry& Registry, float Delta)
        {
            const FGameState& State = Registry.GetSingleton<FGameState>();

            for (auto [Entity, Body, Brick, Visual] : Registry.View<FBody, FBrick, FVisual>().Each())
            {
                Brick.Flash = Math::Max(0.0f, Brick.Flash - Delta * 3.4f);
                Brick.Shove = Math::Max(0.0f, Brick.Shove - Delta * 5.0f);

                const FVector2 Rest = BrickCenter(Brick.Row, Brick.Column, State);
                const float Wave = Math::Sin(State.Elapsed * 1.6f + Brick.Phase) * 2.4f;
                const float Panic = State.Progress * Math::Sin(State.Elapsed * 7.0f + Brick.Phase * 2.0f) * 3.5f;
                Body.Position = { Rest.x, Rest.y + Wave + Panic };

                const float Damage = 1.0f - float(Brick.Health) / float(Math::Max(1, Brick.MaxHealth));
                const FVector4 Base = BrickBaseColor(Brick);
                const float Dim = 0.88f - (Brick.Kind == EBrickKind::Steel ? 0.0f : Damage * 0.30f);
                const float Flash = Brick.Flash * 1.1f;

                float Pulse = 0.0f;
                if (Brick.Kind == EBrickKind::Explosive)
                {
                    Pulse = 0.22f + 0.22f * Math::Sin(State.Elapsed * 6.5f + Brick.Phase);
                }
                else if (Brick.Kind == EBrickKind::Mystery)
                {
                    Pulse = 0.18f + 0.18f * Math::Sin(State.Elapsed * 3.5f + Brick.Phase * 1.7f);
                }

                Visual.Color = { Base.x * Dim + Flash + Pulse, Base.y * Dim + Flash + Pulse * 0.4f,
                                 Base.z * Dim + Flash + Pulse * 0.8f, 1.0f };
                Visual.Glow = 0.30f + Brick.Flash * 1.2f + Pulse * 1.4f
                            + Math::Sin(State.Elapsed * 2.2f + Brick.Phase) * 0.08f;
            }
        }

        void CameraSystem(ECS::FRegistry& Registry, float Delta)
        {
            FCameraShake& Shake = Registry.GetSingleton<FCameraShake>();
            FRandom& Rng = Registry.GetSingleton<FRandom>();

            Shake.Trauma = Math::Max(0.0f, Shake.Trauma - Delta * 1.7f);
            Shake.Chroma = Math::Max(0.0f, Shake.Chroma - Delta * 2.2f);

            const float Amount = Shake.Trauma * Shake.Trauma * 30.0f;
            Shake.Offset = { Rng.Range(-Amount, Amount), Rng.Range(-Amount, Amount) };
            Shake.Seed = Rng.Unit();
        }

        void FlowSystem(ECS::FRegistry& Registry, float Delta)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();

            State.Elapsed += Delta;
            State.PhaseTimer += Delta;
            State.FlashPulse = Math::Max(0.0f, State.FlashPulse - Delta * 2.6f);
            State.FireGlow = Math::Max(0.0f, State.FireGlow - Delta * 1.6f);
            State.ComboTimer = Math::Max(0.0f, State.ComboTimer - Delta);
            if (State.ComboTimer <= 0.0f)
            {
                State.Combo = 0;
            }

            State.SlowTimer = Math::Max(0.0f, State.SlowTimer - Delta);
            State.TimeScale = State.SlowTimer > 0.0f ? 0.45f : 1.0f;

            if (State.FeverTimer > 0.0f)
            {
                State.FeverTimer -= Delta;
                State.FeverMeter = Math::Max(0.0f, State.FeverTimer / kFeverDuration);
                if (State.FeverTimer <= 0.0f)
                {
                    State.FeverTimer = 0.0f;
                    State.FeverMeter = 0.0f;
                    PlaySound(Registry, ESound::FeverEnd);
                }
            }
            else
            {
                State.FeverMeter = Math::Max(0.0f, State.FeverMeter - Delta * 0.035f);
            }

            const bool bLive = State.Phase == EPhase::Playing || State.Phase == EPhase::Serve;
            const float DangerGoal = bLive && State.Lives <= 1 ? 1.0f : (bLive && State.Lives == 2 ? 0.32f : 0.0f);
            State.Danger += (DangerGoal - State.Danger) * Math::Min(1.0f, Delta * 2.0f);

            const int32 Difference = State.Score - State.DisplayScore;
            if (Difference > 0)
            {
                State.DisplayScore += Math::Max(1, int32(float(Difference) * Math::Min(1.0f, Delta * 9.0f)));
                State.DisplayScore = Math::Min(State.DisplayScore, State.Score);
            }
            State.HighScore = Math::Max(State.HighScore, State.Score);

            if (!bLive)
            {
                return;
            }

            if (State.Phase == EPhase::Playing)
            {
                State.FormationDrop += (0.9f + float(State.Level) * 0.55f) * Delta;
                State.FormationDrift = State.Level % 3 == 0 ? Math::Sin(State.Elapsed * 0.35f) * 80.0f : 0.0f;
            }

            const float Lowest = LowestBrickEdge(Registry);
            State.BreachWarning = Math::Clamp((Lowest - (kBreachY - 170.0f)) / 170.0f, 0.0f, 1.0f);

            if (Lowest >= kBreachY)
            {
                State.FormationDrop = Math::Max(0.0f, State.FormationDrop - 260.0f);
                State.Lives -= 1;
                State.Combo = 0;
                AddTrauma(Registry, 0.8f, 0.9f);
                AddHitStop(Registry, 0.065f);
                PlaySound(Registry, ESound::Breach);
                SpawnShockwave(Registry, { kFieldWidth * 0.5f, kBreachY }, { 1.4f, 0.2f, 0.25f, 1.0f }, 1200.0f, 0.7f, 1.0f);

                if (State.Lives <= 0)
                {
                    State.Phase = EPhase::GameOver;
                    State.PhaseTimer = 0.0f;
                    PlaySound(Registry, ESound::GameOver);
                }
                return;
            }

            if (State.BricksAlive == 0 && !State.bBossAlive)
            {
                State.Phase = EPhase::LevelClear;
                State.PhaseTimer = 0.0f;
                AddTrauma(Registry, 0.5f, 0.6f);
                AddHitStop(Registry, 0.075f);
                PlaySound(Registry, ESound::LevelClear);
                return;
            }

            if (Registry.View<FBall>().IsEmpty())
            {
                const ECS::FEntity PaddleEntity = FindPaddle(Registry);
                const bool bShielded = !PaddleEntity.IsNull()
                    && Registry.Get<FPaddle>(PaddleEntity).ShieldCharge >= 1.0f;

                if (bShielded)
                {
                    FPaddle& Paddle = Registry.Get<FPaddle>(PaddleEntity);
                    Paddle.ShieldCharge = 0.0f;
                    Paddle.ShieldFlash = 1.0f;

                    ResetBallToPaddle(Registry);
                    State.Phase = EPhase::Serve;
                    State.PhaseTimer = 0.0f;

                    SpawnShockwave(Registry, { kFieldWidth * 0.5f, kPaddleY }, { 0.35f, 1.30f, 1.60f, 1.0f },
                        900.0f, 0.6f, 0.8f);
                    SpawnBurst(Registry, { kFieldWidth * 0.5f, kPaddleY }, { 0.40f, 1.20f, 1.60f, 1.0f },
                        48, 220.0f, 900.0f, 0.0f);
                    AddTrauma(Registry, 0.35f, 0.5f);
                    PlaySound(Registry, ESound::ShieldSave);
                    return;
                }

                State.Lives -= 1;
                AddTrauma(Registry, 0.55f, 0.8f);
                AddHitStop(Registry, 0.065f);
                PlaySound(Registry, State.Lives > 0 ? ESound::LifeLost : ESound::GameOver);
                if (State.Lives == 1)
                {
                    PlaySound(Registry, ESound::Danger);
                }
                State.Phase = State.Lives > 0 ? EPhase::LifeLost : EPhase::GameOver;
                State.PhaseTimer = 0.0f;
            }
        }

        void PhaseTransitionSystem(ECS::FRegistry& Registry)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();

            if (State.Phase == EPhase::LevelClear && State.PhaseTimer > 2.4f)
            {
                StartLevel(Registry, State.Level + 1);
            }
            else if (State.Phase == EPhase::LifeLost && State.PhaseTimer > 1.3f)
            {
                ClearTransients(Registry);
                ResetBallToPaddle(Registry);
                State.FormationDrop = Math::Max(0.0f, State.FormationDrop - 200.0f);
                State.Phase = EPhase::Serve;
                State.PhaseTimer = 0.0f;
            }
        }

        void CelebrationSystem(ECS::FRegistry& Registry, float Delta)
        {
            if (Registry.GetSingleton<FGameState>().Phase != EPhase::LevelClear)
            {
                return;
            }

            FRandom& Rng = Registry.GetSingleton<FRandom>();
            const int32 Count = int32(Delta * 340.0f) + 1;

            for (int32 i = 0; i < Count; ++i)
            {
                if (ParticleBudget(Registry, 1) <= 0)
                {
                    break;
                }

                const FVector2 Position { Rng.Range(0.0f, kFieldWidth), Rng.Range(-60.0f, 120.0f) };
                const FVector4 Color = kRowColors[int32(Rng.Range(0.0f, float(kBrickRowCount) - 0.01f))];

                SpawnParticle(Registry, Position, { Rng.Range(-140.0f, 140.0f), Rng.Range(140.0f, 560.0f) },
                    { Color.x * 1.20f, Color.y * 1.20f, Color.z * 1.20f, 1.0f },
                    { Color.x * 0.20f, Color.y * 0.20f, Color.z * 0.30f, 0.0f },
                    Rng.Range(4.0f, 11.0f), 1.0f, Rng.Range(1.4f, 2.8f), 0.6f, 320.0f, EQuadKind::Spark, 0.45f);
            }
        }
    }


    void FGame::Initialize()
    {
        Registry.EmplaceSingleton<FGameState>();
        Registry.EmplaceSingleton<FCameraShake>();
        Registry.EmplaceSingleton<FFrameInput>();
        Registry.EmplaceSingleton<FSoundQueue>();
        Registry.EmplaceSingleton<FExplosionQueue>();
        Registry.EmplaceSingleton<FRandom>();

        Registry.ReserveComponents<FBody>(kMaxParticles + 512);
        Registry.ReserveComponents<FVisual>(kMaxParticles + 512);
        Registry.ReserveComponents<FParticle>(kMaxParticles);

        const ECS::FEntity PaddleEntity = Registry.Create();

        FBody& Body = Registry.Emplace<FBody>(PaddleEntity);
        Body.Position = { kFieldWidth * 0.5f, kPaddleY };
        Body.HalfSize = { kPaddleBaseHalfWidth, kPaddleHalfHeight };
        Body.Rotation = 0.0f;

        FVisual& Visual = Registry.Emplace<FVisual>(PaddleEntity);
        Visual.Color  = { 0.28f, 0.95f, 1.35f, 1.0f };
        Visual.Accent = { 0.03f, 0.18f, 0.35f, 1.0f };
        Visual.Kind   = EQuadKind::Rect;
        Visual.Glow   = 0.65f;
        Visual.CornerRadius = 0.95f;

        Registry.Emplace<FPaddle>(PaddleEntity);

        BuildLevel(Registry, 1);
        ResetBallToPaddle(Registry);
    }

    void FGame::Advance(float DeltaSeconds)
    {
        FFrameInput& Input = Registry.GetSingleton<FFrameInput>();
        FGameState& State = Registry.GetSingleton<FGameState>();

        if (bPaused)
        {
            Input.bLaunch = false;
            Input.bConfirm = false;
            return;
        }

        const float Clamped = Math::Min(DeltaSeconds, 0.05f);

        float Scale = State.TimeScale;
        if (State.HitStop > 0.0f)
        {
            State.HitStop = Math::Max(0.0f, State.HitStop - Clamped);
            Scale *= kHitStopScale;
        }

        Accumulator += Clamped * Scale;

        int32 Steps = 0;
        while (Accumulator >= kFixedStep && Steps < kMaxCatchUpSteps)
        {
            Accumulator -= kFixedStep;
            StepFixed(kFixedStep);
            ++Steps;
        }

        // Dropping the backlog costs a little simulated time; chasing it feeds the next frame more work.
        Stats.DroppedSteps = Accumulator >= kFixedStep ? int32(Accumulator / kFixedStep) : 0;
        if (Stats.DroppedSteps > 0)
        {
            Accumulator = 0.0f;
        }

        Stats.SimSteps = Steps;
        StepVisual(Clamped * Scale);

        // A frame shorter than the fixed step runs no simulation, so a press has to survive to the next one.
        if (Steps > 0)
        {
            Input.bLaunch = false;
            Input.bConfirm = false;
        }
    }

    void FGame::StepFixed(float Delta)
    {
        FGameState& State = Registry.GetSingleton<FGameState>();

        if (Registry.GetSingleton<FFrameInput>().bConfirm)
        {
            if (State.Phase == EPhase::Title || State.Phase == EPhase::GameOver)
            {
                PlaySound(Registry, ESound::UiConfirm);
                StartRun(Registry);
                return;
            }
            LaunchHeldBalls(Registry);
        }

        PaddleSystem(Registry, Delta);
        HeldBallSystem(Registry);

        if (State.Phase == EPhase::Playing || State.Phase == EPhase::Serve)
        {
            LaserSystem(Registry, Delta);
            BossSystem(Registry, Delta);
            BoltSystem(Registry, Delta);
            BallSystem(Registry, Delta);
            ResolveExplosions(Registry);
            PowerUpSystem(Registry, Delta);
        }

        FlowSystem(Registry, Delta);
        PhaseTransitionSystem(Registry);
    }

    // Purely presentational, so it runs once per frame at the real delta rather than per simulation step.
    void FGame::StepVisual(float Delta)
    {
        BrickIdleSystem(Registry, Delta);
        ParticleSystem(Registry, Delta);
        ShockwaveSystem(Registry, Delta);
        ScorePopSystem(Registry, Delta);
        CelebrationSystem(Registry, Delta);
        CameraSystem(Registry, Delta);

        Stats.Particles = CountParticles(Registry);
        Stats.Entities = int32(Registry.NumEntities());
    }

    void FGame::OnKey(Lumina::EKey Key, bool bPressed)
    {
        FFrameInput& Input = Registry.GetSingleton<FFrameInput>();

        const bool bLeft  = Key == EKey::Left  || Key == EKey::A;
        const bool bRight = Key == EKey::Right || Key == EKey::D;

        if (bLeft || bRight)
        {
            const float Sign = bLeft ? -1.0f : 1.0f;
            if (bPressed)
            {
                Input.bUsingMouse = false;
                Input.KeyAxis = Sign;
            }
            else if (Input.KeyAxis == Sign)
            {
                Input.KeyAxis = 0.0f;
            }
            return;
        }

        if (!bPressed)
        {
            return;
        }

        switch (Key)
        {
        case EKey::Space:
        case EKey::Enter:
            Input.bConfirm = true;
            Input.bLaunch = true;
            break;
        case EKey::P:
            bPaused = !bPaused;
            Registry.GetSingleton<FSoundQueue>().Pending.push_back(FSoundRequest{ ESound::UiMove });
            break;
        case EKey::Escape:
            bQuitRequested = bPaused;
            bPaused = true;
            break;
        case EKey::R:
            StartRun(Registry);
            break;
        case EKey::F3:
            bShowStats = !bShowStats;
            break;
        default:
            break;
        }
    }

    void FGame::OnMouseMoved(float FieldX)
    {
        FFrameInput& Input = Registry.GetSingleton<FFrameInput>();
        Input.PaddleTarget = FieldX;
        Input.bUsingMouse = true;
        Input.KeyAxis = 0.0f;
    }
}
