#include "Game.h"

#include "Containers/Vector.h"
#include "Platform/Time/PlatformTime.h"

namespace Umbral
{
    namespace
    {
        constexpr float kSpawnRing   = 1500.0f;
        constexpr int32 kMaxUpgrades = 8;

        const char* const kWeaponNames[int32(EWeapon::Count)] =
        {
            "UMBRAL BLADES",
            "SOULBOLT",
            "NOVA",
            "PYRE",
            "HUNGERING MAW",
            "CHAIN",
            "GLOOM",
        };

        const char* const kWeaponTaglines[int32(EWeapon::Count)] =
        {
            "EDGES THAT CIRCLE YOU",
            "SHARDS THAT SEEK",
            "A BURST FROM WITHIN",
            "FLAME THAT LINGERS",
            "A HOLE THAT DRINKS",
            "LIGHT THAT LEAPS",
            "A TIDE THAT DRAGS",
        };

        float VectorLength(const FVector2& Value)
        {
            return Math::Sqrt(Value.x * Value.x + Value.y * Value.y);
        }

        FVector2 Normalized(const FVector2& Value)
        {
            const float Length = VectorLength(Value);
            return Length > 0.0001f ? FVector2{ Value.x / Length, Value.y / Length } : FVector2{ 1.0f, 0.0f };
        }

        void PlaySound(ECS::FRegistry& Registry, ESound Sound, float Pitch = 1.0f, float Volume = 1.0f, float Pan = 0.0f)
        {
            FSoundQueue& Queue = Registry.GetSingleton<FSoundQueue>();
            if (Queue.Pending.size() < 64)
            {
                Queue.Pending.push_back(FSoundRequest{ Sound, Pitch, Volume, Pan });
            }
        }

        float PanFor(const FVector2& World, const FVector2& Camera)
        {
            return Math::Clamp((World.x - Camera.x) / (kViewWidth * 0.5f), -1.0f, 1.0f) * 0.6f;
        }

        void AddTrauma(ECS::FRegistry& Registry, float Amount)
        {
            FRunState& Run = Registry.GetSingleton<FRunState>();
            Run.ShakeTrauma = Math::Min(Run.ShakeTrauma + Amount, 1.0f);
        }

        int32 CountParticles(ECS::FRegistry& Registry)
        {
            return int32(Registry.View<FParticle>().NumCandidates());
        }

        void SpawnParticle(ECS::FRegistry& Registry, const FVector2& Position, const FVector2& Velocity,
                           const FVector4& Start, const FVector4& End, float Size, float Life, float Drag)
        {
            if (CountParticles(Registry) >= kMaxParticles)
            {
                return;
            }

            const ECS::FEntity Entity = Registry.Create();

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = Position;
            Body.HalfSize = { Size, Size };
            Body.Rotation = 0.0f;

            FVisual& Visual = Registry.Emplace<FVisual>(Entity);
            Visual.Color  = Start;
            Visual.Accent = Start;
            Visual.Kind   = EQuadKind::Spark;
            Visual.Glow   = 1.0f;
            Visual.Lit    = 0.0f;

            FParticle& Particle = Registry.Emplace<FParticle>(Entity);
            Particle.Velocity  = Velocity;
            Particle.EndColor  = End;
            Particle.MaxLife   = Life;
            Particle.Life      = Life;
            Particle.Drag      = Drag;
            Particle.StartSize = Size;
        }

        void SpawnBurst(ECS::FRegistry& Registry, const FVector2& Position, const FVector4& Color, int32 Count,
                        float SpeedMin, float SpeedMax)
        {
            FRandom& Rng = Registry.GetSingleton<FRandom>();
            const int32 Budget = Math::Min(Count, kMaxParticles - CountParticles(Registry));

            for (int32 i = 0; i < Budget; ++i)
            {
                const float Angle = Rng.Range(0.0f, 6.2831853f);
                const float Speed = Rng.Range(SpeedMin, SpeedMax);

                const FVector4 Start { Color.x * 2.6f, Color.y * 2.6f, Color.z * 2.6f, 1.0f };
                const FVector4 End   { Color.x * 0.2f, Color.y * 0.1f, Color.z * 0.3f, 0.0f };

                SpawnParticle(Registry, Position, { Math::Cos(Angle) * Speed, Math::Sin(Angle) * Speed },
                    Start, End, Rng.Range(2.5f, 6.0f), Rng.Range(0.25f, 0.7f), Rng.Range(2.0f, 4.0f));
            }
        }

        void SpawnMote(ECS::FRegistry& Registry, const FVector2& Position, float Value)
        {
            const ECS::FEntity Entity = Registry.Create();

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = Position;
            Body.HalfSize = { 5.5f, 5.5f };
            Body.Rotation = 0.0f;

            FVisual& Visual = Registry.Emplace<FVisual>(Entity);
            Visual.Color  = { 0.16f, 0.50f, 0.74f, 1.0f };
            Visual.Accent = Visual.Color;
            Visual.Kind   = EQuadKind::Mote;
            Visual.Glow   = 0.50f;

            FSoulMote& Mote = Registry.Emplace<FSoulMote>(Entity);
            Mote.Value = Value;
        }

        void SpawnDamageNumber(ECS::FRegistry& Registry, const FVector2& Position, int32 Value, const FVector4& Color)
        {
            if (Registry.View<FDamageNumber>().NumCandidates() > 40)
            {
                return;
            }

            const ECS::FEntity Entity = Registry.Create();
            FDamageNumber& Number = Registry.Emplace<FDamageNumber>(Entity);
            Number.Position = Position;
            Number.Value = Value;
            Number.Color = Color;
        }

        void SpawnBolt(ECS::FRegistry& Registry, const FVector2& Position, const FVector2& Velocity,
                       float Damage, int32 Pierce)
        {
            const ECS::FEntity Entity = Registry.Create();

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = Position;
            Body.HalfSize = { 26.0f, 11.0f };
            Body.Rotation = Math::Atan2(Velocity.y, Velocity.x);

            FVisual& Visual = Registry.Emplace<FVisual>(Entity);
            Visual.Color  = { 0.90f, 1.70f, 2.60f, 1.0f };
            Visual.Accent = { 0.20f, 0.60f, 1.40f, 1.0f };
            Visual.Kind   = EQuadKind::Bolt;
            Visual.Glow   = 1.2f;

            FLight& Light = Registry.Emplace<FLight>(Entity);
            Light.Color  = { 0.30f, 0.75f, 1.30f, 1.0f };
            Light.Radius = 260.0f;
            Light.Energy = 0.9f;

            FProjectile& Projectile = Registry.Emplace<FProjectile>(Entity);
            Projectile.Velocity = Velocity;
            Projectile.Damage   = Damage;
            Projectile.Pierce   = Pierce;
            Projectile.Homing   = 5.0f;
            Projectile.Life     = 2.4f;
        }

        void SpawnNova(ECS::FRegistry& Registry, const FVector2& Position, float Radius, float Damage)
        {
            const ECS::FEntity Entity = Registry.Create();

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = Position;
            Body.HalfSize = { 1.0f, 1.0f };
            Body.Rotation = 0.0f;

            FVisual& Visual = Registry.Emplace<FVisual>(Entity);
            Visual.Color  = { 2.20f, 1.10f, 2.80f, 1.0f };
            Visual.Accent = Visual.Color;
            Visual.Kind   = EQuadKind::Ring;
            Visual.Glow   = 1.4f;

            FLight& Light = Registry.Emplace<FLight>(Entity);
            Light.Color  = { 0.85f, 0.45f, 1.20f, 1.0f };
            Light.Radius = Radius;
            Light.Energy = 1.6f;

            FNova& Nova = Registry.Emplace<FNova>(Entity);
            Nova.MaxRadius = Radius;
            Nova.Damage    = Damage;
        }

        void SpawnMaw(ECS::FRegistry& Registry, const FVector2& Position, float Radius, float Pull, float Damage)
        {
            const ECS::FEntity Entity = Registry.Create();

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = Position;
            Body.HalfSize = { Radius * 0.35f, Radius * 0.35f };
            Body.Rotation = 0.0f;

            FVisual& Visual = Registry.Emplace<FVisual>(Entity);
            Visual.Color  = { 1.10f, 0.35f, 1.80f, 1.0f };
            Visual.Accent = { 0.20f, 0.02f, 0.40f, 1.0f };
            Visual.Kind   = EQuadKind::Ring;
            Visual.Glow   = 1.3f;

            FLight& Light = Registry.Emplace<FLight>(Entity);
            Light.Color  = { 0.60f, 0.20f, 1.10f, 1.0f };
            Light.Radius = Radius * 1.4f;
            Light.Energy = 1.2f;

            FMaw& Maw = Registry.Emplace<FMaw>(Entity);
            Maw.Radius = Radius;
            Maw.Pull   = Pull;
            Maw.Damage = Damage;
        }

        void SpawnArc(ECS::FRegistry& Registry, const FVector2& From, const FVector2& To)
        {
            const ECS::FEntity Entity = Registry.Create();

            FArc& Arc = Registry.Emplace<FArc>(Entity);
            Arc.From = From;
            Arc.To = To;

            FLight& Light = Registry.Emplace<FLight>(Entity);
            Light.Color  = { 1.00f, 0.95f, 0.45f, 1.0f };
            Light.Radius = 240.0f;
            Light.Energy = 1.4f;

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = { (From.x + To.x) * 0.5f, (From.y + To.y) * 0.5f };
            Body.HalfSize = { 4.0f, 4.0f };
            Body.Rotation = 0.0f;
        }

        void SpawnPyre(ECS::FRegistry& Registry, const FVector2& Position, float Radius, float Damage)
        {
            const ECS::FEntity Entity = Registry.Create();

            FBody& Body = Registry.Emplace<FBody>(Entity);
            Body.Position = Position;
            Body.HalfSize = { Radius, Radius };
            Body.Rotation = 0.0f;

            FVisual& Visual = Registry.Emplace<FVisual>(Entity);
            Visual.Color  = { 1.90f, 0.70f, 0.16f, 1.0f };
            Visual.Accent = { 0.90f, 0.20f, 0.04f, 1.0f };
            Visual.Kind   = EQuadKind::Glow;
            Visual.Glow   = 1.2f;

            FLight& Light = Registry.Emplace<FLight>(Entity);
            Light.Color  = { 1.20f, 0.52f, 0.14f, 1.0f };
            Light.Radius = Radius * 2.4f;
            Light.Energy = 1.1f;

            FPyre& Pyre = Registry.Emplace<FPyre>(Entity);
            Pyre.Radius = Radius;
            Pyre.Damage = Damage;
        }

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

        void ClearWorld(ECS::FRegistry& Registry)
        {
            DestroyAllWith<FProjectile>(Registry);
            DestroyAllWith<FNova>(Registry);
            DestroyAllWith<FPyre>(Registry);
            DestroyAllWith<FMaw>(Registry);
            DestroyAllWith<FArc>(Registry);
            DestroyAllWith<FSoulMote>(Registry);
            DestroyAllWith<FParticle>(Registry);
            DestroyAllWith<FDamageNumber>(Registry);
        }
    }


    void FGame::Initialize()
    {
        Registry.EmplaceSingleton<FRunState>();
        Registry.EmplaceSingleton<FPlayerState>();
        Registry.EmplaceSingleton<FWeaponState>();
        Registry.EmplaceSingleton<FFrameInput>();
        Registry.EmplaceSingleton<FSoundQueue>();
        Registry.EmplaceSingleton<FRandom>();

        Registry.ReserveComponents<FBody>(kMaxParticles + 512);
        Registry.ReserveComponents<FVisual>(kMaxParticles + 512);
        Registry.ReserveComponents<FParticle>(kMaxParticles);

        Swarm.Initialize();

        Registry.GetSingleton<FWeaponState>().Level[int32(EWeapon::Blades)] = 1;
    }

    void FGame::Advance(float DeltaSeconds)
    {
        FFrameInput& Input = Registry.GetSingleton<FFrameInput>();
        FRunState& Run = Registry.GetSingleton<FRunState>();

        if (bPaused)
        {
            Input.bConfirm = false;
            Input.bClick = false;
            Input.Choice = -1;
            return;
        }

        const float Clamped = Math::Min(DeltaSeconds, 0.05f);

        float Scale = 1.0f;
        if (Run.HitStop > 0.0f)
        {
            Run.HitStop = Math::Max(0.0f, Run.HitStop - Clamped);
            Scale = 0.15f;
        }

        Accumulator += Clamped * Scale;

        int32 Steps = 0;
        while (Accumulator >= kFixedStep && Steps < kMaxCatchUpSteps)
        {
            Accumulator -= kFixedStep;
            StepFixed(kFixedStep);
            ++Steps;
        }

        if (Accumulator >= kFixedStep)
        {
            Accumulator = 0.0f;
        }

        Stats.SimSteps = Steps;
        StepVisual(Clamped * Scale);

        // A frame shorter than the fixed step runs no simulation, so a press has to survive to the next one.
        if (Steps > 0)
        {
            Input.bConfirm = false;
            Input.bClick = false;
            Input.Choice = -1;
        }
    }

    void FGame::StepFixed(float Delta)
    {
        FRunState& Run = Registry.GetSingleton<FRunState>();
        FPlayerState& Player = Registry.GetSingleton<FPlayerState>();
        FWeaponState& Weapons = Registry.GetSingleton<FWeaponState>();
        FFrameInput& Input = Registry.GetSingleton<FFrameInput>();
        FRandom& Rng = Registry.GetSingleton<FRandom>();

        if (Run.Phase == EPhase::Title || Run.Phase == EPhase::Dead)
        {
            if (Input.bConfirm)
            {
                Swarm.Reset();
                ClearWorld(Registry);

                Run.Phase = EPhase::Playing;
                Run.Elapsed = 0.0f;
                Run.Level = 1;
                Run.Souls = 0.0f;
                Run.SoulsNeeded = 12.0f;
                Run.Kills = 0;
                Run.Danger = 0.0f;

                Player = FPlayerState{};
                Weapons = FWeaponState{};
                Weapons.Level[int32(EWeapon::Blades)] = 1;

                PlaySound(Registry, ESound::UiConfirm);
            }
            Run.Elapsed += Delta;
            return;
        }

        if (Run.Phase == EPhase::LevelUp)
        {
            Run.PhaseTimer += Delta;

            const int32 Previous = Run.Hovered;
            Run.Hovered = -1;
            for (int32 Slot = 0; Slot < Run.ChoiceCount; ++Slot)
            {
                const FCardRect Rect = LevelUpCardRect(Slot, Input.ViewSize);
                if (Math::Abs(Input.MouseView.x - Rect.Center.x) < Rect.Half.x &&
                    Math::Abs(Input.MouseView.y - Rect.Center.y) < Rect.Half.y)
                {
                    Run.Hovered = Slot;
                    break;
                }
            }

            if (Run.Hovered >= 0 && Run.Hovered != Previous)
            {
                PlaySound(Registry, ESound::UiMove, 1.0f, 0.5f);
            }

            if (Input.bClick && Run.Hovered >= 0)
            {
                Input.Choice = Run.Hovered;
            }

            if (Input.Choice >= 0 && Input.Choice < Run.ChoiceCount)
            {
                const int32 Pick = Run.Choices[Input.Choice];
                Weapons.Level[Pick] = Math::Min(Weapons.Level[Pick] + 1, kMaxUpgrades);
                Run.Phase = EPhase::Playing;
                Run.BannerTimer = 1.6f;
                Run.Hovered = -1;
                PlaySound(Registry, ESound::UpgradePick);
            }
            return;
        }

        Run.BannerTimer = Math::Max(0.0f, Run.BannerTimer - Delta);

        Run.Elapsed += Delta;

        //~ Player

        const FVector2 Wish
        {
            (Input.bRight ? 1.0f : 0.0f) - (Input.bLeft ? 1.0f : 0.0f),
            (Input.bDown ? 1.0f : 0.0f) - (Input.bUp ? 1.0f : 0.0f),
        };

        if (Math::Abs(Wish.x) > 0.0f || Math::Abs(Wish.y) > 0.0f)
        {
            const FVector2 Direction = Normalized(Wish);
            Player.Position += Direction * kPlayerSpeed * Delta;
            Player.Facing = Direction;
        }

        Player.Position.x = Math::Clamp(Player.Position.x, 120.0f, kWorldSize - 120.0f);
        Player.Position.y = Math::Clamp(Player.Position.y, 120.0f, kWorldSize - 120.0f);
        Player.BladePhase += Delta * 2.6f;
        Player.HurtFlash = Math::Max(0.0f, Player.HurtFlash - Delta * 2.5f);
        Player.Invuln = Math::Max(0.0f, Player.Invuln - Delta);

        //~ Horde spawning ramps with survival time

        const float Minutes = Run.Elapsed / 60.0f;
        const int32 Target = Math::Min(int32(600.0f + Math::Pow(Minutes, 2.2f) * 45000.0f), kMaxAgents);
        const int32 Deficit = Target - Swarm.Num();

        if (Deficit > 0)
        {
            const int32 Batch = Math::Min(Deficit, 7000);
            for (int32 i = 0; i < Batch; ++i)
            {
                const float Angle = Rng.Range(0.0f, 6.2831853f);
                const float Distance = Rng.Range(kSpawnRing, kSpawnRing + 900.0f);
                const FVector2 Spot
                {
                    Math::Clamp(Player.Position.x + Math::Cos(Angle) * Distance, 20.0f, kWorldSize - 20.0f),
                    Math::Clamp(Player.Position.y + Math::Sin(Angle) * Distance, 20.0f, kWorldSize - 20.0f),
                };

                const float Roll = Rng.Unit();
                EAgentKind Kind = EAgentKind::Wisp;
                if (Minutes > 0.5f && Roll < 0.30f) { Kind = EAgentKind::Crawler; }
                if (Minutes > 1.5f && Roll < 0.12f) { Kind = EAgentKind::Shade; }
                if (Minutes > 2.5f && Roll < 0.05f) { Kind = EAgentKind::Brute; }

                Swarm.Spawn(Kind, Spot, Rng);
            }
        }

        //~ Swarm

        const uint64 SwarmStart = PlatformTime::Cycles();

        Swarm.BuildGrid();
        Swarm.Advance(Delta, Player.Position);

        Volumes.clear();
        Deaths.clear();
        DeathColors.clear();

        //~ Weapons

        const int32 BladeLevel = Weapons.Level[int32(EWeapon::Blades)];
        if (BladeLevel > 0)
        {
            const int32 Blades = 2 + BladeLevel;
            const float Orbit = 140.0f + float(BladeLevel) * 12.0f;
            const float Damage = (10.0f + float(BladeLevel) * 5.0f) * Delta * 8.0f;

            for (int32 Index = 0; Index < Blades; ++Index)
            {
                const float Angle = Player.BladePhase + 6.2831853f * float(Index) / float(Blades);
                const FVector2 Spot
                {
                    Player.Position.x + Math::Cos(Angle) * Orbit,
                    Player.Position.y + Math::Sin(Angle) * Orbit,
                };
                Volumes.push_back(FDamageVolume{ Spot, 46.0f, Damage, 260.0f });
            }
        }

        const int32 BoltLevel = Weapons.Level[int32(EWeapon::Soulbolt)];
        if (BoltLevel > 0)
        {
            Weapons.Cooldown[int32(EWeapon::Soulbolt)] -= Delta;
            if (Weapons.Cooldown[int32(EWeapon::Soulbolt)] <= 0.0f)
            {
                Weapons.Cooldown[int32(EWeapon::Soulbolt)] = Math::Max(0.14f, 0.62f - float(BoltLevel) * 0.05f);

                const int32 Shots = 1 + BoltLevel / 3;
                for (int32 Shot = 0; Shot < Shots; ++Shot)
                {
                    FVector2 Aim = Player.Facing;
                    FVector2 Nearest;
                    if (Swarm.FindNearest(Player.Position, 900.0f, Nearest))
                    {
                        Aim = Normalized(Nearest - Player.Position);
                    }

                    const float Spread = (float(Shot) - float(Shots - 1) * 0.5f) * 0.22f;
                    const float Cos = Math::Cos(Spread);
                    const float Sin = Math::Sin(Spread);
                    const FVector2 Direction { Aim.x * Cos - Aim.y * Sin, Aim.x * Sin + Aim.y * Cos };

                    SpawnBolt(Registry, Player.Position, Direction * 900.0f,
                        22.0f + float(BoltLevel) * 9.0f, 2 + BoltLevel / 2);
                }
                PlaySound(Registry, ESound::BoltFire, Rng.Range(0.94f, 1.08f), 0.7f);
            }
        }

        const int32 NovaLevel = Weapons.Level[int32(EWeapon::Nova)];
        if (NovaLevel > 0)
        {
            Weapons.Cooldown[int32(EWeapon::Nova)] -= Delta;
            if (Weapons.Cooldown[int32(EWeapon::Nova)] <= 0.0f)
            {
                Weapons.Cooldown[int32(EWeapon::Nova)] = Math::Max(1.4f, 4.2f - float(NovaLevel) * 0.32f);
                SpawnNova(Registry, Player.Position, 420.0f + float(NovaLevel) * 60.0f,
                    40.0f + float(NovaLevel) * 16.0f);
                AddTrauma(Registry, 0.25f);
                PlaySound(Registry, ESound::NovaCast);
            }
        }

        const int32 PyreLevel = Weapons.Level[int32(EWeapon::Pyre)];
        if (PyreLevel > 0)
        {
            Weapons.Cooldown[int32(EWeapon::Pyre)] -= Delta;
            if (Weapons.Cooldown[int32(EWeapon::Pyre)] <= 0.0f)
            {
                Weapons.Cooldown[int32(EWeapon::Pyre)] = Math::Max(0.7f, 2.4f - float(PyreLevel) * 0.18f);

                const float Angle = Rng.Range(0.0f, 6.2831853f);
                const float Distance = Rng.Range(60.0f, 240.0f);
                SpawnPyre(Registry, { Player.Position.x + Math::Cos(Angle) * Distance,
                                      Player.Position.y + Math::Sin(Angle) * Distance },
                    110.0f + float(PyreLevel) * 14.0f, 30.0f + float(PyreLevel) * 12.0f);
                PlaySound(Registry, ESound::PyreLight, Rng.Range(0.9f, 1.1f), 0.6f);
            }
        }

        const int32 MawLevel = Weapons.Level[int32(EWeapon::Maw)];
        if (MawLevel > 0)
        {
            Weapons.Cooldown[int32(EWeapon::Maw)] -= Delta;
            if (Weapons.Cooldown[int32(EWeapon::Maw)] <= 0.0f)
            {
                Weapons.Cooldown[int32(EWeapon::Maw)] = Math::Max(2.4f, 7.0f - float(MawLevel) * 0.55f);

                const float Angle = Rng.Range(0.0f, 6.2831853f);
                const float Distance = Rng.Range(200.0f, 460.0f);
                SpawnMaw(Registry, { Player.Position.x + Math::Cos(Angle) * Distance,
                                     Player.Position.y + Math::Sin(Angle) * Distance },
                    360.0f + float(MawLevel) * 42.0f, 700.0f + float(MawLevel) * 160.0f,
                    45.0f + float(MawLevel) * 18.0f);
                AddTrauma(Registry, 0.20f);
                PlaySound(Registry, ESound::NovaCast, 0.72f, 0.9f);
            }
        }

        const int32 ChainLevel = Weapons.Level[int32(EWeapon::Chain)];
        if (ChainLevel > 0)
        {
            Weapons.Cooldown[int32(EWeapon::Chain)] -= Delta;
            if (Weapons.Cooldown[int32(EWeapon::Chain)] <= 0.0f)
            {
                Weapons.Cooldown[int32(EWeapon::Chain)] = Math::Max(0.30f, 1.30f - float(ChainLevel) * 0.11f);

                const int32 Leaps = 3 + ChainLevel;
                const float Reach = 320.0f + float(ChainLevel) * 34.0f;
                const float Damage = 30.0f + float(ChainLevel) * 14.0f;

                FVector2 From = Player.Position;
                for (int32 Leap = 0; Leap < Leaps; ++Leap)
                {
                    FVector2 Hit;
                    if (!Swarm.FindNearest(From, Reach, Hit))
                    {
                        break;
                    }

                    Volumes.push_back(FDamageVolume{ Hit, 62.0f, Damage, 120.0f, 0.0f, 0.0f });
                    SpawnArc(Registry, From, Hit);
                    From = Hit;
                }
                PlaySound(Registry, ESound::BoltHit, Rng.Range(1.15f, 1.35f), 0.6f);
            }
        }

        const int32 GloomLevel = Weapons.Level[int32(EWeapon::Gloom)];
        if (GloomLevel > 0)
        {
            const float Radius = 240.0f + float(GloomLevel) * 26.0f;
            Volumes.push_back(FDamageVolume{ Player.Position, Radius,
                (6.0f + float(GloomLevel) * 4.0f) * Delta, 0.0f, 0.0f,
                Math::Min(0.06f + float(GloomLevel) * 0.012f, 0.16f) });
        }

        //~ Projectiles

        {
            TVector<ECS::FEntity> Doomed;

            for (auto [Entity, Body, Projectile] : Registry.View<FBody, FProjectile>().Each())
            {
                Projectile.Life -= Delta;
                if (Projectile.Life <= 0.0f || Projectile.Pierce <= 0)
                {
                    Doomed.push_back(Entity);
                    continue;
                }

                if (Projectile.Homing > 0.0f)
                {
                    FVector2 Nearest;
                    if (Swarm.FindNearest(Body.Position, 520.0f, Nearest))
                    {
                        const FVector2 Desired = Normalized(Nearest - Body.Position) * VectorLength(Projectile.Velocity);
                        Projectile.Velocity += (Desired - Projectile.Velocity) * Math::Min(1.0f, Projectile.Homing * Delta);
                    }
                }

                Body.Position += Projectile.Velocity * Delta;
                Body.Rotation = Math::Atan2(Projectile.Velocity.y, Projectile.Velocity.x);

                Volumes.push_back(FDamageVolume{ Body.Position, Projectile.Radius, Projectile.Damage, 340.0f });
                Projectile.Pierce -= Swarm.CountNear(Body.Position, Projectile.Radius) > 0 ? 1 : 0;
            }

            for (const ECS::FEntity Entity : Doomed)
            {
                Registry.Destroy(Entity);
            }
        }

        //~ Novas and pyres

        {
            TVector<ECS::FEntity> Doomed;

            for (auto [Entity, Body, Nova, Light] : Registry.View<FBody, FNova, FLight>().Each())
            {
                Nova.Age += Delta;
                if (Nova.Age >= Nova.Duration)
                {
                    Doomed.push_back(Entity);
                    continue;
                }

                const float Alpha = Nova.Age / Nova.Duration;
                const float Radius = Nova.MaxRadius * (1.0f - (1.0f - Alpha) * (1.0f - Alpha));
                Body.HalfSize = { Radius, Radius };
                Light.Radius = Radius * 1.3f;
                Light.Energy = 1.6f * (1.0f - Alpha);

                Volumes.push_back(FDamageVolume{ Body.Position, Radius, Nova.Damage * Delta * 6.0f, 700.0f });
            }

            for (auto [Entity, Body, Pyre, Light] : Registry.View<FBody, FPyre, FLight>().Each())
            {
                Pyre.Age += Delta;
                if (Pyre.Age >= Pyre.Duration)
                {
                    Doomed.push_back(Entity);
                    continue;
                }

                const float Flicker = 0.85f + 0.15f * Math::Sin(Run.Elapsed * 14.0f + Pyre.Radius);
                Light.Energy = 1.1f * Flicker * (1.0f - Pyre.Age / Pyre.Duration);
                Volumes.push_back(FDamageVolume{ Body.Position, Pyre.Radius, Pyre.Damage * Delta, 0.0f });
            }

            for (const ECS::FEntity Entity : Doomed)
            {
                Registry.Destroy(Entity);
            }
        }

        {
            TVector<ECS::FEntity> Doomed;

            for (auto [Entity, Body, Maw, Light] : Registry.View<FBody, FMaw, FLight>().Each())
            {
                Maw.Age += Delta;
                Maw.Spin += Delta * 5.0f;

                if (Maw.Age >= Maw.Duration)
                {
                    Doomed.push_back(Entity);
                    SpawnBurst(Registry, Body.Position, { 0.70f, 0.24f, 1.10f, 1.0f }, 40, 200.0f, 900.0f);
                    AddTrauma(Registry, 0.28f);
                    continue;
                }

                const float Alpha = Maw.Age / Maw.Duration;
                const float Swell = Math::Sin(Alpha * 3.14159f);
                Body.HalfSize = { Maw.Radius * (0.35f + Swell * 0.65f), Maw.Radius * (0.35f + Swell * 0.65f) };
                Body.Rotation = Maw.Spin;
                Light.Energy = 1.4f * Swell;
                Light.Radius = Maw.Radius * 1.4f;

                Volumes.push_back(FDamageVolume{ Body.Position, Maw.Radius, Maw.Damage * Delta, 0.0f,
                    Maw.Pull * Delta * Swell, 0.0f });

                if (CountParticles(Registry) < kMaxParticles - 4)
                {
                    const float Angle = Rng.Range(0.0f, 6.2831853f);
                    const FVector2 Edge { Body.Position.x + Math::Cos(Angle) * Maw.Radius,
                                          Body.Position.y + Math::Sin(Angle) * Maw.Radius };
                    SpawnParticle(Registry, Edge, { (Body.Position.x - Edge.x) * 1.6f, (Body.Position.y - Edge.y) * 1.6f },
                        { 1.10f, 0.42f, 1.70f, 1.0f }, { 0.20f, 0.02f, 0.40f, 0.0f }, 4.0f, 0.55f, 0.4f);
                }
            }

            for (auto [Entity, Arc] : Registry.View<FArc>().Each())
            {
                Arc.Age += Delta;
                if (Arc.Age >= Arc.Life)
                {
                    Doomed.push_back(Entity);
                }
            }

            for (const ECS::FEntity Entity : Doomed)
            {
                Registry.Destroy(Entity);
            }
        }

        //~ Damage resolution

        int64 Kills = 0;
        const float Souls = Swarm.ApplyDamage(TSpan<const FDamageVolume>(Volumes.data(), Volumes.size()),
            Deaths, DeathColors, Kills);

        Swarm.Compact();

        Stats.SwarmMilliseconds = float(PlatformTime::ToMilliseconds(PlatformTime::Cycles() - SwarmStart));

        Run.Kills += Kills;
        Run.BestKills = Math::Max(Run.BestKills, Run.Kills);

        for (size_t Index = 0; Index < Deaths.size(); ++Index)
        {
            SpawnBurst(Registry, Deaths[Index], DeathColors[Index], 5, 60.0f, 320.0f);
            if (Index % 3 == 0)
            {
                SpawnMote(Registry, Deaths[Index], 1.0f);
            }
        }

        if (Kills > 0)
        {
            Run.Souls += Souls * 0.35f;
            PlaySound(Registry, ESound::AgentDie, Rng.Range(0.85f, 1.2f), Math::Min(0.35f, float(Kills) * 0.05f));
        }

        //~ Motes and pickups

        {
            TVector<ECS::FEntity> Doomed;

            for (auto [Entity, Body, Mote, Visual] : Registry.View<FBody, FSoulMote, FVisual>().Each())
            {
                Mote.Age += Delta;
                const FVector2 ToPlayer = Player.Position - Body.Position;
                const float Distance = VectorLength(ToPlayer);

                if (Distance < kPickupRadius || Mote.bDrawn)
                {
                    Mote.bDrawn = true;
                    Body.Position += Normalized(ToPlayer) * Math::Min(1400.0f, 420.0f + (kPickupRadius - Distance) * 6.0f) * Delta;
                }

                Visual.Glow = 0.45f + 0.15f * Math::Sin(Run.Elapsed * 9.0f + Mote.Age * 4.0f);

                if (Distance < 34.0f || Mote.Age > 26.0f)
                {
                    Doomed.push_back(Entity);
                    Run.Souls += Mote.Value;
                }
            }

            for (const ECS::FEntity Entity : Doomed)
            {
                Registry.Destroy(Entity);
            }

            if (!Doomed.empty())
            {
                PlaySound(Registry, ESound::SoulPickup, Rng.Range(1.0f, 1.35f), 0.4f);
            }
        }

        if (Run.Souls >= Run.SoulsNeeded)
        {
            Run.Souls -= Run.SoulsNeeded;
            Run.SoulsNeeded *= 1.32f;
            Run.Level += 1;
            Run.Phase = EPhase::LevelUp;
            Run.PhaseTimer = 0.0f;
            Run.ChoiceCount = 3;

            int32 Pool[int32(EWeapon::Count)];
            for (int32 Index = 0; Index < int32(EWeapon::Count); ++Index)
            {
                Pool[Index] = Index;
            }
            for (int32 Index = int32(EWeapon::Count) - 1; Index > 0; --Index)
            {
                const int32 Swap = int32(Rng.Range(0.0f, float(Index) + 0.99f));
                const int32 Temp = Pool[Index];
                Pool[Index] = Pool[Swap];
                Pool[Swap] = Temp;
            }
            for (int32 Slot = 0; Slot < 3; ++Slot)
            {
                Run.Choices[Slot] = Pool[Slot];
            }
            PlaySound(Registry, ESound::LevelUp);
            AddTrauma(Registry, 0.3f);
        }

        //~ Contact damage

        FVector2 Push { 0.0f, 0.0f };
        const float Contact = Swarm.SampleContact(Player.Position, kPlayerRadius + 14.0f, Push);

        if (Contact > 0.0f && Player.Invuln <= 0.0f)
        {
            Player.Health -= Math::Min(Contact, 26.0f) * Delta * 2.4f;
            Player.HurtFlash = 1.0f;
            AddTrauma(Registry, 0.10f);

            if (Player.Health <= 0.0f)
            {
                Player.Health = 0.0f;
                Run.Phase = EPhase::Dead;
                Run.PhaseTimer = 0.0f;
                AddTrauma(Registry, 1.0f);
                Run.HitStop = 0.12f;
                PlaySound(Registry, ESound::PlayerDie);
            }
            else if (Rng.Unit() < Delta * 6.0f)
            {
                PlaySound(Registry, ESound::PlayerHurt, Rng.Range(0.9f, 1.1f), 0.8f);
            }
        }

        Run.Danger = Math::Clamp(1.0f - Player.Health / kPlayerMaxHealth, 0.0f, 1.0f);
        Stats.Agents = Swarm.Num();
    }

    void FGame::StepVisual(float Delta)
    {
        FRunState& Run = Registry.GetSingleton<FRunState>();
        FRandom& Rng = Registry.GetSingleton<FRandom>();

        Run.ShakeTrauma = Math::Max(0.0f, Run.ShakeTrauma - Delta * 1.8f);
        const float Amount = Run.ShakeTrauma * Run.ShakeTrauma * 26.0f;
        Run.ShakeOffset = { Rng.Range(-Amount, Amount), Rng.Range(-Amount, Amount) };

        TVector<ECS::FEntity> Doomed;

        for (auto [Entity, Body, Particle, Visual] : Registry.View<FBody, FParticle, FVisual>().Each())
        {
            Particle.Life -= Delta;
            if (Particle.Life <= 0.0f)
            {
                Doomed.push_back(Entity);
                continue;
            }

            Particle.Velocity -= Particle.Velocity * Math::Min(1.0f, Particle.Drag * Delta);
            Body.Position += Particle.Velocity * Delta;

            const float Alpha = 1.0f - Particle.Life / Particle.MaxLife;
            const float Size = Math::Lerp(Particle.StartSize, Particle.EndSize, Alpha);
            Body.HalfSize = { Size, Size };
            Visual.Color = { Math::Lerp(Visual.Accent.x, Particle.EndColor.x, Alpha),
                             Math::Lerp(Visual.Accent.y, Particle.EndColor.y, Alpha),
                             Math::Lerp(Visual.Accent.z, Particle.EndColor.z, Alpha),
                             Math::Lerp(Visual.Accent.w, Particle.EndColor.w, Alpha) };
        }

        for (auto [Entity, Number] : Registry.View<FDamageNumber>().Each())
        {
            Number.Age += Delta;
            Number.Position.y -= 60.0f * Delta;
            if (Number.Age > 0.8f)
            {
                Doomed.push_back(Entity);
            }
        }

        for (const ECS::FEntity Entity : Doomed)
        {
            Registry.Destroy(Entity);
        }

        Stats.Particles = CountParticles(Registry);
    }

    void FGame::OnKey(Lumina::EKey Key, bool bPressed)
    {
        FFrameInput& Input = Registry.GetSingleton<FFrameInput>();

        switch (Key)
        {
        case EKey::W: case EKey::Up:    Input.bUp = bPressed; return;
        case EKey::S: case EKey::Down:  Input.bDown = bPressed; return;
        case EKey::A: case EKey::Left:  Input.bLeft = bPressed; return;
        case EKey::D: case EKey::Right: Input.bRight = bPressed; return;
        default: break;
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
            break;
        case EKey::D1: Input.Choice = 0; break;
        case EKey::D2: Input.Choice = 1; break;
        case EKey::D3: Input.Choice = 2; break;
        case EKey::P:
            bPaused = !bPaused;
            break;
        case EKey::Escape:
            bQuitRequested = bPaused;
            bPaused = true;
            break;
        case EKey::F3:
            bShowStats = !bShowStats;
            break;
        default:
            break;
        }
    }

    void FGame::OnMouseMoved(const FVector2& ViewPosition)
    {
        Registry.GetSingleton<FFrameInput>().MouseView = ViewPosition;
    }

    void FGame::OnMouseClick()
    {
        FFrameInput& Input = Registry.GetSingleton<FFrameInput>();
        Input.bClick = true;

        const EPhase Phase = Registry.GetSingleton<FRunState>().Phase;
        if (Phase == EPhase::Title || Phase == EPhase::Dead)
        {
            Input.bConfirm = true;
        }
    }

    void FGame::SetViewSize(const FVector2& ViewSize)
    {
        Registry.GetSingleton<FFrameInput>().ViewSize = ViewSize;
    }

    const char* WeaponName(EWeapon Weapon)
    {
        return kWeaponNames[int32(Weapon)];
    }

    const char* WeaponTagline(EWeapon Weapon)
    {
        return kWeaponTaglines[int32(Weapon)];
    }

    const char* WeaponUpgrade(EWeapon Weapon, int32 NextLevel)
    {
        if (NextLevel <= 1)
        {
            return "UNLOCK THIS POWER";
        }

        switch (Weapon)
        {
        case EWeapon::Soulbolt: return NextLevel % 3 == 0 ? "+1 SHARD PER VOLLEY" : "FASTER AND SHARPER";
        case EWeapon::Nova:     return "WIDER BLAST, SHORTER WAIT";
        case EWeapon::Pyre:     return "LARGER POOLS, LIT MORE OFTEN";
        case EWeapon::Maw:      return "STRONGER PULL, WIDER MOUTH";
        case EWeapon::Chain:    return "+1 LEAP, LONGER REACH";
        case EWeapon::Gloom:    return "SLOWS HARDER, REACHES FURTHER";
        default:                return "+1 BLADE, WIDER ORBIT";
        }
    }
}
