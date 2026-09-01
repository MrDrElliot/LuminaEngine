#include "Game.h"

#include "Containers/Vector.h"

#include <ctime>

namespace Breakout
{
    namespace
    {
        constexpr float kBrickLeft = (kFieldWidth - (kBrickColumnCount * kBrickPitchX - kBrickGap)) * 0.5f + kBrickWidth * 0.5f;

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

        const FVector4 kFireColor  { 1.00f, 0.42f, 0.10f, 1.0f };
        const FVector4 kSmashColor { 1.80f, 1.60f, 1.20f, 1.0f };
        const FVector4 kVaultColor { 1.60f, 1.30f, 0.30f, 1.0f };
        const FVector4 kPortalColor { 0.30f, 1.20f, 1.60f, 1.0f };
        const FVector4 kIceColor   { 0.55f, 0.85f, 1.30f, 1.0f };

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
            case EPowerUp::Magnet:    return { 0.80f, 0.40f, 1.00f, 1.0f };
            case EPowerUp::Shield:    return { 0.30f, 1.00f, 1.00f, 1.0f };
            case EPowerUp::Bomb:      return { 1.00f, 0.62f, 0.20f, 1.0f };
            case EPowerUp::BigBall:   return { 0.90f, 0.95f, 1.00f, 1.0f };
            case EPowerUp::Wall:      return { 0.20f, 0.90f, 0.60f, 1.0f };
            case EPowerUp::Freeze:    return { 0.55f, 0.85f, 1.30f, 1.0f };
            case EPowerUp::Jackpot:   return { 1.20f, 1.00f, 0.25f, 1.0f };
            case EPowerUp::Reverse:   return { 0.95f, 0.15f, 0.60f, 1.0f };
            case EPowerUp::Blind:     return { 0.45f, 0.05f, 0.30f, 1.0f };
            case EPowerUp::Drop:      return { 0.90f, 0.20f, 0.10f, 1.0f };
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
            case EBrickKind::Mover:     return { 1.00f, 0.85f, 0.35f, 1.0f };
            case EBrickKind::Ghost:     return { 0.70f, 0.90f, 1.00f, 1.0f };
            case EBrickKind::Regen:     return { 0.30f, 1.00f, 0.55f, 1.0f };
            case EBrickKind::Portal:    return { 0.30f, 1.10f, 1.40f, 1.0f };
            case EBrickKind::Gravity:   return { 0.55f, 0.30f, 1.00f, 1.0f };
            case EBrickKind::Bumper:    return { 1.00f, 0.55f, 0.90f, 1.0f };
            case EBrickKind::Gold:      return { 1.30f, 1.05f, 0.25f, 1.0f };
            default:                    return kRowColors[Brick.Row];
            }
        }

        FVector4 DroneColor(EDroneKind Kind)
        {
            switch (Kind)
            {
            case EDroneKind::Cone: return { 1.00f, 0.55f, 0.20f, 1.0f };
            case EDroneKind::Tri:  return { 1.00f, 0.25f, 0.45f, 1.0f };
            default:               return { 0.60f, 0.40f, 1.20f, 1.0f };
            }
        }


        //~ Authored layouts. Row 0 is the top. Legend lives in CharToBrick.

        struct FLayout
        {
            const char* Name;
            const char* Rows[kBrickRowCount];
        };

        const FLayout kAuthoredLevels[] =
        {
            { "WARM UP", {
                "...........",
                "...........",
                "....===....",
                "..#######..",
                ".#########.",
                "###########",
                "###########",
                "..........." } },
            { "CHECKER", {
                "#.#.#.#.#.#",
                ".#.#.#.#.#.",
                "#.#.#.#.#.#",
                ".#.X.#.X.#.",
                "#.#.#.#.#.#",
                ".=.=.$.=.=.",
                "=.=.=.=.=.=",
                "..........." } },
            { "THE TUNNEL", {
                "SSSSSSSSSS.",
                "$#########.",
                "=========#.",
                "#########=.",
                ".#########.",
                ".=========.",
                ".#########.",
                "..........." } },
            { "CONVEYOR", {
                "...........",
                "MMM.....MMM",
                "...........",
                "...MMMMM...",
                "...........",
                "=====X=====",
                "###########",
                "..........." } },
            { "GHOST TOWN", {
                "HHHHHHHHHHH",
                ".=.=.=.=.=.",
                "H.#.#.#.#.H",
                ".=.=.X.=.=.",
                "H.#.#.#.#.H",
                ".=.=.=.=.=.",
                "HHHHHHHHHHH",
                "..........." } },
            { "WORMHOLE", {
                "P.........P",
                ".#########.",
                ".#=======#.",
                ".#=X...X=#.",
                ".#=======#.",
                ".#########.",
                "P.........P",
                "..........." } },
            { "SINGULARITY", {
                "=====.=====",
                "#####.#####",
                "##..G.G..##",
                "##...$...##",
                "##..G.G..##",
                "#####.#####",
                "=====.=====",
                "..........." } },
            { "PINBALL", {
                ".#.......#.",
                "#.B..$..B.#",
                ".#.......#.",
                "...B...B...",
                ".=.......=.",
                "=.B..$..B.=",
                ".=.......=.",
                "..........." } },
            { "FORTRESS", {
                "S====X====S",
                "S=%%%%%%%=S",
                "S=%RRRRR%=S",
                "S=%R$$$R%=S",
                "S=%RRRRR%=S",
                "S=%%%%%%%=S",
                "S====X====S",
                ".....?....." } },
            { "MINEFIELD", {
                "#.X.#.X.#.X",
                ".#.#.#.#.#.",
                "X.#.X.#.X.#",
                ".#.#.#.#.#.",
                "#.X.#.X.#.X",
                "...........",
                "=====?=====",
                "###########" } },
            { "SPIRAL", {
                "###########",
                "#.........#",
                "#.=======.#",
                "#.=.....=.#",
                "#.=.%G%.=.#",
                "#.=.....=.#",
                "#.=======.#",
                "#....H....#" } },
            { "THE HIVE", {
                ".R.R.R.R.R.",
                "R.R.R.R.R.R",
                ".R.R.R.R.R.",
                "R.R.$.$.R.R",
                ".R.R.R.R.R.",
                "...........",
                "MMMMMMMMMMM",
                "..........." } },
            { "GAUNTLET", {
                "SGS.MMM.SGS",
                "H=H.=X=.H=H",
                "P.#######.P",
                ".#%%%%%%%#.",
                ".#%B.$.B%#.",
                ".#%%%%%%%#.",
                "P.#######.P",
                "..........." } },
            { "TWIN PEAKS", {
                "..X.....X..",
                ".=%=...=%=.",
                "=###=.=###=",
                "#####G#####",
                ".=#=.$.=#=.",
                "..R.....R..",
                "HHHH...HHHH",
                "..........." } },
            { "LATTICE", {
                "S.S.S.S.S.S",
                ".=.=.=.=.=.",
                "S.R.R.R.R.S",
                ".=.X.$.X.=.",
                "S.R.R.R.R.S",
                ".=.=.=.=.=.",
                "S.S.S.S.S.S",
                "..M.....M.." } },
            { "FINALE", {
                "P%%%%X%%%%P",
                "G=RRR=RRR=G",
                "H#H#H#H#H#H",
                "MMMM.$.MMMM",
                "B.=.B.B.=.B",
                "=X=======X=",
                "SS.#####.SS",
                "..........." } },
        };

        constexpr int32 kAuthoredCount = int32(sizeof(kAuthoredLevels) / sizeof(kAuthoredLevels[0]));

        bool CharToBrick(char Glyph, EBrickKind& Kind, int32& Health)
        {
            Health = 0;
            switch (Glyph)
            {
            case '#': Kind = EBrickKind::Normal;     Health = 1; return true;
            case '=': Kind = EBrickKind::Reinforced; Health = 2; return true;
            case '%': Kind = EBrickKind::Reinforced; Health = 3; return true;
            case 'X': Kind = EBrickKind::Explosive;  return true;
            case 'S': Kind = EBrickKind::Steel;      return true;
            case '?': Kind = EBrickKind::Mystery;    return true;
            case 'M': Kind = EBrickKind::Mover;      return true;
            case 'H': Kind = EBrickKind::Ghost;      return true;
            case 'R': Kind = EBrickKind::Regen;      return true;
            case 'P': Kind = EBrickKind::Portal;     return true;
            case 'G': Kind = EBrickKind::Gravity;    return true;
            case 'B': Kind = EBrickKind::Bumper;     return true;
            case '$': Kind = EBrickKind::Gold;       return true;
            default:  return false;
            }
        }

        bool BrickPresent(int32 Level, int32 Row, int32 Column)
        {
            switch (Level % 8)
            {
            case 0:  return (Row + Column) % 3 != 2;
            case 1:  return true;
            case 2:  return (Row + Column) % 4 != 3;
            case 3:  return Math::Abs(Column - kBrickColumnCount / 2) <= (kBrickRowCount - Row) / 2 + 1;
            case 4:  return !(Row >= 2 && Row <= 4 && Column >= 4 && Column <= 6);
            case 5:  return (Column % 2 == 0) || (Row % 2 == 0);
            case 6:  return Math::Abs(Column - kBrickColumnCount / 2) >= Row / 2;
            default: return (Row + Column) % 2 == 0 || Row == 0 || Row == kBrickRowCount - 1;
            }
        }

        EBrickKind ProceduralKind(FRandom& Rng, int32 Level, int32 Row, int32 Column)
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
            if (Rng.Unit() < 0.025f)
            {
                return EBrickKind::Gold;
            }
            if (Level >= 3 && Rng.Unit() < 0.04f)
            {
                return EBrickKind::Ghost;
            }
            if (Level >= 4 && Rng.Unit() < 0.035f)
            {
                return EBrickKind::Regen;
            }
            if (Level >= 5 && Row >= 2 && Rng.Unit() < 0.018f)
            {
                return EBrickKind::Gravity;
            }
            if (Level >= 6 && Row >= 3 && Rng.Unit() < 0.014f)
            {
                return EBrickKind::Bumper;
            }
            return Row <= 4 ? EBrickKind::Reinforced : EBrickKind::Normal;
        }

        int32 BrickHealthFor(EBrickKind Kind, int32 Row, int32 Level)
        {
            switch (Kind)
            {
            case EBrickKind::Steel:
            case EBrickKind::Portal:
            case EBrickKind::Bumper:
                return 9999;
            case EBrickKind::Explosive:
            case EBrickKind::Mystery:
            case EBrickKind::Gold:
            case EBrickKind::Ghost:
                return 1;
            case EBrickKind::Regen:
            case EBrickKind::Gravity:
                return 2;
            case EBrickKind::Mover:
                return Math::Min(1 + Level / 4, 3);
            default:
                break;
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

        float HighestScoringBrickEdge(ECS::FRegistry& Registry, bool& bAny)
        {
            int32 HighestRow = kBrickRowCount;
            for (auto [Entity, Brick] : Registry.View<FBrick>().Each())
            {
                if (Brick.Health > 0 && BrickScores(Brick.Kind))
                {
                    HighestRow = Math::Min(HighestRow, Brick.Row);
                }
            }

            bAny = HighestRow < kBrickRowCount;
            const FGameState& State = Registry.GetSingleton<FGameState>();
            return kBrickTop + float(HighestRow) * kBrickPitchY - kBrickHeight * 0.5f + State.FormationDrop;
        }

        float VectorLength(const FVector2& Value)
        {
            return Math::Sqrt(Value.x * Value.x + Value.y * Value.y);
        }

        FVector2 Normalized(const FVector2& Value)
        {
            const float Length = VectorLength(Value);
            return Length > 0.0001f ? Value * (1.0f / Length) : FVector2{ 0.0f, -1.0f };
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

        void AddTrauma(ECS::FRegistry& Registry, float Amount, float ChromaAmount = 0.0f)
        {
            FCameraShake& Shake = Registry.GetSingleton<FCameraShake>();
            Shake.Trauma = Math::Min(Shake.Trauma + Amount, 1.0f);
            Shake.Chroma = Math::Min(Shake.Chroma + ChromaAmount, 1.0f);
        }

        void AwardScore(ECS::FRegistry& Registry, int32 RawPoints, const FVector2& Position, const FVector4& Color,
                        const char* Label = nullptr, float Size = 3.5f)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            const int32 Points = State.Scaled(RawPoints);
            State.Score += Points;

            const ECS::FEntity Entity = Registry.Create();
            FScorePop& Pop = Registry.Emplace<FScorePop>(Entity);
            Pop.Position = Position;
            Pop.Value    = Points;
            Pop.Color    = Color;
            Pop.Size     = Size;
            if (Label != nullptr)
            {
                int32 Index = 0;
                for (; Label[Index] != '\0' && Index < 13; ++Index)
                {
                    Pop.Label[Index] = Label[Index];
                }
                Pop.Label[Index] = '\0';
            }
        }

        void SpawnLabel(ECS::FRegistry& Registry, const FVector2& Position, const char* Label, const FVector4& Color,
                        float Size = 5.0f, float Duration = 1.1f)
        {
            const ECS::FEntity Entity = Registry.Create();
            FScorePop& Pop = Registry.Emplace<FScorePop>(Entity);
            Pop.Position = Position;
            Pop.Value    = 0;
            Pop.Color    = Color;
            Pop.Size     = Size;
            Pop.Duration = Duration;
            int32 Index = 0;
            for (; Label[Index] != '\0' && Index < 13; ++Index)
            {
                Pop.Label[Index] = Label[Index];
            }
            Pop.Label[Index] = '\0';
        }


        //~ Spawning.

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

        ECS::FEntity SpawnBall(ECS::FRegistry& Registry, const FVector2& Position, const FVector2& Velocity,
                               float Speed, float FireTimer = 0.0f, float BigTimer = 0.0f)
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
            Ball.BigTimer  = BigTimer;
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

        ECS::FEntity SpawnBoss(ECS::FRegistry& Registry, EBossKind Kind, int32 Level, float Scale, uint8 Generation,
                               const FVector2& Position, float Health)
        {
            const ECS::FEntity Entity = Registry.Create();

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = Position;
            Body.HalfSize = { 178.0f * Scale, 54.0f * Scale };
            Body.Rotation = 0.0f;

            FVisual& Visual = Registry.Emplace<FVisual>(Entity);
            Visual.Color  = { 1.20f, 0.28f, 0.75f, 1.0f };
            Visual.Accent = { 0.28f, 0.05f, 0.22f, 1.0f };
            Visual.Kind   = EQuadKind::Brick;
            Visual.Glow   = 0.85f;
            Visual.CornerRadius = Kind == EBossKind::Hydra ? 0.55f : 0.30f;

            FBoss& Boss = Registry.Emplace<FBoss>(Entity);
            Boss.Kind       = Kind;
            Boss.MaxHealth  = Health;
            Boss.Health     = Health;
            Boss.Scale      = Scale;
            Boss.Generation = Generation;
            Boss.DriftPhase = Registry.GetSingleton<FRandom>().Range(0.0f, 6.28f);
            Boss.FireTimer  = Kind == EBossKind::Warden ? 2.4f : 3.6f;
            Boss.RepairTimer = 3.0f;
            Boss.ArmorTimer = 3.0f;
            Boss.bArmored   = false;
            (void)Level;
            return Entity;
        }

        float BossHealthFor(EBossKind Kind, int32 Level)
        {
            switch (Kind)
            {
            case EBossKind::Warden:    return 26.0f + float(Level) * 5.0f;
            case EBossKind::Architect: return 22.0f + float(Level) * 4.0f;
            default:                   return 30.0f + float(Level) * 5.0f;
            }
        }

        void SpawnDrone(ECS::FRegistry& Registry, EDroneKind Kind, const FVector2& Position)
        {
            const ECS::FEntity Entity = Registry.Create();
            const FVector4 Color = DroneColor(Kind);

            FDrone& Drone = Registry.Emplace<FDrone>(Entity);
            Drone.Kind   = Kind;
            Drone.Phase  = Registry.GetSingleton<FRandom>().Range(0.0f, 6.28f);
            Drone.Health = Kind == EDroneKind::Tri ? 2 : 1;
            Drone.Radius = Kind == EDroneKind::Orb ? 40.0f : (Kind == EDroneKind::Tri ? 24.0f : 28.0f);

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = Position;
            Body.HalfSize = { Drone.Radius, Drone.Radius };
            Body.Rotation = 0.0f;

            FVisual& Visual = Registry.Emplace<FVisual>(Entity);
            Visual.Color  = Color;
            Visual.Accent = { Color.x * 0.25f, Color.y * 0.25f, Color.z * 0.35f, 1.0f };
            Visual.Kind   = EQuadKind::Drone;
            Visual.Glow   = 0.9f;
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
        void DamageBrick(ECS::FRegistry& Registry, ECS::FEntity Entity, int32 Damage, bool bOverkill, bool bSmash = false);

        void ClearTransients(ECS::FRegistry& Registry)
        {
            DestroyAllWith<FParticle>(Registry);
            DestroyAllWith<FShockwave>(Registry);
            DestroyAllWith<FPowerUpDrop>(Registry);
            DestroyAllWith<FScorePop>(Registry);
            DestroyAllWith<FLaserBolt>(Registry);
            DestroyAllWith<FDrone>(Registry);

            FExplosionQueue& Queue = Registry.GetSingleton<FExplosionQueue>();
            Queue.Pending.clear();
            Queue.Cursor = 0;
        }

        TVector<ECS::FEntity> CollectPaddles(ECS::FRegistry& Registry)
        {
            TVector<ECS::FEntity> Paddles;
            for (int32 Index = 0; Index < kMaxPaddles; ++Index)
            {
                for (auto [Entity, Paddle] : Registry.View<FPaddle>().Each())
                {
                    if (Paddle.PlayerIndex == Index)
                    {
                        Paddles.push_back(Entity);
                    }
                }
            }
            return Paddles;
        }

        ECS::FEntity SpawnPaddle(ECS::FRegistry& Registry, uint8 PlayerIndex)
        {
            const ECS::FEntity Entity = Registry.Create();

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = { kFieldWidth * (PlayerIndex == 0 ? 0.35f : 0.65f), kPaddleY };
            Body.HalfSize = { kPaddleBaseHalfWidth, kPaddleHalfHeight };
            Body.Rotation = 0.0f;

            FVisual& Visual = Registry.Emplace<FVisual>(Entity);
            Visual.Color  = PlayerIndex == 0 ? FVector4{ 0.28f, 0.95f, 1.35f, 1.0f } : FVector4{ 1.35f, 0.70f, 0.25f, 1.0f };
            Visual.Accent = PlayerIndex == 0 ? FVector4{ 0.03f, 0.18f, 0.35f, 1.0f } : FVector4{ 0.35f, 0.14f, 0.03f, 1.0f };
            Visual.Kind   = EQuadKind::Rect;
            Visual.Glow   = 0.65f;
            Visual.CornerRadius = 0.95f;

            FPaddle& Paddle = Registry.Emplace<FPaddle>(Entity);
            Paddle.PlayerIndex = PlayerIndex;
            return Entity;
        }

        void EnsurePaddles(ECS::FRegistry& Registry, int32 Count)
        {
            TVector<ECS::FEntity> Paddles = CollectPaddles(Registry);
            while (int32(Paddles.size()) > Count)
            {
                Registry.Destroy(Paddles.back());
                Paddles.pop_back();
            }
            while (int32(Paddles.size()) < Count)
            {
                Paddles.push_back(SpawnPaddle(Registry, uint8(Paddles.size())));
            }
        }

        void ApplyBrickVisual(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            const FBrick& Brick = Registry.Get<FBrick>(Entity);
            const FVector4 Base = BrickBaseColor(Brick);

            FVisual& Visual = Registry.Get<FVisual>(Entity);
            Visual.Color  = { Base.x * 0.88f, Base.y * 0.88f, Base.z * 0.88f, 1.0f };
            Visual.Accent = { Base.x * 0.20f, Base.y * 0.20f, Base.z * 0.30f, 1.0f };
            Visual.Kind   = Brick.Kind == EBrickKind::Bumper ? EQuadKind::Disc : EQuadKind::Brick;
            Visual.Glow   = Brick.Kind == EBrickKind::Explosive || Brick.Kind == EBrickKind::Gold ? 0.55f : 0.30f;
            Visual.CornerRadius = Brick.Kind == EBrickKind::Steel ? 0.10f : 0.22f;
        }

        ECS::FEntity SpawnBrick(ECS::FRegistry& Registry, int32 Row, int32 Column, EBrickKind Kind, int32 Health)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            const ECS::FEntity Entity = Registry.Create();

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = BrickCenter(Row, Column, State);
            Body.HalfSize = Kind == EBrickKind::Bumper
                ? FVector2{ 24.0f, 24.0f }
                : FVector2{ kBrickWidth * 0.5f, kBrickHeight * 0.5f };
            Body.Rotation = 0.0f;

            FBrick& Brick = Registry.Emplace<FBrick>(Entity);
            Brick.Row       = Row;
            Brick.Column    = Column;
            Brick.Kind      = Kind;
            Brick.MaxHealth = Health;
            Brick.Health    = Health;
            Brick.Phase     = float(Row) * 0.45f + float(Column) * 0.22f;
            Brick.Solidity  = 1.0f;

            Registry.Emplace<FVisual>(Entity);
            ApplyBrickVisual(Registry, Entity);

            if (BrickScores(Kind))
            {
                ++State.BricksAlive;
            }
            return Entity;
        }

        bool LevelHasBoss(EGameMode Mode, int32 Level)
        {
            switch (Mode)
            {
            case EGameMode::BossRush: return true;
            case EGameMode::Endless:  return Level % 4 == 0;
            default:                  return Level % 5 == 0;
            }
        }

        EBossKind BossKindFor(EGameMode Mode, int32 Level)
        {
            const int32 Ordinal = Mode == EGameMode::BossRush ? Level - 1
                                : (Mode == EGameMode::Endless ? Level / 4 - 1 : Level / 5 - 1);
            return EBossKind(Math::Max(Ordinal, 0) % int32(EBossKind::Count));
        }

        int32 AuthoredIndexFor(EGameMode Mode, int32 Level)
        {
            if (Mode == EGameMode::Endless || Mode == EGameMode::BossRush)
            {
                return -1;
            }
            const int32 Index = Level - 1 - (Level - 1) / 5;
            return Index < kAuthoredCount ? Index : -1;
        }

        void AssignPortalPairs(ECS::FRegistry& Registry)
        {
            TVector<ECS::FEntity> Portals;
            for (auto [Entity, Brick] : Registry.View<FBrick>().Each())
            {
                if (Brick.Kind == EBrickKind::Portal)
                {
                    Portals.push_back(Entity);
                }
            }

            for (int32 Index = 0; Index < int32(Portals.size()); ++Index)
            {
                Registry.Get<FBrick>(Portals[Index]).PortalPair = Index / 2;
            }
            if (Portals.size() % 2 == 1)
            {
                Registry.Get<FBrick>(Portals.back()).PortalPair = -1;
            }
        }

        void BuildAuthoredLevel(ECS::FRegistry& Registry, const FLayout& Layout, int32 Level)
        {
            for (int32 Row = 0; Row < kBrickRowCount; ++Row)
            {
                const char* Line = Layout.Rows[Row];
                for (int32 Column = 0; Column < kBrickColumnCount && Line[Column] != '\0'; ++Column)
                {
                    EBrickKind Kind = EBrickKind::Normal;
                    int32 Health = 0;
                    if (!CharToBrick(Line[Column], Kind, Health))
                    {
                        continue;
                    }
                    if (Health == 0)
                    {
                        Health = BrickHealthFor(Kind, Row, Level);
                    }
                    else
                    {
                        Health = Math::Min(Health + (Level - 1) / 6, 4);
                    }
                    SpawnBrick(Registry, Row, Column, Kind, Health);
                }
            }
        }

        void BuildProceduralRows(ECS::FRegistry& Registry, int32 Level, int32 FirstRow)
        {
            FRandom& Rng = Registry.GetSingleton<FRandom>();
            const int32 MoverRow = Level >= 4 && Rng.Unit() < 0.3f ? FirstRow + Rng.Below(kBrickRowCount - FirstRow) : -1;
            const bool bPortals = Level >= 6 && Rng.Unit() < 0.3f;

            for (int32 Row = FirstRow; Row < kBrickRowCount; ++Row)
            {
                for (int32 Column = 0; Column < kBrickColumnCount; ++Column)
                {
                    const bool bPortalSlot = bPortals && Row == FirstRow && (Column == 0 || Column == kBrickColumnCount - 1);
                    if (bPortalSlot)
                    {
                        SpawnBrick(Registry, Row, Column, EBrickKind::Portal, 9999);
                        continue;
                    }
                    if (!BrickPresent(Level, Row, Column))
                    {
                        continue;
                    }

                    const EBrickKind Kind = Row == MoverRow ? EBrickKind::Mover : ProceduralKind(Rng, Level, Row, Column);
                    SpawnBrick(Registry, Row, Column, Kind, BrickHealthFor(Kind, Row, Level));
                }
            }
        }

        void BuildLevel(ECS::FRegistry& Registry, int32 Level)
        {
            DestroyAllWith<FBrick>(Registry);
            DestroyAllWith<FBoss>(Registry);

            FGameState& State = Registry.GetSingleton<FGameState>();
            State.BricksAlive = 0;
            State.bBossAlive = LevelHasBoss(State.Mode, Level);
            State.bAuthoredLevel = false;
            State.BossIntro = 0.0f;

            if (State.bBossAlive)
            {
                State.BossKind = BossKindFor(State.Mode, Level);
                const float Menace = State.Mode == EGameMode::BossRush ? 1.0f + float(Level) * 0.12f : 1.0f;
                SpawnBoss(Registry, State.BossKind, Level, 1.0f, 0, { kFieldWidth * 0.5f, kBossTopY },
                    BossHealthFor(State.BossKind, Level) * Menace);
                State.BossIntro = 2.6f;
                BuildProceduralRows(Registry, Level, 3);
                PlaySound(Registry, ESound::BossIntro);
            }
            else
            {
                const int32 Authored = AuthoredIndexFor(State.Mode, Level);
                if (Authored >= 0)
                {
                    State.bAuthoredLevel = true;
                    BuildAuthoredLevel(Registry, kAuthoredLevels[Authored], Level);
                }
                else
                {
                    BuildProceduralRows(Registry, Level, 0);
                }
            }

            AssignPortalPairs(Registry);

            State.BricksTotal = Math::Max(State.BricksAlive, 1);
            State.Progress = 0.0f;
        }

        const char* LevelName(const FGameState& State)
        {
            if (State.bBossAlive)
            {
                return BossName(State.BossKind);
            }
            const int32 Authored = AuthoredIndexFor(State.Mode, State.Level);
            return Authored >= 0 ? kAuthoredLevels[Authored].Name : nullptr;
        }

        void ResetBallToPaddle(ECS::FRegistry& Registry)
        {
            DestroyAllWith<FBall>(Registry);

            for (auto [Entity, Body, Paddle] : Registry.View<FBody, FPaddle>().Each())
            {
                const FVector2 Position { Body.Position.x, kPaddleY - kPaddleHalfHeight - kBallRadius - 4.0f };
                const ECS::FEntity BallEntity = SpawnBall(Registry, Position, { 0.0f, 0.0f }, kBallBaseSpeed);
                FBall& Ball = Registry.Get<FBall>(BallEntity);
                Ball.bHeld  = true;
                Ball.HeldBy = Paddle.PlayerIndex;
            }
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
            State.WallTimer    = 0.0f;
            State.FreezeTimer  = 0.0f;
            State.JackpotTimer = 0.0f;
            State.BlindTimer   = 0.0f;
            State.VaultTimer   = 0.0f;
            State.DroneTimer   = 7.0f;
            State.LevelTime    = 0.0f;
            State.LevelLivesLost = 0;
            State.LevelBestCombo = 0;
            State.LevelSmashes = 0;
            State.bBulwarkSpent = false;

            ClearTransients(Registry);
            BuildLevel(Registry, Level);

            for (auto [Entity, Body, Paddle] : Registry.View<FBody, FPaddle>().Each())
            {
                const uint8 Index = Paddle.PlayerIndex;
                Paddle = FPaddle{};
                Paddle.PlayerIndex = Index;
                Paddle.CatchTimer = State.Perks.Has(EPerk::Sticky) ? 6.0f : 0.0f;
                Body.HalfSize = { State.Perks.BasePaddleHalfWidth(), kPaddleHalfHeight };
            }

            ResetBallToPaddle(Registry);
        }

        uint64 SeedForMode(EGameMode Mode)
        {
            const uint64 Now = uint64(std::time(nullptr));
            return Mode == EGameMode::Daily ? (Now / 86400ull) * 0x9E3779B97F4A7C15ull : Now * 0xBF58476D1CE4E5B9ull;
        }

        void StartRun(ECS::FRegistry& Registry, EGameMode Mode)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            State.Mode         = Mode;
            State.MenuCursor   = Mode;
            State.Score        = 0;
            State.DisplayScore = 0;
            State.Lives        = 3;
            State.BestCombo    = 0;
            State.FeverMeter   = 0.0f;
            State.FeverTimer   = 0.0f;
            State.Smashes      = 0;
            State.Vaults       = 0;
            State.DronesDowned = 0;
            State.BricksBroken = 0;
            State.Perks.Clear();

            Registry.GetSingleton<FRandom>().Reseed(SeedForMode(Mode));
            EnsurePaddles(Registry, Mode == EGameMode::Coop ? 2 : 1);
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
                const bool bReversed = Paddle.ReverseTimer > 0.0f;
                const float Axis = Input.KeyAxis[Paddle.PlayerIndex] * (bReversed ? -1.0f : 1.0f);

                float Target = Body.Position.x;
                if (Paddle.PlayerIndex == 0 && Input.bUsingMouse)
                {
                    Target = bReversed ? kFieldWidth - Input.PaddleTarget : Input.PaddleTarget;
                }
                else if (Axis != 0.0f)
                {
                    Target = Body.Position.x + Axis * 2600.0f * Delta;
                }

                if (State.Phase == EPhase::Title || State.Phase == EPhase::GameOver)
                {
                    Target = kFieldWidth * 0.5f + Math::Sin(State.Elapsed * 0.9f + float(Paddle.PlayerIndex) * 3.1f) * 380.0f;
                }

                const float BaseHalf = State.Perks.BasePaddleHalfWidth();
                Paddle.HalfWidthGoal = Paddle.ShrinkTimer > 0.0f ? kPaddleThinHalfWidth
                                     : (Paddle.WidenTimer > 0.0f ? kPaddleWideHalfWidth : BaseHalf);

                Body.HalfSize.x += (Paddle.HalfWidthGoal - Body.HalfSize.x) * Math::Min(1.0f, Delta * 9.0f);
                Body.HalfSize.y = kPaddleHalfHeight;

                const float Clamped = Math::Clamp(Target, Body.HalfSize.x + 8.0f, kFieldWidth - Body.HalfSize.x - 8.0f);
                const float Previous = Body.Position.x;

                Body.Position.x += (Clamped - Body.Position.x) * Math::Min(1.0f, Delta * 26.0f);
                Body.Position.y = kPaddleY;

                Paddle.Velocity = Delta > 0.0f ? (Body.Position.x - Previous) / Delta : 0.0f;
                Paddle.Tilt += (Math::Clamp(Paddle.Velocity * 0.00007f, -0.10f, 0.10f) - Paddle.Tilt) * Math::Min(1.0f, Delta * 12.0f);
                Body.Rotation = Paddle.Tilt;

                Paddle.WidenTimer   = Math::Max(0.0f, Paddle.WidenTimer - Delta);
                Paddle.ShrinkTimer  = Math::Max(0.0f, Paddle.ShrinkTimer - Delta);
                Paddle.LaserTimer   = Math::Max(0.0f, Paddle.LaserTimer - Delta);
                Paddle.CatchTimer   = Math::Max(0.0f, Paddle.CatchTimer - Delta);
                Paddle.MagnetTimer  = Math::Max(0.0f, Paddle.MagnetTimer - Delta);
                Paddle.ReverseTimer = Math::Max(0.0f, Paddle.ReverseTimer - Delta);
                Paddle.SmashArm     = Math::Max(0.0f, Paddle.SmashArm - Delta);
                Paddle.SmashFlash   = Math::Max(0.0f, Paddle.SmashFlash - Delta * 3.0f);
                Paddle.HitFlash     = Math::Max(0.0f, Paddle.HitFlash - Delta * 4.0f);
                Paddle.ShieldFlash  = Math::Max(0.0f, Paddle.ShieldFlash - Delta * 2.2f);

                if (State.Phase == EPhase::Playing || State.Phase == EPhase::Serve)
                {
                    const float Before = Paddle.ShieldCharge;
                    Paddle.ShieldCharge = Math::Min(1.0f, Paddle.ShieldCharge + Delta / State.Perks.ShieldRechargeTime());
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
            const FGameState& State = Registry.GetSingleton<FGameState>();
            const bool bQuad = State.Perks.Has(EPerk::TwinLaser);

            struct FShot
            {
                FVector2 Position;
                float    HalfWidth;
                float    Timer;
            };
            TVector<FShot> Shots;

            for (auto [Entity, Body, Paddle] : Registry.View<FBody, FPaddle>().Each())
            {
                Paddle.LaserCooldown = Math::Max(0.0f, Paddle.LaserCooldown - Delta);
                if (Paddle.LaserTimer <= 0.0f || Paddle.LaserCooldown > 0.0f)
                {
                    continue;
                }
                Paddle.LaserCooldown = kLaserInterval;
                Shots.push_back({ Body.Position, Body.HalfSize.x, Paddle.LaserTimer });
            }

            for (const FShot& Shot : Shots)
            {
                const FVector2 Up { 0.0f, -kBoltSpeed };
                SpawnBolt(Registry, { Shot.Position.x - Shot.HalfWidth * 0.82f, Shot.Position.y - 26.0f }, Up, false);
                SpawnBolt(Registry, { Shot.Position.x + Shot.HalfWidth * 0.82f, Shot.Position.y - 26.0f }, Up, false);
                if (bQuad)
                {
                    SpawnBolt(Registry, { Shot.Position.x - Shot.HalfWidth * 0.35f, Shot.Position.y - 26.0f }, { -160.0f, -kBoltSpeed }, false);
                    SpawnBolt(Registry, { Shot.Position.x + Shot.HalfWidth * 0.35f, Shot.Position.y - 26.0f }, { 160.0f, -kBoltSpeed }, false);
                }

                const float Pitch = Shot.Timer < 3.0f ? 1.18f : 1.0f;
                PlaySound(Registry, ESound::Laser, Pitch * Registry.GetSingleton<FRandom>().Range(0.94f, 1.06f), 0.8f,
                    PanFor(Shot.Position.x));
            }
        }

        void HeldBallSystem(ECS::FRegistry& Registry)
        {
            float PaddleX[kMaxPaddles] = { kFieldWidth * 0.5f, kFieldWidth * 0.5f };
            for (auto [Entity, Body, Paddle] : Registry.View<FBody, FPaddle>().Each())
            {
                PaddleX[Math::Min<int32>(Paddle.PlayerIndex, kMaxPaddles - 1)] = Body.Position.x;
            }

            for (auto [Entity, Body, Ball] : Registry.View<FBody, FBall>().Each())
            {
                if (!Ball.bHeld)
                {
                    continue;
                }
                const float Radius = Ball.Radius();
                Body.Position.x = PaddleX[Math::Min<int32>(Ball.HeldBy, kMaxPaddles - 1)] + Ball.HeldOffset;
                Body.Position.y = kPaddleY - kPaddleHalfHeight - Radius - 4.0f;
                Body.HalfSize = { Radius, Radius };

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

            struct FExtra
            {
                FVector2 Position;
                float    Speed;
            };
            TVector<FExtra> Extras;

            for (auto [Entity, Body, Ball] : Registry.View<FBody, FBall>().Each())
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

                if (State.Phase == EPhase::Serve)
                {
                    for (int32 Extra = 0; Extra < State.Perks.Count(EPerk::Splitter); ++Extra)
                    {
                        Extras.push_back({ Body.Position, Ball.Speed });
                    }
                }
            }

            for (int32 Index = 0; Index < int32(Extras.size()); ++Index)
            {
                const float Angle = (Index % 2 == 0 ? -0.7f : 0.7f) + Rng.Range(-0.15f, 0.15f);
                SpawnBall(Registry, Extras[Index].Position, { Math::Sin(Angle) * Extras[Index].Speed,
                    -Math::Cos(Angle) * Extras[Index].Speed }, Extras[Index].Speed);
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

        void ApplySmash(ECS::FRegistry& Registry, FBall& Ball, FVector2& Velocity, const FVector2& Position)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();

            Ball.Speed = State.Perks.MaxBallSpeed();
            const float Angle = Math::Atan2(Velocity.x, -Velocity.y) * 0.65f;
            Velocity = { Math::Sin(Angle) * Ball.Speed, -Math::Cos(Angle) * Ball.Speed };
            Ball.SmashTimer = kSmashDuration;
            Ball.SmashGrace = 0.0f;
            Ball.Squash = 1.0f;

            State.Smashes += 1;
            State.LevelSmashes += 1;
            AwardScore(Registry, 300, { Position.x, Position.y - 60.0f }, kSmashColor, "SMASH", 5.0f);
            SpawnShockwave(Registry, Position, kSmashColor, 420.0f, 0.5f, 0.9f);
            SpawnBurst(Registry, Position, kSmashColor, 40, 300.0f, 1200.0f, 300.0f);
            AddTrauma(Registry, 0.4f, 0.5f);
            AddHitStop(Registry, 0.05f);
            PlaySound(Registry, ESound::Smash, 1.0f, 1.0f, PanFor(Position.x));
        }

        void SmashInputSystem(ECS::FRegistry& Registry)
        {
            FFrameInput& Input = Registry.GetSingleton<FFrameInput>();
            if (!Input.bSmash)
            {
                return;
            }
            Input.bSmash = false;

            const FGameState& State = Registry.GetSingleton<FGameState>();
            if (State.Phase != EPhase::Playing)
            {
                return;
            }

            bool bAnyHeld = false;
            for (auto [Entity, Ball] : Registry.View<FBall>().Each())
            {
                bAnyHeld |= Ball.bHeld;
            }
            if (bAnyHeld)
            {
                return;
            }

            bool bLateSmash = false;
            for (auto [Entity, Body, Ball] : Registry.View<FBody, FBall>().Each())
            {
                if (Ball.SmashGrace > 0.0f && Ball.SmashTimer <= 0.0f)
                {
                    ApplySmash(Registry, Ball, Ball.Velocity, Body.Position);
                    bLateSmash = true;
                }
            }

            if (!bLateSmash)
            {
                for (auto [Entity, Paddle] : Registry.View<FPaddle>().Each())
                {
                    Paddle.SmashArm = kSmashArmWindow;
                }
            }
        }

        void QueueExplosion(ECS::FRegistry& Registry, const FVector2& Origin)
        {
            Registry.GetSingleton<FExplosionQueue>().Pending.push_back(Origin);
        }

        void ApplyPowerUp(ECS::FRegistry& Registry, EPowerUp Type, const FVector2& Position)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            FRandom& Rng = Registry.GetSingleton<FRandom>();

            switch (Type)
            {
            case EPowerUp::Widen:
                for (auto [Entity, Paddle] : Registry.View<FPaddle>().Each())
                {
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
                        SpawnBall(Registry, Origin, Rotated, Ball.Speed, Ball.FireTimer, Ball.BigTimer);
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
                for (auto [Entity, Paddle] : Registry.View<FPaddle>().Each())
                {
                    Paddle.LaserTimer = State.Perks.Has(EPerk::TwinLaser) ? 16.0f : 12.0f;
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
                for (auto [Entity, Paddle] : Registry.View<FPaddle>().Each())
                {
                    Paddle.CatchTimer = State.Perks.Has(EPerk::Sticky) ? 24.0f : 14.0f;
                }
                break;

            case EPowerUp::Magnet:
                for (auto [Entity, Paddle] : Registry.View<FPaddle>().Each())
                {
                    Paddle.MagnetTimer = 14.0f;
                }
                PlaySound(Registry, ESound::Magnet, 1.0f, 1.0f, PanFor(Position.x));
                break;

            case EPowerUp::Shield:
                for (auto [Entity, Paddle] : Registry.View<FPaddle>().Each())
                {
                    Paddle.ShieldCharge = 1.0f;
                    Paddle.ShieldFlash = 1.0f;
                }
                PlaySound(Registry, ESound::ShieldReady, 1.1f, 1.0f);
                break;

            case EPowerUp::Bomb:
                for (int32 Stage = 0; Stage < 3; ++Stage)
                {
                    const float Y = Position.y - 230.0f - float(Stage) * 250.0f;
                    if (Y > 60.0f)
                    {
                        QueueExplosion(Registry, { Position.x, Y });
                    }
                }
                PlaySound(Registry, ESound::Bomb, 1.0f, 1.0f, PanFor(Position.x));
                break;

            case EPowerUp::BigBall:
                for (auto [Entity, Ball] : Registry.View<FBall>().Each())
                {
                    Ball.BigTimer = 11.0f;
                }
                PlaySound(Registry, ESound::MultiBall, 0.6f, 1.0f, PanFor(Position.x));
                break;

            case EPowerUp::Wall:
                State.WallTimer = 10.0f;
                SpawnShockwave(Registry, { kFieldWidth * 0.5f, kWallY }, PowerUpColor(Type), 1100.0f, 0.6f, 0.6f);
                PlaySound(Registry, ESound::WallUp);
                break;

            case EPowerUp::Freeze:
                State.FreezeTimer = 13.0f;
                PlaySound(Registry, ESound::Freeze);
                break;

            case EPowerUp::Jackpot:
                State.JackpotTimer = 10.0f;
                SpawnLabel(Registry, { kFieldWidth * 0.5f, 560.0f }, "JACKPOT", PowerUpColor(Type), 9.0f, 1.4f);
                PlaySound(Registry, ESound::Gold, 1.0f, 1.0f);
                break;

            case EPowerUp::Shrink:
                for (auto [Entity, Paddle] : Registry.View<FPaddle>().Each())
                {
                    Paddle.ShrinkTimer = 9.0f;
                    Paddle.WidenTimer = 0.0f;
                }
                break;

            case EPowerUp::SpeedUp:
                for (auto [Entity, Ball] : Registry.View<FBall>().Each())
                {
                    Ball.Speed = Math::Min(Ball.Speed * 1.35f, State.Perks.MaxBallSpeed());
                }
                break;

            case EPowerUp::Reverse:
                for (auto [Entity, Paddle] : Registry.View<FPaddle>().Each())
                {
                    Paddle.ReverseTimer = 6.0f;
                }
                PlaySound(Registry, ESound::Reverse);
                break;

            case EPowerUp::Blind:
                State.BlindTimer = 5.5f;
                PlaySound(Registry, ESound::Blind);
                break;

            default:
                State.FormationDrop += kBrickPitchY;
                AddTrauma(Registry, 0.5f, 0.6f);
                PlaySound(Registry, ESound::Hazard, 0.7f, 1.0f);
                break;
            }

            const FVector4 Color = PowerUpColor(Type);
            SpawnShockwave(Registry, Position, Color, 280.0f, 0.55f, IsHarmful(Type) ? 0.5f : 0.8f);
            SpawnBurst(Registry, Position, Color, 30, 180.0f, 660.0f, 0.0f);
            SpawnLabel(Registry, { Position.x, Position.y - 40.0f }, PowerUpName(Type), Color, 4.0f, 0.9f);
            AddTrauma(Registry, IsHarmful(Type) ? 0.30f : 0.18f, IsHarmful(Type) ? 0.55f : 0.25f);

            PlaySound(Registry, IsHarmful(Type) ? ESound::PowerDown : ESound::PowerUpCollect,
                1.0f, 1.0f, PanFor(Position.x));
        }

        void PowerUpSystem(ECS::FRegistry& Registry, float Delta)
        {
            const FGameState& State = Registry.GetSingleton<FGameState>();

            struct FPaddleSpan
            {
                FVector2 Position;
                FVector2 HalfSize;
                bool     bMagnet;
            };
            TVector<FPaddleSpan> Paddles;
            for (auto [Entity, Body, Paddle] : Registry.View<FBody, FPaddle>().Each())
            {
                Paddles.push_back({ Body.Position, Body.HalfSize, Paddle.MagnetTimer > 0.0f });
            }
            if (Paddles.empty())
            {
                return;
            }

            const float PerkPull = 380.0f * float(State.Perks.Count(EPerk::Magnetic));

            TVector<ECS::FEntity> Collected;
            TVector<ECS::FEntity> Missed;

            for (auto [Entity, Body, Drop, Visual] : Registry.View<FBody, FPowerUpDrop, FVisual>().Each())
            {
                Drop.Bob += Delta;

                const FPaddleSpan* Nearest = &Paddles[0];
                for (const FPaddleSpan& Span : Paddles)
                {
                    if (Math::Abs(Span.Position.x - Body.Position.x) < Math::Abs(Nearest->Position.x - Body.Position.x))
                    {
                        Nearest = &Span;
                    }
                }

                float Pull = PerkPull;
                for (const FPaddleSpan& Span : Paddles)
                {
                    if (Span.bMagnet)
                    {
                        Pull = Math::Max(Pull, 1100.0f);
                    }
                }

                if (Pull > 0.0f && !IsHarmful(Drop.Type))
                {
                    const float Direction = Nearest->Position.x > Body.Position.x ? 1.0f : -1.0f;
                    Drop.DriftX += (Direction * Pull - Drop.DriftX) * Math::Min(1.0f, Delta * 3.0f);
                }
                else
                {
                    Drop.DriftX -= Drop.DriftX * Math::Min(1.0f, Delta * 2.0f);
                }

                Body.Position.x = Math::Clamp(Body.Position.x + Drop.DriftX * Delta, 30.0f, kFieldWidth - 30.0f);
                Body.Position.y += (IsHarmful(Drop.Type) ? 470.0f : 340.0f) * Delta;
                Body.Rotation = Math::Sin(Drop.Bob * 2.4f) * 0.35f;
                Body.HalfSize = { 30.0f + Math::Sin(Drop.Bob * 7.0f) * 2.5f, 30.0f - Math::Sin(Drop.Bob * 7.0f) * 2.5f };
                Visual.Glow = 1.2f + Math::Sin(Drop.Bob * 9.0f) * 0.4f;

                bool bTouches = false;
                for (const FPaddleSpan& Span : Paddles)
                {
                    bTouches |= Math::Abs(Body.Position.x - Span.Position.x) < Span.HalfSize.x + Body.HalfSize.x &&
                                Math::Abs(Body.Position.y - Span.Position.y) < Span.HalfSize.y + Body.HalfSize.y;
                }

                if (bTouches)
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
                    AwardScore(Registry, 250, Position, PowerUpColor(Type));
                }
            }

            for (const ECS::FEntity Entity : Missed)
            {
                Registry.Destroy(Entity);
            }
        }

        EPowerUp RollPowerUp(FRandom& Rng)
        {
            struct FWeight
            {
                EPowerUp Type;
                int32    Weight;
            };

            static const FWeight kTable[] =
            {
                { EPowerUp::Widen,     11 },
                { EPowerUp::MultiBall, 11 },
                { EPowerUp::Laser,     10 },
                { EPowerUp::Fireball,   8 },
                { EPowerUp::Catch,      7 },
                { EPowerUp::SlowTime,   7 },
                { EPowerUp::Magnet,     6 },
                { EPowerUp::Shield,     5 },
                { EPowerUp::Bomb,       6 },
                { EPowerUp::BigBall,    6 },
                { EPowerUp::Wall,       6 },
                { EPowerUp::Freeze,     5 },
                { EPowerUp::Jackpot,    3 },
                { EPowerUp::ExtraLife,  3 },
                { EPowerUp::Shrink,     8 },
                { EPowerUp::SpeedUp,    7 },
                { EPowerUp::Reverse,    5 },
                { EPowerUp::Blind,      4 },
                { EPowerUp::Drop,       4 },
            };

            int32 Total = 0;
            for (const FWeight& Entry : kTable)
            {
                Total += Entry.Weight;
            }

            int32 Roll = Rng.Below(Total);
            for (const FWeight& Entry : kTable)
            {
                Roll -= Entry.Weight;
                if (Roll < 0)
                {
                    return Entry.Type;
                }
            }
            return EPowerUp::Widen;
        }

        void MaybeDropPowerUp(ECS::FRegistry& Registry, const FVector2& Position, bool bGuaranteedGood)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            FRandom& Rng = Registry.GetSingleton<FRandom>();

            const bool bJackpot = State.JackpotTimer > 0.0f;
            const float Chance = bJackpot ? 1.0f : 0.16f + State.Perks.DropChanceBonus();
            if (!bGuaranteedGood && Rng.Unit() > Chance)
            {
                return;
            }

            const bool bOnlyGood = bGuaranteedGood || bJackpot;
            EPowerUp Type = RollPowerUp(Rng);
            for (int32 Attempt = 0; bOnlyGood && IsHarmful(Type) && Attempt < 12; ++Attempt)
            {
                Type = RollPowerUp(Rng);
            }

            SpawnPowerUp(Registry, Position, Type);
            PlaySound(Registry, ESound::PowerUpDrop, 1.0f, 1.0f, PanFor(Position.x));
        }

        void ResolveExplosions(ECS::FRegistry& Registry)
        {
            FExplosionQueue& Queue = Registry.GetSingleton<FExplosionQueue>();
            if (Queue.Cursor >= int32(Queue.Pending.size()))
            {
                Queue.Pending.clear();
                Queue.Cursor = 0;
                return;
            }

            const float Radius = Registry.GetSingleton<FGameState>().Perks.ExplosionRadius();

            TVector<FVector2> Batch;
            const int32 Last = Math::Min(Queue.Cursor + kExplosionsPerStep,
                Math::Min(int32(Queue.Pending.size()), kMaxChainStages));

            for (int32 Index = Queue.Cursor; Index < Last; ++Index)
            {
                Batch.push_back(Queue.Pending[Index]);
            }
            Queue.Cursor = Last;

            TVector<ECS::FEntity> Caught;
            TVector<ECS::FEntity> DronesCaught;

            for (const FVector2& Origin : Batch)
            {
                Caught.clear();
                for (auto [Entity, Body, Brick] : Registry.View<FBody, FBrick>().Each())
                {
                    if (Brick.Health > 0 && !BrickIsFixture(Brick.Kind) && VectorLength(Body.Position - Origin) <= Radius)
                    {
                        Caught.push_back(Entity);
                    }
                }
                for (auto [Entity, Body, Drone] : Registry.View<FBody, FDrone>().Each())
                {
                    if (VectorLength(Body.Position - Origin) <= Radius)
                    {
                        DronesCaught.push_back(Entity);
                    }
                }

                SpawnShockwave(Registry, Origin, kFireColor, Radius * 2.1f, 0.5f, 1.0f);
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

            for (const ECS::FEntity Entity : DronesCaught)
            {
                if (Registry.IsValid(Entity) && Registry.HasAll<FDrone>(Entity))
                {
                    Registry.Get<FDrone>(Entity).Health = 0;
                }
            }
        }

        void DamageBrick(ECS::FRegistry& Registry, ECS::FEntity Entity, int32 Damage, bool bOverkill, bool bSmash)
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

                const bool bImmune = Kind == EBrickKind::Steel && !bOverkill;
                if (bImmune || BrickIsFixture(Kind))
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
                Brick.Timer = 0.0f;
                bDestroyed = Brick.Health <= 0;
            }

            FGameState& State = Registry.GetSingleton<FGameState>();
            State.Combo += 1;
            State.BestCombo = Math::Max(State.BestCombo, State.Combo);
            State.LevelBestCombo = Math::Max(State.LevelBestCombo, State.Combo);
            State.ComboTimer = 2.4f;

            const int32 Multiplier = Math::Max(1, State.Combo / 3 + 1) + (bSmash ? 1 : 0);
            const float RowPitch = 1.0f + float(kBrickRowCount - 1 - Row) * 0.085f;

            if (!bDestroyed)
            {
                PlaySound(Registry, ESound::BrickHit, RowPitch, 1.0f, PanFor(Position.x));
                State.Score += State.Scaled(25 * Multiplier);
                SpawnBurst(Registry, Position, Base, 10, 120.0f, 480.0f, 900.0f, 0.25f);
                AddTrauma(Registry, 0.05f);
                ApplyBrickVisual(Registry, Entity);
                return;
            }

            const int32 Points = (Kind == EBrickKind::Gold ? 1000 : 100 * Math::Min(MaxHealth, 4)) * Multiplier;
            State.BricksBroken += 1;
            State.BricksAlive = BrickScores(Kind) ? Math::Max(0, State.BricksAlive - 1) : State.BricksAlive;
            State.FlashPulse = Math::Min(1.0f, State.FlashPulse + 0.35f);
            State.Progress = 1.0f - float(State.BricksAlive) / float(Math::Max(State.BricksTotal, 1));

            const float FeverGain = State.Perks.FeverPerBrick();
            const bool bFeverReady = !State.IsFever() && State.FeverMeter + FeverGain >= 1.0f;
            if (!State.IsFever())
            {
                State.FeverMeter = Math::Min(1.0f, State.FeverMeter + FeverGain);
            }

            Registry.Destroy(Entity);

            if (bFeverReady)
            {
                EnterFever(Registry);
            }

            AwardScore(Registry, Points, Position, Kind == EBrickKind::Gold ? BrickBaseColor(FBrick{ .Kind = Kind }) : PopColor,
                Kind == EBrickKind::Gold ? "GOLD" : nullptr, Kind == EBrickKind::Gold ? 4.5f : 3.5f);
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
                QueueExplosion(Registry, Position);
                return;
            }

            if (Kind == EBrickKind::Gold)
            {
                PlaySound(Registry, ESound::Gold, 1.0f, 1.0f, PanFor(Position.x));
                SpawnBurst(Registry, Position, { 1.6f, 1.3f, 0.4f, 1.0f }, 40, 200.0f, 1000.0f, 600.0f, 0.4f);
                MaybeDropPowerUp(Registry, Position, true);
                return;
            }

            PlaySound(Registry, ESound::BrickBreak, RowPitch, 1.0f, PanFor(Position.x));
            MaybeDropPowerUp(Registry, Position, Kind == EBrickKind::Mystery);
        }

        void EnterFever(ECS::FRegistry& Registry)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            State.FeverTimer = State.Perks.FeverDuration();
            State.FeverMeter = 1.0f;

            const FVector2 Center { kFieldWidth * 0.5f, kFieldHeight * 0.55f };
            SpawnShockwave(Registry, Center, { 1.60f, 0.45f, 1.30f, 1.0f }, 1400.0f, 0.9f, 0.9f);
            SpawnBurst(Registry, Center, { 1.50f, 0.55f, 1.40f, 1.0f }, 70, 260.0f, 1500.0f, 220.0f, 0.3f);
            AddTrauma(Registry, 0.45f, 0.7f);
            PlaySound(Registry, ESound::FeverStart);
        }

        void SplitHydra(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            const FBoss Boss = Registry.Get<FBoss>(Entity);
            const FVector2 Position = Registry.Get<FBody>(Entity).Position;
            const FGameState& State = Registry.GetSingleton<FGameState>();
            Registry.Destroy(Entity);

            const float ChildScale = Boss.Generation == 0 ? 0.62f : 0.42f;
            const float ChildHealth = Math::Max(Boss.Health, 4.0f);
            for (int32 Side = 0; Side < 2; ++Side)
            {
                const float Offset = (Side == 0 ? -1.0f : 1.0f) * 220.0f * Boss.Scale;
                const ECS::FEntity Child = SpawnBoss(Registry, EBossKind::Hydra, State.Level, ChildScale,
                    uint8(Boss.Generation + 1), { Position.x + Offset, Position.y }, ChildHealth);
                Registry.Get<FBoss>(Child).DriftPhase = Boss.DriftPhase + (Side == 0 ? 1.6f : -1.6f);
            }

            SpawnShockwave(Registry, Position, { 1.50f, 0.40f, 1.10f, 1.0f }, 700.0f, 0.7f, 1.0f);
            SpawnBurst(Registry, Position, { 1.50f, 0.45f, 1.20f, 1.0f }, 80, 260.0f, 1500.0f, 700.0f, 0.4f);
            SpawnLabel(Registry, { Position.x, Position.y + 80.0f }, "SPLIT", { 1.6f, 0.5f, 1.3f, 1.0f }, 6.0f, 1.2f);
            AddTrauma(Registry, 0.7f, 0.8f);
            AddHitStop(Registry, 0.06f);
            PlaySound(Registry, ESound::BossSplit);
        }

        void DamageBoss(ECS::FRegistry& Registry, ECS::FEntity Entity, float Damage)
        {
            float Remaining = 0.0f;
            float MaxHealth = 1.0f;
            FVector2 Position { 0.0f, 0.0f };
            EBossKind Kind = EBossKind::Warden;
            uint8 Generation = 0;

            {
                FBoss& Boss = Registry.Get<FBoss>(Entity);
                Position = Registry.Get<FBody>(Entity).Position;
                Boss.Flash = 1.0f;

                if (Boss.bArmored)
                {
                    PlaySound(Registry, ESound::SteelHit, 0.8f, 1.0f, PanFor(Position.x));
                    SpawnBurst(Registry, Position, { 0.75f, 0.82f, 1.00f, 1.0f }, 12, 140.0f, 520.0f, 900.0f, 0.3f);
                    AddTrauma(Registry, 0.06f);
                    return;
                }

                Boss.Health -= Damage;
                Remaining  = Boss.Health;
                MaxHealth  = Boss.MaxHealth;
                Kind       = Boss.Kind;
                Generation = Boss.Generation;
            }

            FGameState& State = Registry.GetSingleton<FGameState>();

            if (Remaining > 0.0f)
            {
                State.Score += State.Scaled(60);
                SpawnBurst(Registry, Position, { 1.40f, 0.35f, 0.90f, 1.0f }, 16, 160.0f, 700.0f, 700.0f, 0.3f);
                AddTrauma(Registry, 0.08f);
                PlaySound(Registry, ESound::BossHit, 1.0f + (1.0f - Remaining / MaxHealth) * 0.35f, 1.0f,
                    PanFor(Position.x));

                if (Kind == EBossKind::Hydra && Generation < 2 && Remaining <= MaxHealth * 0.5f)
                {
                    SplitHydra(Registry, Entity);
                }
                return;
            }

            Registry.Destroy(Entity);
            const int32 Points = Generation == 0 ? 5000 : (Generation == 1 ? 1500 : 600);
            AwardScore(Registry, Points, Position, { 1.60f, 0.50f, 1.30f, 1.0f }, Generation == 0 ? "BOSS DOWN" : nullptr, 5.0f);

            for (int32 Ring = 0; Ring < (Generation == 0 ? 4 : 2); ++Ring)
            {
                SpawnShockwave(Registry, Position, { 1.50f, 0.40f, 1.10f, 1.0f },
                    420.0f + float(Ring) * 260.0f, 0.75f, 1.0f);
            }
            SpawnBurst(Registry, Position, { 1.50f, 0.45f, 1.20f, 1.0f }, Generation == 0 ? 120 : 50, 260.0f, 1900.0f, 900.0f, 0.4f);
            AddTrauma(Registry, Generation == 0 ? 1.0f : 0.5f, 1.0f);
            AddHitStop(Registry, 0.075f);
            PlaySound(Registry, ESound::BossDeath, Generation == 0 ? 1.0f : 1.3f);

            if (Generation == 0)
            {
                MaybeDropPowerUp(Registry, Position, true);
            }
        }

        void ArchitectRepair(ECS::FRegistry& Registry, int32 Level)
        {
            bool Occupied[kBrickRowCount][kBrickColumnCount] = {};
            for (auto [Entity, Brick] : Registry.View<FBrick>().Each())
            {
                if (Brick.Row >= 0 && Brick.Row < kBrickRowCount && Brick.Column >= 0 && Brick.Column < kBrickColumnCount)
                {
                    Occupied[Brick.Row][Brick.Column] = true;
                }
            }

            struct FSlot
            {
                int32 Row;
                int32 Column;
            };
            TVector<FSlot> Empty;
            for (int32 Row = 3; Row < kBrickRowCount; ++Row)
            {
                for (int32 Column = 0; Column < kBrickColumnCount; ++Column)
                {
                    if (!Occupied[Row][Column])
                    {
                        Empty.push_back({ Row, Column });
                    }
                }
            }

            if (Empty.empty())
            {
                return;
            }

            FRandom& Rng = Registry.GetSingleton<FRandom>();
            FGameState& State = Registry.GetSingleton<FGameState>();
            const int32 Count = Math::Min<int32>(2, int32(Empty.size()));

            for (int32 Index = 0; Index < Count; ++Index)
            {
                const int32 Pick = Rng.Below(int32(Empty.size()));
                const FSlot Slot = Empty[Pick];
                Empty[Pick] = Empty.back();
                Empty.pop_back();

                const EBrickKind Kind = Rng.Unit() < 0.25f ? EBrickKind::Reinforced : EBrickKind::Normal;
                const ECS::FEntity Entity = SpawnBrick(Registry, Slot.Row, Slot.Column, Kind, Kind == EBrickKind::Reinforced ? 2 : 1);
                Registry.Get<FBrick>(Entity).Flash = 1.0f;
                State.BricksTotal += 1;

                const FVector2 Position = Registry.Get<FBody>(Entity).Position;
                SpawnShockwave(Registry, Position, { 0.80f, 0.40f, 1.40f, 1.0f }, 180.0f, 0.4f, 0.3f);
                SpawnBurst(Registry, Position, { 0.90f, 0.50f, 1.40f, 1.0f }, 14, 80.0f, 400.0f, 0.0f);
            }

            State.Progress = 1.0f - float(State.BricksAlive) / float(Math::Max(State.BricksTotal, 1));
            PlaySound(Registry, ESound::Repair, 1.0f, 0.9f);
            (void)Level;
        }

        void BossSystem(ECS::FRegistry& Registry, float Delta)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            FRandom& Rng = Registry.GetSingleton<FRandom>();

            struct FShot
            {
                FVector2 Origin;
                int32    Count;
            };
            TVector<FShot> Shots;
            bool bRepair = false;
            bool bArmorUp = false;

            for (auto [Entity, Body, Boss, Visual] : Registry.View<FBody, FBoss, FVisual>().Each())
            {
                const float Health = Math::Clamp(Boss.Health / Math::Max(Boss.MaxHealth, 1.0f), 0.0f, 1.0f);
                const float Rate = Boss.Kind == EBossKind::Architect ? 0.32f : (Boss.Kind == EBossKind::Hydra ? 0.55f + float(Boss.Generation) * 0.35f : 0.55f);
                Boss.DriftPhase += Delta * Rate;
                Boss.Flash = Math::Max(0.0f, Boss.Flash - Delta * 3.0f);

                const float Reach = (Boss.Kind == EBossKind::Architect ? 380.0f : 520.0f + (1.0f - Health) * 180.0f) * (Boss.Generation == 0 ? 1.0f : 1.25f);
                const float Bob = Boss.Kind == EBossKind::Hydra ? 60.0f + float(Boss.Generation) * 40.0f : 26.0f;

                Body.Position.x = Math::Clamp(kFieldWidth * 0.5f + Math::Sin(Boss.DriftPhase) * Reach,
                    Body.HalfSize.x + 10.0f, kFieldWidth - Body.HalfSize.x - 10.0f);
                Body.Position.y = kBossTopY + Math::Sin(Boss.DriftPhase * 1.7f) * Bob + State.FormationDrop * 0.5f
                                + (Boss.Generation > 0 ? 40.0f * float(Boss.Generation) : 0.0f);

                if (Boss.Kind == EBossKind::Architect)
                {
                    Boss.ArmorTimer -= Delta;
                    if (Boss.ArmorTimer <= 0.0f)
                    {
                        Boss.bArmored = !Boss.bArmored;
                        Boss.ArmorTimer = Boss.bArmored ? 3.4f : 3.2f;
                        Boss.Flash = 1.0f;
                        bArmorUp |= Boss.bArmored;
                    }

                    Boss.RepairTimer -= Delta;
                    if (Boss.RepairTimer <= 0.0f)
                    {
                        Boss.RepairTimer = Math::Max(2.2f, 4.2f - float(State.Level) * 0.08f);
                        bRepair = true;
                    }
                }

                FVector4 Tint = { 1.20f, 0.28f, 0.75f, 1.0f };
                if (Boss.Kind == EBossKind::Architect)
                {
                    Tint = Boss.bArmored ? FVector4{ 0.70f, 0.78f, 0.95f, 1.0f } : FVector4{ 0.85f, 0.35f, 1.30f, 1.0f };
                }
                else if (Boss.Kind == EBossKind::Hydra)
                {
                    Tint = { 0.40f, 1.10f, 0.55f, 1.0f };
                }

                Visual.Color = { Tint.x + Boss.Flash * 1.4f, Tint.y + Boss.Flash * 1.4f, Tint.z + Boss.Flash * 1.4f, 1.0f };
                Visual.Accent = { Tint.x * 0.22f, Tint.y * 0.22f, Tint.z * 0.30f, 1.0f };
                Visual.Glow = 0.85f + Boss.Flash * 1.6f + (1.0f - Health) * 0.5f;

                Boss.FireTimer -= Delta;
                if (Boss.FireTimer <= 0.0f)
                {
                    const float Base = Boss.Kind == EBossKind::Warden ? 1.9f : (Boss.Kind == EBossKind::Architect ? 3.4f : 2.8f);
                    Boss.FireTimer = Math::Max(0.55f, Base - (1.0f - Health) * 1.1f);
                    Shots.push_back({ { Body.Position.x, Body.Position.y + Body.HalfSize.y },
                        Boss.Kind == EBossKind::Warden ? 1 + int32(Rng.Range(0.0f, 2.4f)) : 1 });
                }
            }

            for (const FShot& Shot : Shots)
            {
                for (int32 Index = 0; Index < Shot.Count; ++Index)
                {
                    const float Spread = (float(Index) - float(Shot.Count - 1) * 0.5f) * 70.0f;
                    SpawnBolt(Registry, { Shot.Origin.x + Spread, Shot.Origin.y },
                        { Rng.Range(-90.0f, 90.0f), 540.0f + Rng.Range(0.0f, 200.0f) }, true);
                }
                PlaySound(Registry, ESound::Hazard, Rng.Range(0.9f, 1.1f), 0.9f, PanFor(Shot.Origin.x));
            }

            if (bArmorUp)
            {
                PlaySound(Registry, ESound::ArmorUp);
            }
            if (bRepair)
            {
                ArchitectRepair(Registry, State.Level);
            }
        }

        void KillDrone(ECS::FRegistry& Registry, ECS::FEntity Entity, bool bScores)
        {
            const FDrone Drone = Registry.Get<FDrone>(Entity);
            const FVector2 Position = Registry.Get<FBody>(Entity).Position;
            const FVector4 Color = DroneColor(Drone.Kind);
            Registry.Destroy(Entity);

            FGameState& State = Registry.GetSingleton<FGameState>();
            State.DronesDowned += 1;

            if (bScores)
            {
                const int32 Points = Drone.Kind == EDroneKind::Orb ? 500 : (Drone.Kind == EDroneKind::Tri ? 350 : 200);
                AwardScore(Registry, Points, Position, Color);
                if (Registry.GetSingleton<FRandom>().Unit() < 0.3f)
                {
                    MaybeDropPowerUp(Registry, Position, true);
                }
            }

            SpawnBurst(Registry, Position, Color, 30, 160.0f, 800.0f, 500.0f, 0.3f);
            SpawnShockwave(Registry, Position, Color, 220.0f, 0.4f, 0.4f);
            AddTrauma(Registry, 0.14f, 0.1f);
            PlaySound(Registry, ESound::DroneDeath, Drone.Kind == EDroneKind::Orb ? 0.8f : 1.0f, 1.0f, PanFor(Position.x));

            if (Drone.Kind == EDroneKind::Orb)
            {
                SpawnDrone(Registry, EDroneKind::Cone, { Position.x - 50.0f, Position.y });
                SpawnDrone(Registry, EDroneKind::Cone, { Position.x + 50.0f, Position.y });
            }
        }

        void DamageDrone(ECS::FRegistry& Registry, ECS::FEntity Entity, int32 Damage)
        {
            FDrone& Drone = Registry.Get<FDrone>(Entity);
            Drone.Health -= Damage;
            Drone.Flash = 1.0f;

            if (Drone.Health > 0)
            {
                const FVector2 Position = Registry.Get<FBody>(Entity).Position;
                SpawnBurst(Registry, Position, DroneColor(Drone.Kind), 10, 100.0f, 400.0f, 300.0f);
                PlaySound(Registry, ESound::DroneHit, 1.0f, 1.0f, PanFor(Position.x));
                return;
            }

            KillDrone(Registry, Entity, true);
        }

        bool DronesEnabled(const FGameState& State)
        {
            switch (State.Mode)
            {
            case EGameMode::Endless:  return State.Level >= 2;
            case EGameMode::BossRush: return State.Level >= 2;
            default:                  return State.Level >= 3;
            }
        }

        void DroneSystem(ECS::FRegistry& Registry, float Delta)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            FRandom& Rng = Registry.GetSingleton<FRandom>();

            if (State.Phase == EPhase::Playing && DronesEnabled(State))
            {
                State.DroneTimer -= Delta;
                const int32 Alive = int32(Registry.View<FDrone>().NumCandidates());
                const int32 Cap = Math::Min(1 + State.Level / 3, 5);

                if (State.DroneTimer <= 0.0f && Alive < Cap)
                {
                    State.DroneTimer = Math::Max(4.0f, 10.5f - float(State.Level) * 0.35f);
                    const float Roll = Rng.Unit();
                    const EDroneKind Kind = State.Level >= 8 && Roll < 0.2f ? EDroneKind::Orb
                                          : (State.Level >= 5 && Roll < 0.5f ? EDroneKind::Tri : EDroneKind::Cone);
                    const float SpawnY = Math::Clamp(LowestBrickEdge(Registry) + 70.0f, 120.0f, 620.0f);
                    SpawnDrone(Registry, Kind, { Rng.Range(120.0f, kFieldWidth - 120.0f), SpawnY });
                    PlaySound(Registry, ESound::Hazard, 1.4f, 0.6f);
                }
            }

            struct FPaddleSpan
            {
                FVector2 Position;
                FVector2 HalfSize;
                ECS::FEntity Entity;
            };
            TVector<FPaddleSpan> Paddles;
            for (auto [Entity, Body, Paddle] : Registry.View<FBody, FPaddle>().Each())
            {
                Paddles.push_back({ Body.Position, Body.HalfSize, Entity });
            }

            TVector<ECS::FEntity> Doomed;
            TVector<ECS::FEntity> PaddleStruck;

            for (auto [Entity, Body, Drone, Visual] : Registry.View<FBody, FDrone, FVisual>().Each())
            {
                Drone.Age += Delta;
                Drone.Flash = Math::Max(0.0f, Drone.Flash - Delta * 4.0f);

                if (Drone.Health <= 0)
                {
                    Doomed.push_back(Entity);
                    continue;
                }

                switch (Drone.Kind)
                {
                case EDroneKind::Cone:
                    Drone.Velocity = { Math::Sin(Drone.Age * 1.7f + Drone.Phase) * 190.0f, 62.0f };
                    break;
                case EDroneKind::Tri:
                    Drone.Velocity = { (Math::Sin(Drone.Age * 3.1f + Drone.Phase) >= 0.0f ? 1.0f : -1.0f) * 280.0f, 115.0f };
                    break;
                default:
                    Drone.Velocity = { Math::Sin(Drone.Age * 0.9f + Drone.Phase) * 90.0f, 42.0f };
                    break;
                }

                Body.Position += Drone.Velocity * Delta;
                Body.Position.x = Math::Clamp(Body.Position.x, Drone.Radius, kFieldWidth - Drone.Radius);
                Body.Rotation += Delta * (Drone.Kind == EDroneKind::Tri ? 4.0f : 1.4f);
                Body.HalfSize = { Drone.Radius, Drone.Radius };

                const FVector4 Color = DroneColor(Drone.Kind);
                Visual.Color = { Color.x + Drone.Flash * 1.5f, Color.y + Drone.Flash * 1.5f, Color.z + Drone.Flash * 1.5f, 1.0f };
                Visual.Glow = 0.9f + Drone.Flash * 1.2f + Math::Sin(Drone.Age * 6.0f) * 0.15f;

                if (Body.Position.y > kFieldHeight + 80.0f)
                {
                    Doomed.push_back(Entity);
                    continue;
                }

                for (const FPaddleSpan& Span : Paddles)
                {
                    if (Math::Abs(Body.Position.x - Span.Position.x) < Span.HalfSize.x + Drone.Radius * 0.8f &&
                        Math::Abs(Body.Position.y - Span.Position.y) < Span.HalfSize.y + Drone.Radius * 0.8f)
                    {
                        PaddleStruck.push_back(Entity);
                        Registry.Get<FPaddle>(Span.Entity).HitFlash = 1.0f;
                        break;
                    }
                }
            }

            for (const ECS::FEntity Entity : Doomed)
            {
                if (Registry.IsValid(Entity) && Registry.HasAll<FDrone>(Entity))
                {
                    if (Registry.Get<FDrone>(Entity).Health <= 0)
                    {
                        KillDrone(Registry, Entity, true);
                    }
                    else
                    {
                        Registry.Destroy(Entity);
                    }
                }
            }

            for (const ECS::FEntity Entity : PaddleStruck)
            {
                if (Registry.IsValid(Entity) && Registry.HasAll<FDrone>(Entity))
                {
                    AddTrauma(Registry, 0.25f, 0.3f);
                    KillDrone(Registry, Entity, false);
                }
            }
        }

        void BoltSystem(ECS::FRegistry& Registry, float Delta)
        {
            struct FPaddleSpan
            {
                FVector2     Position;
                FVector2     HalfSize;
                ECS::FEntity Entity;
            };
            TVector<FPaddleSpan> Paddles;
            for (auto [Entity, Body, Paddle] : Registry.View<FBody, FPaddle>().Each())
            {
                Paddles.push_back({ Body.Position, Body.HalfSize, Entity });
            }

            TVector<ECS::FEntity> Doomed;
            TVector<ECS::FEntity> Struck;
            TVector<ECS::FEntity> BossHits;
            TVector<ECS::FEntity> DroneHits;
            TVector<ECS::FEntity> StruckPaddles;

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
                    for (const FPaddleSpan& Span : Paddles)
                    {
                        if (Math::Abs(Body.Position.x - Span.Position.x) < Span.HalfSize.x + Body.HalfSize.x &&
                            Math::Abs(Body.Position.y - Span.Position.y) < Span.HalfSize.y + Body.HalfSize.y)
                        {
                            StruckPaddles.push_back(Span.Entity);
                            Doomed.push_back(Entity);
                            break;
                        }
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

                for (auto [DroneEntity, DroneBody, Drone] : Registry.View<FBody, FDrone>().Each())
                {
                    if (VectorLength(Body.Position - DroneBody.Position) < Drone.Radius + Body.HalfSize.y * 0.5f)
                    {
                        DroneHits.push_back(DroneEntity);
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
                    if (Brick.Health <= 0 || !Brick.IsSolid())
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

            for (const ECS::FEntity Entity : DroneHits)
            {
                if (Registry.IsValid(Entity) && Registry.HasAll<FDrone>(Entity))
                {
                    DamageDrone(Registry, Entity, 1);
                }
            }

            const bool bBulwark = Registry.GetSingleton<FGameState>().Perks.Has(EPerk::Bulwark);
            for (const ECS::FEntity PaddleEntity : StruckPaddles)
            {
                if (!Registry.IsValid(PaddleEntity) || !Registry.HasAll<FPaddle>(PaddleEntity))
                {
                    continue;
                }

                FPaddle& Paddle = Registry.Get<FPaddle>(PaddleEntity);
                const FVector2 Position = Registry.Get<FBody>(PaddleEntity).Position;
                const bool bAbsorbed = Paddle.ShieldCharge >= 1.0f || bBulwark;

                Paddle.ShieldCharge = bBulwark && Paddle.ShieldCharge < 1.0f ? Paddle.ShieldCharge : 0.0f;
                Paddle.ShieldFlash = 1.0f;
                if (!bAbsorbed)
                {
                    Paddle.ShrinkTimer = Math::Max(Paddle.ShrinkTimer, 6.0f);
                    Paddle.WidenTimer = 0.0f;
                }

                SpawnBurst(Registry, Position, { 1.60f, 0.90f, 0.25f, 1.0f }, 24, 160.0f, 700.0f, 400.0f);
                AddTrauma(Registry, 0.32f, 0.45f);
                PlaySound(Registry, bAbsorbed ? ESound::ShieldSave : ESound::PowerDown);
            }
        }

        void BrickBehaviorSystem(ECS::FRegistry& Registry, float Delta)
        {
            const FGameState& State = Registry.GetSingleton<FGameState>();
            bool bRegenerated = false;

            for (auto [Entity, Brick] : Registry.View<FBrick>().Each())
            {
                switch (Brick.Kind)
                {
                case EBrickKind::Ghost:
                {
                    const float Wave = Math::Sin(State.Elapsed * 1.3f + Brick.Phase * 1.9f);
                    Brick.Solidity = Math::Clamp(Wave * 2.5f + 0.5f, 0.0f, 1.0f);
                    break;
                }
                case EBrickKind::Regen:
                    if (Brick.Health < Brick.MaxHealth && Brick.Health > 0)
                    {
                        Brick.Timer += Delta;
                        if (Brick.Timer >= 3.2f)
                        {
                            Brick.Timer = 0.0f;
                            Brick.Health += 1;
                            Brick.Flash = 0.8f;
                            bRegenerated = true;
                            ApplyBrickVisual(Registry, Entity);
                        }
                    }
                    break;
                case EBrickKind::Mover:
                    Brick.Slide = Math::Sin(State.Elapsed * 1.15f + float(Brick.Row) * 1.3f) * 72.0f;
                    break;
                default:
                    break;
                }
            }

            if (bRegenerated)
            {
                PlaySound(Registry, ESound::Regen, 1.0f, 0.6f);
            }
        }

        struct FPaddleSnap
        {
            ECS::FEntity Entity;
            FVector2     Position;
            FVector2     HalfSize;
            float        Velocity;
            uint8        Index;
            bool         bCatch;
            bool         bSmash;
            bool         bWasHit;
            bool         bSmashed;
        };

        // Local copies, so the ball state written back is the state this step actually simulated.
        void BallSystem(ECS::FRegistry& Registry, float Delta)
        {
            TVector<FPaddleSnap> Paddles;
            for (auto [Entity, Body, Paddle] : Registry.View<FBody, FPaddle>().Each())
            {
                Paddles.push_back({ Entity, Body.Position, Body.HalfSize, Paddle.Velocity, Paddle.PlayerIndex,
                    Paddle.CatchTimer > 0.0f, Paddle.SmashArm > 0.0f, false, false });
            }
            if (Paddles.empty())
            {
                return;
            }

            FGameState& State = Registry.GetSingleton<FGameState>();
            FRandom& Rng = Registry.GetSingleton<FRandom>();
            const float MaxSpeed = State.Perks.MaxBallSpeed();
            const int32 BaseDamage = State.Perks.BallDamage();
            const bool bWall = State.WallTimer > 0.0f;

            TVector<FVector2> Wells;
            for (auto [Entity, Body, Brick] : Registry.View<FBody, FBrick>().Each())
            {
                if (Brick.Kind == EBrickKind::Gravity && Brick.Health > 0)
                {
                    Wells.push_back(Body.Position);
                }
            }

            TVector<ECS::FEntity> Balls;
            for (const ECS::FEntity Entity : Registry.View<FBall>())
            {
                Balls.push_back(Entity);
            }

            struct FBrickHit
            {
                ECS::FEntity Entity;
                int32        Damage;
                bool         bSmash;
            };
            TVector<FBrickHit> DoomedBricks;
            TVector<ECS::FEntity> BossTargets;
            TVector<ECS::FEntity> DroneTargets;
            TVector<ECS::FEntity> LostBalls;
            TVector<ECS::FEntity> BumpersHit;

            for (const ECS::FEntity Entity : Balls)
            {
                FBall Ball = Registry.Get<FBall>(Entity);
                if (Ball.bHeld)
                {
                    continue;
                }

                FVector2 Position = Registry.Get<FBody>(Entity).Position;
                FVector2 Velocity = Ball.Velocity;
                const float Radius = Ball.Radius();

                Ball.Squash = Math::Max(0.0f, Ball.Squash - Delta * 6.0f);
                Ball.FireTimer = Math::Max(0.0f, Ball.FireTimer - Delta);
                Ball.BigTimer = Math::Max(0.0f, Ball.BigTimer - Delta);
                Ball.SmashTimer = Math::Max(0.0f, Ball.SmashTimer - Delta);
                Ball.SmashGrace = Math::Max(0.0f, Ball.SmashGrace - Delta);
                Ball.PortalCooldown = Math::Max(0.0f, Ball.PortalCooldown - Delta);
                Ball.Spin *= 1.0f - Math::Min(1.0f, Delta * 0.9f);

                Velocity.x += Ball.Spin * Delta;

                const int32 Damage = BaseDamage + (Ball.SmashTimer > 0.0f ? 1 : 0) + (Ball.BigTimer > 0.0f ? 1 : 0);

                const float Travel = Math::Max(Math::Abs(Velocity.x), Math::Abs(Velocity.y)) * Delta;
                const int32 Steps = Math::Clamp(int32(Travel / (Radius * 0.55f)) + 1, 1, 10);
                const float StepDelta = Delta / float(Steps);
                bool bCaught = false;

                for (int32 Step = 0; Step < Steps; ++Step)
                {
                    for (const FVector2& Well : Wells)
                    {
                        const FVector2 Offset = Well - Position;
                        const float Distance = VectorLength(Offset);
                        if (Distance > 8.0f && Distance < 330.0f)
                        {
                            const float Strength = (1.0f - Distance / 330.0f) * 3200.0f;
                            Velocity += Offset * (Strength * StepDelta / Distance);
                        }
                    }

                    Position += Velocity * StepDelta;

                    bool bHitWall = false;
                    if (Position.x < Radius && Velocity.x < 0.0f)
                    {
                        Position.x = Radius;
                        Velocity.x = -Velocity.x;
                        bHitWall = true;
                    }
                    else if (Position.x > kFieldWidth - Radius && Velocity.x > 0.0f)
                    {
                        Position.x = kFieldWidth - Radius;
                        Velocity.x = -Velocity.x;
                        bHitWall = true;
                    }

                    if (Position.y < Radius && Velocity.y < 0.0f)
                    {
                        Position.y = Radius;
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

                    if (bWall && Position.y > kWallY - Radius && Velocity.y > 0.0f)
                    {
                        Position.y = kWallY - Radius;
                        Velocity.y = -Velocity.y;
                        Ball.Squash = 1.0f;
                        PlaySound(Registry, ESound::WallBounce, Rng.Range(0.95f, 1.05f), 1.0f, PanFor(Position.x));
                        SpawnBurst(Registry, Position, PowerUpColor(EPowerUp::Wall), 14, 120.0f, 520.0f, -300.0f);
                        SpawnShockwave(Registry, Position, PowerUpColor(EPowerUp::Wall), 150.0f, 0.3f, 0.2f);
                        AddTrauma(Registry, 0.08f);
                    }

                    for (FPaddleSnap& Paddle : Paddles)
                    {
                        const bool bHitsPaddle =
                            Velocity.y > 0.0f &&
                            Math::Abs(Position.x - Paddle.Position.x) < Paddle.HalfSize.x + Radius &&
                            Math::Abs(Position.y - Paddle.Position.y) < Paddle.HalfSize.y + Radius;

                        if (!bHitsPaddle)
                        {
                            continue;
                        }

                        Position.y = Paddle.Position.y - Paddle.HalfSize.y - Radius;

                        const float Offset = Math::Clamp((Position.x - Paddle.Position.x) / Paddle.HalfSize.x, -1.0f, 1.0f);
                        const float Angle = Math::Clamp(Offset * 1.0996f + Paddle.Velocity * 0.00012f, -1.2217f, 1.2217f);

                        Ball.Speed = Math::Min(Ball.Speed + (State.Perks.Has(EPerk::SoftBall) ? 7.0f : 14.0f), MaxSpeed);
                        Velocity = { Math::Sin(Angle) * Ball.Speed, -Math::Cos(Angle) * Ball.Speed };
                        Ball.Spin = Math::Clamp(Paddle.Velocity * 0.55f, -900.0f, 900.0f);
                        Ball.Squash = 1.0f;

                        Paddle.bWasHit = true;
                        State.Combo = 0;
                        AddTrauma(Registry, 0.07f);
                        SpawnBurst(Registry, Position, { 0.40f, 0.95f, 1.00f, 1.0f }, 14, 140.0f, 540.0f, 600.0f);
                        SpawnShockwave(Registry, Position, { 0.40f, 0.90f, 1.00f, 1.0f }, 130.0f, 0.30f, 0.18f);

                        if (Paddle.bCatch)
                        {
                            bCaught = true;
                            Ball.HeldBy = Paddle.Index;
                            Ball.HeldOffset = Math::Clamp(Position.x - Paddle.Position.x,
                                -Paddle.HalfSize.x * 0.85f, Paddle.HalfSize.x * 0.85f);
                            PlaySound(Registry, ESound::Catch, 1.0f, 1.0f, PanFor(Position.x));
                            break;
                        }

                        if (Paddle.bSmash)
                        {
                            Paddle.bSmash = false;
                            Paddle.bSmashed = true;
                            ApplySmash(Registry, Ball, Velocity, Position);
                        }
                        else
                        {
                            Ball.SmashGrace = kSmashGraceWindow;
                            PlaySound(Registry, ESound::PaddleHit, Rng.Range(0.94f, 1.08f), 1.0f, PanFor(Position.x));
                        }
                        break;
                    }

                    if (bCaught)
                    {
                        break;
                    }

                    ECS::FEntity BossStruck = ECS::NullEntity;
                    for (auto [BossEntity, BossBody, Boss] : Registry.View<FBody, FBoss>().Each())
                    {
                        const FVector2 Offset = Position - BossBody.Position;
                        const float OverlapX = BossBody.HalfSize.x + Radius - Math::Abs(Offset.x);
                        const float OverlapY = BossBody.HalfSize.y + Radius - Math::Abs(Offset.y);
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

                    ECS::FEntity DroneStruck = ECS::NullEntity;
                    for (auto [DroneEntity, DroneBody, Drone] : Registry.View<FBody, FDrone>().Each())
                    {
                        const FVector2 Offset = Position - DroneBody.Position;
                        const float Distance = VectorLength(Offset);
                        if (Distance >= Drone.Radius + Radius || Distance < 0.001f)
                        {
                            continue;
                        }

                        const FVector2 Normal = Offset * (1.0f / Distance);
                        const float Dot = Velocity.x * Normal.x + Velocity.y * Normal.y;
                        if (Dot < 0.0f)
                        {
                            Velocity -= Normal * (2.0f * Dot);
                        }
                        Position = DroneBody.Position + Normal * (Drone.Radius + Radius + 1.0f);
                        DroneStruck = DroneEntity;
                        break;
                    }

                    if (!DroneStruck.IsNull())
                    {
                        DroneTargets.push_back(DroneStruck);
                        Ball.Squash = 1.0f;
                        break;
                    }

                    ECS::FEntity Struck = ECS::NullEntity;
                    bool bPassThrough = false;
                    bool bWarped = false;

                    for (auto [BrickEntity, BrickBody, Brick] : Registry.View<FBody, FBrick>().Each())
                    {
                        if (Brick.Health <= 0 || !Brick.IsSolid())
                        {
                            continue;
                        }

                        if (Brick.Kind == EBrickKind::Bumper)
                        {
                            const FVector2 Offset = Position - BrickBody.Position;
                            const float Distance = VectorLength(Offset);
                            const float Reach = BrickBody.HalfSize.x + Radius;
                            if (Distance >= Reach || Distance < 0.001f)
                            {
                                continue;
                            }

                            const FVector2 Normal = Offset * (1.0f / Distance);
                            Ball.Speed = Math::Min(Ball.Speed * 1.06f + 40.0f, MaxSpeed);
                            Velocity = Normal * Ball.Speed;
                            Position = BrickBody.Position + Normal * (Reach + 1.0f);
                            Ball.Squash = 1.0f;
                            BumpersHit.push_back(BrickEntity);
                            Struck = ECS::NullEntity;
                            bWarped = true;
                            break;
                        }

                        const FVector2 Offset = Position - BrickBody.Position;
                        const float OverlapX = BrickBody.HalfSize.x + Radius - Math::Abs(Offset.x);
                        const float OverlapY = BrickBody.HalfSize.y + Radius - Math::Abs(Offset.y);
                        if (OverlapX <= 0.0f || OverlapY <= 0.0f)
                        {
                            continue;
                        }

                        if (Brick.Kind == EBrickKind::Portal)
                        {
                            if (Ball.PortalCooldown > 0.0f || Brick.PortalPair < 0)
                            {
                                continue;
                            }

                            FVector2 Exit = BrickBody.Position;
                            bool bFound = false;
                            for (auto [OtherEntity, OtherBody, Other] : Registry.View<FBody, FBrick>().Each())
                            {
                                if (OtherEntity != BrickEntity && Other.Kind == EBrickKind::Portal && Other.PortalPair == Brick.PortalPair)
                                {
                                    Exit = OtherBody.Position;
                                    bFound = true;
                                    break;
                                }
                            }
                            if (!bFound)
                            {
                                continue;
                            }

                            const FVector2 Direction = Normalized(Velocity);
                            SpawnShockwave(Registry, Position, kPortalColor, 200.0f, 0.4f, 0.5f);
                            Position = Exit + Direction * (kBrickWidth * 0.5f + Radius + 6.0f);
                            SpawnShockwave(Registry, Position, kPortalColor, 260.0f, 0.5f, 0.7f);
                            SpawnBurst(Registry, Position, kPortalColor, 24, 120.0f, 600.0f, 0.0f);
                            Ball.PortalCooldown = 0.35f;
                            Ball.TrailCount = 0;
                            for (int32 Node = 0; Node < kBallTrailNodes; ++Node)
                            {
                                Ball.Trail[Node] = Position;
                            }
                            PlaySound(Registry, ESound::Portal, 1.0f, 1.0f, PanFor(Position.x));
                            AddTrauma(Registry, 0.12f, 0.3f);
                            bWarped = true;
                            break;
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

                    if (bWarped)
                    {
                        break;
                    }

                    if (!Struck.IsNull())
                    {
                        DoomedBricks.push_back({ Struck, Damage, Ball.SmashTimer > 0.0f });
                        Ball.Speed = Math::Min(Ball.Speed + 6.0f, MaxSpeed);
                        Ball.Squash = 1.0f;
                        Velocity = Velocity * (Ball.Speed / Math::Max(VectorLength(Velocity), 1.0f));

                        if (!bPassThrough)
                        {
                            break;
                        }
                    }

                    if (Position.y > kFieldHeight + Radius * 3.0f)
                    {
                        break;
                    }
                }

                // A near-horizontal path never clears a row, so bend it back toward vertical.
                if (Math::Abs(Velocity.y) < Ball.Speed * 0.22f)
                {
                    Velocity.y = (Velocity.y >= 0.0f ? 1.0f : -1.0f) * Ball.Speed * 0.22f;
                }
                Velocity = Velocity * (Ball.Speed / Math::Max(VectorLength(Velocity), 1.0f));

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
                const bool bSmashing = Ball.SmashTimer > 0.0f;
                const float TrailStep = bFiery || bSmashing ? 0.0035f : 0.006f;

                Ball.TrailBudget += Delta;
                while (Ball.TrailBudget > TrailStep)
                {
                    Ball.TrailBudget -= TrailStep;
                    if (ParticleBudget(Registry, 1) <= 0)
                    {
                        break;
                    }

                    const FVector2 Jitter { Rng.Range(-4.0f, 4.0f), Rng.Range(-4.0f, 4.0f) };
                    if (bSmashing)
                    {
                        SpawnParticle(Registry, Position + Jitter, Velocity * -0.04f,
                            { 2.20f, 2.00f, 1.50f, 1.0f }, { 0.60f, 0.30f, 0.05f, 0.0f },
                            Radius * 1.3f, 0.0f, 0.36f, 2.5f, 0.0f, EQuadKind::Glow);
                    }
                    else if (bFiery)
                    {
                        SpawnParticle(Registry, Position + Jitter, Velocity * -0.05f + FVector2{ 0.0f, -70.0f },
                            { 1.90f, 0.85f, 0.25f, 1.0f }, { 0.55f, 0.05f, 0.02f, 0.0f },
                            Radius * 1.25f, 0.0f, 0.42f, 2.2f, -220.0f, EQuadKind::Glow);
                    }
                    else
                    {
                        SpawnParticle(Registry, Position + Jitter, Velocity * -0.06f,
                            { 0.60f, 1.00f, 1.55f, 1.0f }, { 0.18f, 0.05f, 0.35f, 0.0f },
                            Radius * 0.95f, 0.0f, 0.30f, 3.0f, 0.0f, EQuadKind::Glow);
                    }
                }

                const float Stretch = 1.0f + Math::Min(VectorLength(Velocity) / kBallMaxSpeed, 1.0f) * 0.22f - Ball.Squash * 0.35f;

                Registry.Get<FBall>(Entity) = Ball;

                FBody& Body = Registry.Get<FBody>(Entity);
                Body.Position = Position;
                Body.Rotation = Math::Atan2(Velocity.y, Velocity.x);
                Body.HalfSize = { Radius * Stretch, Radius * (2.0f - Stretch) };

                FVisual& Visual = Registry.Get<FVisual>(Entity);
                Visual.Glow   = (bSmashing ? 2.0f : (bFiery ? 1.5f : 1.0f)) + Ball.Squash * 0.7f;
                if (bSmashing)
                {
                    Visual.Color  = { 2.40f, 2.20f, 1.70f, 1.0f };
                    Visual.Accent = { 0.90f, 0.60f, 0.10f, 1.0f };
                }
                else if (bFiery)
                {
                    Visual.Color  = { 2.10f, 1.05f, 0.35f, 1.0f };
                    Visual.Accent = { 0.85f, 0.16f, 0.03f, 1.0f };
                }
                else if (Ball.BigTimer > 0.0f)
                {
                    Visual.Color  = { 1.80f, 1.90f, 2.20f, 1.0f };
                    Visual.Accent = { 0.35f, 0.55f, 0.95f, 1.0f };
                }
                else
                {
                    Visual.Color  = { 1.45f, 1.65f, 2.05f, 1.0f };
                    Visual.Accent = { 0.18f, 0.45f, 0.85f, 1.0f };
                }

                if (Position.y > kFieldHeight + Radius * 3.0f)
                {
                    LostBalls.push_back(Entity);
                }
            }

            for (const FPaddleSnap& Paddle : Paddles)
            {
                if (!Registry.IsValid(Paddle.Entity))
                {
                    continue;
                }
                FPaddle& Component = Registry.Get<FPaddle>(Paddle.Entity);
                if (Paddle.bWasHit)
                {
                    Component.HitFlash = 1.0f;
                }
                if (Paddle.bSmashed)
                {
                    Component.SmashArm = 0.0f;
                    Component.SmashFlash = 1.0f;
                }
            }

            for (const FBrickHit& Hit : DoomedBricks)
            {
                if (Registry.IsValid(Hit.Entity) && Registry.HasAll<FBrick>(Hit.Entity))
                {
                    DamageBrick(Registry, Hit.Entity, Hit.Damage, false, Hit.bSmash);
                }
            }

            for (const ECS::FEntity Entity : BumpersHit)
            {
                if (Registry.IsValid(Entity) && Registry.HasAll<FBrick>(Entity))
                {
                    FBrick& Brick = Registry.Get<FBrick>(Entity);
                    Brick.Flash = 1.0f;
                    const FVector2 Position = Registry.Get<FBody>(Entity).Position;
                    AwardScore(Registry, 150, Position, BrickBaseColor(Brick));
                    SpawnShockwave(Registry, Position, BrickBaseColor(Brick), 220.0f, 0.35f, 0.5f);
                    SpawnBurst(Registry, Position, BrickBaseColor(Brick), 18, 160.0f, 700.0f, 300.0f);
                    AddTrauma(Registry, 0.12f, 0.1f);
                    PlaySound(Registry, ESound::Bumper, Rng.Range(0.95f, 1.1f), 1.0f, PanFor(Position.x));
                }
            }

            for (const ECS::FEntity Entity : BossTargets)
            {
                if (Registry.IsValid(Entity) && Registry.HasAll<FBoss>(Entity))
                {
                    DamageBoss(Registry, Entity, float(BaseDamage));
                }
            }

            for (const ECS::FEntity Entity : DroneTargets)
            {
                if (Registry.IsValid(Entity) && Registry.HasAll<FDrone>(Entity))
                {
                    DamageDrone(Registry, Entity, BaseDamage);
                }
            }

            for (const ECS::FEntity Entity : LostBalls)
            {
                const float LostX = Registry.Get<FBody>(Entity).Position.x;
                Registry.Destroy(Entity);
                SpawnBurst(Registry, { LostX, kFieldHeight - 6.0f }, { 1.0f, 0.25f, 0.35f, 1.0f }, 30, 200.0f, 720.0f, -400.0f);
            }
        }

        void VaultSystem(ECS::FRegistry& Registry, float Delta)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();

            bool bAny = false;
            const float TopEdge = HighestScoringBrickEdge(Registry, bAny);

            bool bBehind = false;
            FVector2 Where { kFieldWidth * 0.5f, 120.0f };
            if (bAny)
            {
                for (auto [Entity, Body, Ball] : Registry.View<FBody, FBall>().Each())
                {
                    if (!Ball.bHeld && Body.Position.y + Ball.Radius() < TopEdge - 6.0f)
                    {
                        bBehind = true;
                        Where = Body.Position;
                    }
                }
            }

            if (bBehind)
            {
                if (State.VaultTimer <= 0.0f)
                {
                    State.Vaults += 1;
                    SpawnLabel(Registry, { Where.x, Where.y + 50.0f }, "VAULT", kVaultColor, 6.0f, 1.0f);
                    SpawnShockwave(Registry, Where, kVaultColor, 500.0f, 0.6f, 0.7f);
                    AddTrauma(Registry, 0.2f, 0.4f);
                    PlaySound(Registry, ESound::VaultEnter, 1.0f, 1.0f, PanFor(Where.x));
                }
                State.VaultTimer = kVaultLinger;
            }
            else
            {
                State.VaultTimer = Math::Max(0.0f, State.VaultTimer - Delta);
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
            const float Frozen = State.FreezeTimer > 0.0f ? Math::Min(1.0f, State.FreezeTimer * 2.0f) : 0.0f;

            for (auto [Entity, Body, Brick, Visual] : Registry.View<FBody, FBrick, FVisual>().Each())
            {
                Brick.Flash = Math::Max(0.0f, Brick.Flash - Delta * 3.4f);
                Brick.Shove = Math::Max(0.0f, Brick.Shove - Delta * 5.0f);

                const FVector2 Rest = BrickCenter(Brick.Row, Brick.Column, State);
                const float Wave = Frozen > 0.5f ? 0.0f : Math::Sin(State.Elapsed * 1.6f + Brick.Phase) * 2.4f;
                const float Panic = State.Progress * Math::Sin(State.Elapsed * 7.0f + Brick.Phase * 2.0f) * 3.5f * (1.0f - Frozen);
                Body.Position = { Rest.x + Brick.Slide, Rest.y + Wave + Panic };

                if (Brick.Kind == EBrickKind::Bumper)
                {
                    Body.HalfSize = { 24.0f + Brick.Flash * 6.0f, 24.0f + Brick.Flash * 6.0f };
                }

                const float Damage = 1.0f - float(Brick.Health) / float(Math::Max(1, Brick.MaxHealth));
                FVector4 Base = BrickBaseColor(Brick);
                Base = LerpColor(Base, kIceColor, Frozen * 0.6f);
                const float Dim = 0.88f - (Brick.Kind == EBrickKind::Steel ? 0.0f : Damage * 0.30f);
                const float Flash = Brick.Flash * 1.1f;

                float Pulse = 0.0f;
                switch (Brick.Kind)
                {
                case EBrickKind::Explosive: Pulse = 0.22f + 0.22f * Math::Sin(State.Elapsed * 6.5f + Brick.Phase); break;
                case EBrickKind::Mystery:   Pulse = 0.18f + 0.18f * Math::Sin(State.Elapsed * 3.5f + Brick.Phase * 1.7f); break;
                case EBrickKind::Gold:      Pulse = 0.30f + 0.25f * Math::Sin(State.Elapsed * 8.0f + Brick.Phase); break;
                case EBrickKind::Gravity:   Pulse = 0.25f + 0.25f * Math::Sin(State.Elapsed * 4.0f + Brick.Phase); break;
                case EBrickKind::Portal:    Pulse = 0.20f + 0.20f * Math::Sin(State.Elapsed * 5.0f + Brick.Phase); break;
                case EBrickKind::Regen:     Pulse = Brick.Health < Brick.MaxHealth ? 0.15f + 0.15f * Math::Sin(State.Elapsed * 9.0f) : 0.0f; break;
                default: break;
                }

                const float Alpha = Brick.Kind == EBrickKind::Ghost ? 0.18f + 0.82f * Brick.Solidity : 1.0f;
                Visual.Color = { Base.x * Dim + Flash + Pulse, Base.y * Dim + Flash + Pulse * 0.4f,
                                 Base.z * Dim + Flash + Pulse * 0.8f, Alpha };
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

        void GradeLevel(ECS::FRegistry& Registry)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();

            int32 Marks = 0;
            if (State.LevelLivesLost == 0) { Marks += 2; }
            if (State.LevelTime < 40.0f) { Marks += 2; } else if (State.LevelTime < 75.0f) { Marks += 1; }
            if (State.LevelBestCombo >= 12) { Marks += 2; } else if (State.LevelBestCombo >= 6) { Marks += 1; }
            if (State.LevelSmashes >= 2) { Marks += 1; }

            State.Grade = Marks >= 6 ? 'S' : (Marks >= 4 ? 'A' : (Marks >= 2 ? 'B' : 'C'));
            const int32 Bonus = State.Grade == 'S' ? 4000 : (State.Grade == 'A' ? 2000 : (State.Grade == 'B' ? 750 : 0));
            State.GradeBonus = int32(float(Bonus) * State.Perks.ScoreScale());
            State.Score += State.GradeBonus;

            const float Pitch = State.Grade == 'S' ? 1.4f : (State.Grade == 'A' ? 1.2f : (State.Grade == 'B' ? 1.0f : 0.8f));
            PlaySound(Registry, ESound::Grade, Pitch);
        }

        void OfferDraft(ECS::FRegistry& Registry)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            FRandom& Rng = Registry.GetSingleton<FRandom>();

            TVector<EPerk> Pool;
            for (int32 Index = 0; Index < int32(EPerk::Count); ++Index)
            {
                const EPerk Perk = EPerk(Index);
                if (State.Perks.IsMaxed(Perk))
                {
                    continue;
                }
                if (Perk == EPerk::Lifeline && State.Lives > 2)
                {
                    continue;
                }
                Pool.push_back(Perk);
            }

            for (int32 Slot = 0; Slot < kDraftChoices; ++Slot)
            {
                if (Pool.empty())
                {
                    State.Draft[Slot] = EPerk::Lifeline;
                    continue;
                }
                const int32 Pick = Rng.Below(int32(Pool.size()));
                State.Draft[Slot] = Pool[Pick];
                Pool[Pick] = Pool.back();
                Pool.pop_back();
            }

            State.DraftCursor = 1;
            State.Phase = EPhase::Draft;
            State.PhaseTimer = 0.0f;
            PlaySound(Registry, ESound::PerkOffer);
        }

        void PickDraft(ECS::FRegistry& Registry)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();
            const EPerk Perk = State.Draft[Math::Clamp(State.DraftCursor, 0, kDraftChoices - 1)];
            State.Perks.Add(Perk);

            if (Perk == EPerk::Lifeline)
            {
                State.Lives = Math::Min(State.Lives + 1, 9);
            }

            PlaySound(Registry, ESound::PerkPick);
            StartLevel(Registry, State.Level + 1);
        }

        void FlowSystem(ECS::FRegistry& Registry, float Delta)
        {
            FGameState& State = Registry.GetSingleton<FGameState>();

            State.Elapsed += Delta;
            State.PhaseTimer += Delta;
            State.FlashPulse = Math::Max(0.0f, State.FlashPulse - Delta * 2.6f);
            State.FireGlow = Math::Max(0.0f, State.FireGlow - Delta * 1.6f);
            State.ComboTimer = Math::Max(0.0f, State.ComboTimer - Delta);
            State.BossIntro = Math::Max(0.0f, State.BossIntro - Delta);
            if (State.ComboTimer <= 0.0f)
            {
                State.Combo = 0;
            }

            State.SlowTimer = Math::Max(0.0f, State.SlowTimer - Delta);
            State.TimeScale = State.SlowTimer > 0.0f ? 0.45f : 1.0f;
            State.WallTimer    = Math::Max(0.0f, State.WallTimer - Delta);
            State.FreezeTimer  = Math::Max(0.0f, State.FreezeTimer - Delta);
            State.JackpotTimer = Math::Max(0.0f, State.JackpotTimer - Delta);
            State.BlindTimer   = Math::Max(0.0f, State.BlindTimer - Delta);
            State.VaultGlow   += ((State.IsVault() ? 1.0f : 0.0f) - State.VaultGlow) * Math::Min(1.0f, Delta * 5.0f);

            if (State.FeverTimer > 0.0f)
            {
                State.FeverTimer -= Delta;
                State.FeverMeter = Math::Max(0.0f, State.FeverTimer / State.Perks.FeverDuration());
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

            State.bBossAlive = !Registry.View<FBoss>().IsEmpty();

            if (State.Phase == EPhase::Playing)
            {
                State.LevelTime += Delta;
                const float ModeRate = State.Mode == EGameMode::Endless ? 1.6f : (State.Mode == EGameMode::BossRush ? 0.8f : 1.0f);
                if (State.FreezeTimer <= 0.0f)
                {
                    State.FormationDrop += (0.9f + float(State.Level) * 0.55f) * ModeRate * Delta;
                }
                State.FormationDrift = State.Level % 3 == 0 ? Math::Sin(State.Elapsed * 0.35f) * 80.0f : 0.0f;
            }

            const float Lowest = LowestBrickEdge(Registry);
            State.BreachWarning = Math::Clamp((Lowest - (kBreachY - 170.0f)) / 170.0f, 0.0f, 1.0f);

            if (Lowest >= kBreachY)
            {
                State.FormationDrop = Math::Max(0.0f, State.FormationDrop - 260.0f);
                State.Combo = 0;
                AddTrauma(Registry, 0.8f, 0.9f);
                AddHitStop(Registry, 0.065f);
                SpawnShockwave(Registry, { kFieldWidth * 0.5f, kBreachY }, { 1.4f, 0.2f, 0.25f, 1.0f }, 1200.0f, 0.7f, 1.0f);

                if (State.Perks.Has(EPerk::Bulwark) && !State.bBulwarkSpent)
                {
                    State.bBulwarkSpent = true;
                    SpawnLabel(Registry, { kFieldWidth * 0.5f, kBreachY - 60.0f }, "BULWARK", { 0.4f, 1.3f, 1.7f, 1.0f }, 7.0f, 1.3f);
                    PlaySound(Registry, ESound::ShieldSave);
                    return;
                }

                State.Lives -= 1;
                State.LevelLivesLost += 1;
                PlaySound(Registry, ESound::Breach);

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
                GradeLevel(Registry);
                AddTrauma(Registry, 0.5f, 0.6f);
                AddHitStop(Registry, 0.075f);
                PlaySound(Registry, ESound::LevelClear);
                return;
            }

            if (Registry.View<FBall>().IsEmpty())
            {
                ECS::FEntity Shielded = ECS::NullEntity;
                for (auto [Entity, Paddle] : Registry.View<FPaddle>().Each())
                {
                    if (Paddle.ShieldCharge >= 1.0f)
                    {
                        Shielded = Entity;
                    }
                }

                if (!Shielded.IsNull())
                {
                    FPaddle& Paddle = Registry.Get<FPaddle>(Shielded);
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
                State.LevelLivesLost += 1;
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

            if (State.Phase == EPhase::LevelClear && State.PhaseTimer > 3.0f)
            {
                OfferDraft(Registry);
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

        void MenuInputSystem(ECS::FRegistry& Registry)
        {
            FFrameInput& Input = Registry.GetSingleton<FFrameInput>();
            FGameState& State = Registry.GetSingleton<FGameState>();

            if (State.Phase == EPhase::Title)
            {
                if (Input.Nav != 0)
                {
                    const int32 Count = int32(EGameMode::Count);
                    State.MenuCursor = EGameMode((int32(State.MenuCursor) + Input.Nav + Count) % Count);
                    PlaySound(Registry, ESound::UiMove);
                }
                if (Input.bConfirm)
                {
                    PlaySound(Registry, ESound::UiConfirm);
                    StartRun(Registry, State.MenuCursor);
                }
            }
            else if (State.Phase == EPhase::GameOver)
            {
                if (Input.bConfirm && State.PhaseTimer > 0.8f)
                {
                    PlaySound(Registry, ESound::UiConfirm);
                    State.Phase = EPhase::Title;
                    State.PhaseTimer = 0.0f;
                }
            }
            else if (State.Phase == EPhase::Draft)
            {
                if (Input.Nav != 0)
                {
                    State.DraftCursor = (State.DraftCursor + Input.Nav + kDraftChoices) % kDraftChoices;
                    PlaySound(Registry, ESound::UiMove);
                }
                if (Input.bMouseMoved && Input.bUsingMouse)
                {
                    State.DraftCursor = Math::Clamp(int32(Input.PaddleTarget / (kFieldWidth / float(kDraftChoices))), 0, kDraftChoices - 1);
                }
                if (Input.Hotkey >= 0 && Input.Hotkey < kDraftChoices)
                {
                    State.DraftCursor = Input.Hotkey;
                    PickDraft(Registry);
                }
                else if (Input.bConfirm && State.PhaseTimer > 0.35f)
                {
                    PickDraft(Registry);
                }
            }
            else if (Input.bConfirm)
            {
                LaunchHeldBalls(Registry);
            }

            Input.bConfirm = false;
            Input.Nav = 0;
            Input.Hotkey = -1;
            Input.bMouseMoved = false;
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

        EnsurePaddles(Registry, 1);
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
            Input.bSmash = false;
            Input.Nav = 0;
            Input.Hotkey = -1;
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
            Input.bSmash = false;
        }
    }

    void FGame::StepFixed(float Delta)
    {
        FGameState& State = Registry.GetSingleton<FGameState>();

        MenuInputSystem(Registry);
        SmashInputSystem(Registry);

        PaddleSystem(Registry, Delta);
        HeldBallSystem(Registry);

        if (State.Phase == EPhase::Playing || State.Phase == EPhase::Serve)
        {
            BrickBehaviorSystem(Registry, Delta);
            LaserSystem(Registry, Delta);
            BossSystem(Registry, Delta);
            DroneSystem(Registry, Delta);
            BoltSystem(Registry, Delta);
            BallSystem(Registry, Delta);
            ResolveExplosions(Registry);
            PowerUpSystem(Registry, Delta);
            VaultSystem(Registry, Delta);
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

    const char* FGame::GetLevelName() const
    {
        return LevelName(Registry.GetSingleton<FGameState>());
    }

    void FGame::OnKey(Lumina::EKey Key, bool bPressed)
    {
        FFrameInput& Input = Registry.GetSingleton<FFrameInput>();
        const FGameState& State = Registry.GetSingleton<FGameState>();

        const bool bArrowLeft  = Key == EKey::Left;
        const bool bArrowRight = Key == EKey::Right;
        const bool bKeyLeft    = Key == EKey::A;
        const bool bKeyRight   = Key == EKey::D;

        if (bArrowLeft || bArrowRight || bKeyLeft || bKeyRight)
        {
            const float Sign = bArrowLeft || bKeyLeft ? -1.0f : 1.0f;
            const int32 Player = State.IsCoop() && (bArrowLeft || bArrowRight) ? 1 : 0;

            if (bPressed)
            {
                Input.bUsingMouse = Player == 0 ? false : Input.bUsingMouse;
                Input.KeyAxis[Player] = Sign;
                Input.Nav = Sign < 0.0f ? -1 : 1;
            }
            else if (Input.KeyAxis[Player] == Sign)
            {
                Input.KeyAxis[Player] = 0.0f;
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
        case EKey::Up:
        case EKey::W:
            Input.bConfirm = true;
            Input.bLaunch = true;
            Input.bSmash = true;
            break;
        case EKey::D1:
            Input.Hotkey = 0;
            break;
        case EKey::D2:
            Input.Hotkey = 1;
            break;
        case EKey::D3:
            Input.Hotkey = 2;
            break;
        case EKey::P:
            bPaused = !bPaused;
            Registry.GetSingleton<FSoundQueue>().Pending.push_back(FSoundRequest{ ESound::UiMove });
            break;
        case EKey::M:
            bMuted = !bMuted;
            break;
        case EKey::Escape:
            if (State.Phase == EPhase::Title)
            {
                bQuitRequested = true;
                break;
            }
            bQuitRequested = bPaused;
            bPaused = true;
            break;
        case EKey::R:
            StartRun(Registry, State.Mode);
            break;
        case EKey::T:
            Registry.GetSingleton<FGameState>().Phase = EPhase::Title;
            Registry.GetSingleton<FGameState>().PhaseTimer = 0.0f;
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
        Input.bMouseMoved = true;
        Input.KeyAxis[0] = 0.0f;
    }
}
