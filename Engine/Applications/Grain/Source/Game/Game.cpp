#include "Game.h"

#include "Containers/StringFormat.h"
#include "Log/Log.h"
#include "Render/Renderer.h"

namespace Grain
{
    namespace
    {
        constexpr float kGravity        = -21.0f;
        constexpr float kWalkSpeed      = 5.4f;
        constexpr float kSprintSpeed    = 9.2f;
        constexpr float kSwimSpeed      = 3.6f;
        constexpr float kJumpSpeed      = 7.4f;
        constexpr float kBuoyancy       = 26.0f;
        constexpr float kStepHeight     = 0.62f;
        constexpr float kPlayerHeight   = 1.80f;
        constexpr float kPlayerRadius   = 0.30f;

        constexpr float kSwingReach     = 3.0f;
        constexpr float kSwingArc       = 0.55f;
        constexpr float kDigRadius      = 0.62f;
        constexpr float kBoltRadius     = 1.75f;
        constexpr float kFocusPerCast   = 14.0f;

        constexpr int32 kMaxLiveEnemies = 22;
        constexpr int32 kDebrisBudget   = 26;
        constexpr int32 kSparkBudget    = 22;
        constexpr float kLeashRange     = 52.0f;

        float Length(const FVector3& V)
        {
            return Math::Sqrt(V.x * V.x + V.y * V.y + V.z * V.z);
        }

        FVector3 Normalized(const FVector3& V)
        {
            const float L = Length(V);
            return L > 1e-5f ? FVector3{ V.x / L, V.y / L, V.z / L } : FVector3{ 0.0f, 0.0f, 1.0f };
        }

        float Distance(const FVector3& A, const FVector3& B)
        {
            return Length({ A.x - B.x, A.y - B.y, A.z - B.z });
        }

    }

    float FGame::RandomUnit()
    {
        RandomState = RandomState * 1664525u + 1013904223u;
        return float((RandomState >> 8) & 0xFFFFFFu) / 16777216.0f;
    }

    FVector3 FGame::RandomHorizontal()
    {
        const float Angle = RandomUnit() * 6.2831853f;
        return { Math::Cos(Angle), 0.0f, Math::Sin(Angle) };
    }

    void FGame::Initialize(FVoxelWorld& InWorld, uint32 Seed)
    {
        World = &InWorld;
        RandomState = Seed | 1u;

        Models.Build();
        Entities.assign(size_t(kMaxEntities), FEntity{});

        SpawnPlayer();
        PlaceBeacons();
        RefreshObjectives();

        Say("Ember Vale. Light the beacons.", 5.0f);
    }

    void FGame::SpawnPlayer()
    {
        const FVoxelPhysics Physics(*World);

        Player = FPlayerState{};
        Player.Body.HalfExtent = { kPlayerRadius, kPlayerHeight * 0.5f, kPlayerRadius };

        // A dry, walkable start beats the exact middle of the map, which is often open water.
        FVector3 Best { kWorldSizeX * 0.5f, 0.0f, kWorldSizeZ * 0.5f };
        float BestHeight = -1e9f;

        for (int32 Attempt = 0; Attempt < 220; ++Attempt)
        {
            const float X = kWorldSizeX * (0.22f + RandomUnit() * 0.56f);
            const float Z = kWorldSizeZ * (0.22f + RandomUnit() * 0.56f);
            const float Height = World->SampleHeight(X, Z);

            if (Height > kSeaLevel + 2.5f && Height < kSeaLevel + 16.0f && Height > BestHeight)
            {
                BestHeight = Height;
                Best = { X, Height, Z };
            }
        }

        float Ground = BestHeight > -1e8f ? BestHeight : kSeaLevel + 4.0f;
        Physics.FindGround(Best.x, Best.z, Ground + 12.0f, Ground);

        Player.Body.Position = { Best.x, Ground + kPlayerHeight * 0.5f + 0.2f, Best.z };
        Player.Yaw = RandomUnit() * 6.2831853f;

        CameraPosition = Player.Body.Position;

        LOG_INFO("Grain: delver at {:.1f} {:.1f} {:.1f}.",
            Player.Body.Position.x, Player.Body.Position.y, Player.Body.Position.z);
    }

    void FGame::PlaceBeacons()
    {
        const FVoxelPhysics Physics(*World);
        const FVector3 Start = Player.Body.Position;

        constexpr int32 kBeacons = 3;
        constexpr float kMinRange = 34.0f;

        for (int32 Index = 0; Index < kBeacons; ++Index)
        {
            for (int32 Attempt = 0; Attempt < 400; ++Attempt)
            {
                const float X = kWorldSizeX * (0.14f + RandomUnit() * 0.72f);
                const float Z = kWorldSizeZ * (0.14f + RandomUnit() * 0.72f);
                const float Height = World->SampleHeight(X, Z);

                if (Height < kSeaLevel + 2.0f)
                {
                    continue;
                }

                if (Distance({ X, Height, Z }, Start) < kMinRange)
                {
                    continue;
                }

                bool bClear = true;
                for (const FEntity& Other : Entities)
                {
                    if (Other.bActive && Other.Kind == EEntityKind::Beacon
                        && Distance({ X, Height, Z }, Other.Body.Position) < kMinRange)
                    {
                        bClear = false;
                        break;
                    }
                }

                if (!bClear)
                {
                    continue;
                }

                float Ground = Height;
                Physics.FindGround(X, Z, Height + 10.0f, Ground);

                FEntity* Entity = Allocate();
                if (Entity == nullptr)
                {
                    return;
                }

                const FVector3 Half = Models.HalfExtentOf(EModel::Beacon, 1.6f);

                Entity->Kind = EEntityKind::Beacon;
                Entity->Model = EModel::Beacon;
                Entity->Scale = 1.6f;
                Entity->Body.HalfExtent = Half;
                Entity->Body.Position = { X, Ground + Half.y - 0.2f, Z };
                Entity->bGravity = false;
                Entity->Tint = { 0.55f, 0.58f, 0.66f };
                Entity->Emissive = 0.0f;
                Entity->Yaw = RandomUnit() * 6.2831853f;
                ++TotalBeacons;
                break;
            }
        }

        LOG_INFO("Grain: {} beacons placed.", TotalBeacons);
    }

    FEntity* FGame::Allocate()
    {
        for (FEntity& Entity : Entities)
        {
            if (!Entity.bActive)
            {
                const FEntity Fresh;
                Entity = Fresh;
                Entity.bActive = true;
                return &Entity;
            }
        }
        return nullptr;
    }

    // Scrap has its own budget, so a burst of digging can never crowd out an enemy or a pickup.
    FEntity* FGame::AllocateScrap(EEntityKind Kind, int32 Budget)
    {
        int32 Live = 0;
        FEntity* Oldest = nullptr;

        for (FEntity& Entity : Entities)
        {
            if (Entity.bActive && Entity.Kind == Kind)
            {
                ++Live;
                if (Oldest == nullptr || Entity.Age > Oldest->Age)
                {
                    Oldest = &Entity;
                }
            }
        }

        if (Live < Budget)
        {
            if (FEntity* Fresh = Allocate())
            {
                return Fresh;
            }
        }

        if (Oldest != nullptr)
        {
            const FEntity Fresh;
            *Oldest = Fresh;
            Oldest->bActive = true;
        }
        return Oldest;
    }

    void FGame::SpawnEnemy(EEntityKind Kind, const FVector3& Position)
    {
        FEntity* Entity = Allocate();
        if (Entity == nullptr)
        {
            return;
        }

        Entity->Kind = Kind;
        Entity->Yaw = RandomUnit() * 6.2831853f;

        switch (Kind)
        {
        case EEntityKind::Wisp:
            Entity->Model = EModel::Wisp;
            Entity->Scale = 1.0f;
            Entity->MaxHealth = 26.0f + float(Player.Level) * 4.0f;
            Entity->bGravity = false;
            Entity->Emissive = 1.0f;
            Entity->Tint = { 1.0f, 1.0f, 1.0f };
            break;

        case EEntityKind::Golem:
            Entity->Model = EModel::Golem;
            Entity->Scale = 1.35f;
            Entity->MaxHealth = 120.0f + float(Player.Level) * 22.0f;
            Entity->Tint = { 0.86f, 0.84f, 0.82f };
            break;

        default:
            Entity->Kind = EEntityKind::Husk;
            Entity->Model = EModel::Husk;
            Entity->Scale = 1.0f;
            Entity->MaxHealth = 44.0f + float(Player.Level) * 9.0f;
            Entity->Tint = { 1.0f, 1.0f, 1.0f };
            break;
        }

        Entity->Health = Entity->MaxHealth;
        Entity->Body.HalfExtent = Models.HalfExtentOf(Entity->Model, Entity->Scale);
        Entity->Body.Position = Position;
        Entity->Body.Position.y += Entity->Body.HalfExtent.y;
        Entity->Cooldown = 0.6f + RandomUnit();
    }

    void FGame::SpawnShard(const FVector3& Position, int32 Count)
    {
        for (int32 i = 0; i < Count; ++i)
        {
            FEntity* Entity = Allocate();
            if (Entity == nullptr)
            {
                return;
            }

            Entity->Kind = EEntityKind::Shard;
            Entity->Model = EModel::Shard;
            Entity->Scale = 0.85f;
            Entity->Emissive = 1.0f;
            Entity->Body.HalfExtent = Models.HalfExtentOf(EModel::Shard, 0.85f);
            Entity->Body.Position = Position;
            Entity->Body.Velocity = { (RandomUnit() - 0.5f) * 4.0f, 3.0f + RandomUnit() * 2.5f,
                                      (RandomUnit() - 0.5f) * 4.0f };
            Entity->Lifetime = 75.0f;
            Entity->Bob = RandomUnit() * 6.2831853f;
        }
    }

    void FGame::SpawnDebris(const FVector3& Position, const FVector3& Impulse, uint8 Material, int32 Count)
    {
        for (int32 i = 0; i < Count; ++i)
        {
            FEntity* Entity = AllocateScrap(EEntityKind::Debris, kDebrisBudget);
            if (Entity == nullptr)
            {
                return;
            }

            const float Scale = 0.5f + RandomUnit() * 0.9f;

            Entity->Kind = EEntityKind::Debris;
            Entity->Model = EModel::Debris;
            Entity->Scale = Scale;
            Entity->Body.HalfExtent = Models.HalfExtentOf(EModel::Debris, Scale);
            Entity->Body.Position = Position;
            Entity->Body.Velocity =
            {
                Impulse.x + (RandomUnit() - 0.5f) * 7.0f,
                Impulse.y + RandomUnit() * 5.5f,
                Impulse.z + (RandomUnit() - 0.5f) * 7.0f,
            };
            Entity->Lifetime = 4.5f + RandomUnit() * 3.0f;
            Entity->Yaw = RandomUnit() * 6.2831853f;
            Entity->Owner = Material;

            // Debris borrows the stone model and takes its color from what was actually broken.
            switch (EMaterial(Material))
            {
            case EMaterial::Grass:  Entity->Tint = { 0.55f, 0.95f, 0.40f }; break;
            case EMaterial::Dirt:   Entity->Tint = { 0.80f, 0.55f, 0.36f }; break;
            case EMaterial::Sand:   Entity->Tint = { 1.90f, 1.65f, 1.10f }; break;
            case EMaterial::Snow:   Entity->Tint = { 2.50f, 2.65f, 2.85f }; break;
            case EMaterial::Wood:   Entity->Tint = { 0.80f, 0.52f, 0.30f }; break;
            case EMaterial::Leaves: Entity->Tint = { 0.45f, 0.95f, 0.40f }; break;
            case EMaterial::Ore:    Entity->Tint = { 1.30f, 1.15f, 0.80f }; break;
            case EMaterial::Crystal:
                Entity->Tint = { 0.75f, 1.60f, 2.10f };
                Entity->Emissive = 0.6f;
                break;
            default: Entity->Tint = { 1.0f, 1.0f, 1.0f }; break;
            }
        }
    }

    void FGame::SpawnSpark(const FVector3& Position, const FVector3& Color, int32 Count)
    {
        for (int32 i = 0; i < Count; ++i)
        {
            FEntity* Entity = AllocateScrap(EEntityKind::Spark, kSparkBudget);
            if (Entity == nullptr)
            {
                return;
            }

            const float Scale = 0.22f + RandomUnit() * 0.3f;

            Entity->Kind = EEntityKind::Spark;
            Entity->Model = EModel::Bolt;
            Entity->Scale = Scale;
            Entity->Body.HalfExtent = Models.HalfExtentOf(EModel::Bolt, Scale);
            Entity->Body.Position = Position;
            Entity->Body.Velocity =
            {
                (RandomUnit() - 0.5f) * 9.0f,
                2.0f + RandomUnit() * 6.0f,
                (RandomUnit() - 0.5f) * 9.0f,
            };
            Entity->Lifetime = 0.5f + RandomUnit() * 0.7f;
            Entity->Tint = Color;
            Entity->Emissive = 1.0f;
            Entity->bGravity = true;
        }
    }

    FVector3 FGame::GetEyePosition() const
    {
        return { Player.Body.Position.x,
                 Player.Body.Position.y + kPlayerHeight * 0.34f,
                 Player.Body.Position.z };
    }

    FVector3 FGame::GetLookDirection() const
    {
        const float CosPitch = Math::Cos(Player.Pitch);
        return { Math::Cos(Player.Yaw) * CosPitch, Math::Sin(Player.Pitch),
                 Math::Sin(Player.Yaw) * CosPitch };
    }

    void FGame::Update(float Delta, const FGameInput& Input, FRenderer& Renderer)
    {
        Delta = Math::Min(Delta, 0.05f);

        // Long enough to play inside one hour of it, short enough that the cycle is visible.
        TimeOfDay += Delta / DayLength;
        TimeOfDay -= Math::Floor(TimeOfDay);
        Sky = EvaluateSky(TimeOfDay);

        UpdatePlayer(Delta, Input, Renderer);
        UpdateEntities(Delta, Renderer);
        UpdateSpawning(Delta);
        UpdateCamera(Delta);

        for (size_t i = 0; i < Floaters.size();)
        {
            Floaters[i].Age += Delta;
            if (Floaters[i].Age >= Floaters[i].Life)
            {
                Floaters[i] = Floaters.back();
                Floaters.pop_back();
                continue;
            }
            ++i;
        }

        if (Banner.Life > 0.0f)
        {
            Banner.Age += Delta;
            if (Banner.Age >= Banner.Life)
            {
                Banner.Life = 0.0f;
            }
        }
    }

    void FGame::UpdatePlayer(float Delta, const FGameInput& Input, FRenderer& Renderer)
    {
        const FVoxelPhysics Physics(*World);

        Player.Yaw += Input.LookX;
        Player.Pitch = Math::Clamp(Player.Pitch - Input.LookY, -1.35f, 1.35f);

        Player.SwingTimer = Math::Max(Player.SwingTimer - Delta, 0.0f);
        Player.AttackCooldown = Math::Max(Player.AttackCooldown - Delta, 0.0f);
        Player.CastCooldown = Math::Max(Player.CastCooldown - Delta, 0.0f);
        Player.HurtTimer = Math::Max(Player.HurtTimer - Delta, 0.0f);

        if (!Player.bAlive)
        {
            Player.DeathTimer += Delta;
            if (Player.DeathTimer > 3.0f)
            {
                SpawnPlayer();
                Player.Health = Player.MaxHealth * 0.6f;
                Player.Shards = Math::Max(Player.Shards - 2, 0);
                Say("The vale spits you back out.", 3.0f);
                RefreshObjectives();
            }
            return;
        }

        const bool bSwimming = Player.Body.bInWater;

        const FVector3 Forward { Math::Cos(Player.Yaw), 0.0f, Math::Sin(Player.Yaw) };
        const FVector3 Right { -Math::Sin(Player.Yaw), 0.0f, Math::Cos(Player.Yaw) };

        FVector3 Wish
        {
            Forward.x * Input.Move.y + Right.x * Input.Move.x,
            0.0f,
            Forward.z * Input.Move.y + Right.z * Input.Move.x,
        };

        const float WishLength = Length(Wish);
        if (WishLength > 1.0f)
        {
            Wish = { Wish.x / WishLength, 0.0f, Wish.z / WishLength };
        }

        const bool bMoving = WishLength > 0.01f;
        Player.bSprinting = Input.bSprint && bMoving && Player.Stamina > 1.0f && !bSwimming;

        float Speed = bSwimming ? kSwimSpeed : (Player.bSprinting ? kSprintSpeed : kWalkSpeed);
        if (!Player.Body.bGrounded && !bSwimming)
        {
            Speed *= 0.86f;
        }

        Player.Stamina = Player.bSprinting
            ? Math::Max(Player.Stamina - Delta * 19.0f, 0.0f)
            : Math::Min(Player.Stamina + Delta * 13.0f, Player.MaxStamina);

        Player.Focus = Math::Min(Player.Focus + Delta * 4.6f, Player.MaxFocus);

        Player.Recovery = Player.HurtTimer > 0.0f ? 0.0f : Player.Recovery + Delta;
        if (Player.Recovery > 4.5f)
        {
            Player.Health = Math::Min(Player.Health + Delta * 5.5f, Player.MaxHealth);
        }

        // A snappy ground response with air control kept low is what makes a jump feel committed.
        const float Control = Player.Body.bGrounded || bSwimming ? 16.0f : 3.4f;
        const float Blend = 1.0f - Math::Exp(-Control * Delta);

        Player.Body.Velocity.x = Math::Lerp(Player.Body.Velocity.x, Wish.x * Speed, Blend);
        Player.Body.Velocity.z = Math::Lerp(Player.Body.Velocity.z, Wish.z * Speed, Blend);

        if (bSwimming)
        {
            // Buoyancy balances gravity a little short of fully under, so the head rides clear.
            Player.Body.Velocity.y += (kGravity + kBuoyancy * Player.Body.Submersion) * Delta;
            Player.Body.Velocity.y *= Math::Exp(-2.6f * Delta);

            if (Input.bJump)
            {
                Player.Body.Velocity.y += 24.0f * Delta;
            }
        }
        else
        {
            Player.Body.Velocity.y += kGravity * Delta;

            if (Input.bJump && Player.Body.bGrounded)
            {
                Player.Body.Velocity.y = kJumpSpeed;
            }
        }

        Player.Body.Velocity.y = Math::Max(Player.Body.Velocity.y, -46.0f);

        const float FallSpeed = Player.Body.Velocity.y;
        const bool bWasFalling = !Player.Body.bGrounded;

        const FVector3 Step
        {
            Player.Body.Velocity.x * Delta,
            Player.Body.Velocity.y * Delta,
            Player.Body.Velocity.z * Delta,
        };

        Physics.MoveBody(Player.Body, Step, kStepHeight);

        // Falling hurts past terminal walking speed, which is what makes a cliff read as a threat.
        if (bWasFalling && Player.Body.bGrounded && FallSpeed < -17.0f
            && !bSwimming && !Player.Body.bInWater)
        {
            const float Damage = (-FallSpeed - 17.0f) * 3.4f;
            DamagePlayer(Damage);
            SpawnDebris(Player.Body.Position, { 0.0f, 1.0f, 0.0f }, uint8(EMaterial::Dirt), 5);
        }

        Player.Body.Position.x = Math::Clamp(Player.Body.Position.x, 1.5f, kWorldSizeX - 1.5f);
        Player.Body.Position.z = Math::Clamp(Player.Body.Position.z, 1.5f, kWorldSizeZ - 1.5f);

        if (Input.bAttack && Player.AttackCooldown <= 0.0f)
        {
            Swing(Renderer);
        }

        if (Input.bCast && Player.CastCooldown <= 0.0f && Player.Focus >= kFocusPerCast)
        {
            Cast();
        }

        //~ Beacons light where the player is standing close enough and carrying enough shards.

        Prompt = "";
        constexpr int32 kShardsPerBeacon = 4;

        for (FEntity& Entity : Entities)
        {
            if (!Entity.bActive || Entity.Kind != EEntityKind::Beacon || Entity.bLit)
            {
                continue;
            }

            if (Distance(Entity.Body.Position, Player.Body.Position) > 4.5f)
            {
                continue;
            }

            if (Player.Shards < kShardsPerBeacon)
            {
                Prompt = "Need 4 shards to light this beacon";
                continue;
            }

            Prompt = "Hold E to light the beacon";

            if (!Input.bInteract)
            {
                continue;
            }

            Entity.bLit = true;
            Entity.Emissive = 1.0f;
            Entity.Tint = { 1.4f, 1.1f, 0.8f };
            Player.Shards -= kShardsPerBeacon;
            ++BeaconsLit;

            SpawnSpark(Entity.Body.Position, { 2.4f, 1.2f, 0.4f }, 26);
            GrantExperience(140);
            Prompt = "";

            if (BeaconsLit >= TotalBeacons)
            {
                bComplete = true;
                Say("Every beacon burns. The vale is yours.", 9.0f);
            }
            else
            {
                Say("Beacon lit.", 3.0f);
            }

            RefreshObjectives();
        }
    }

    void FGame::Swing(FRenderer& Renderer)
    {
        const FVoxelPhysics Physics(*World);

        Player.AttackCooldown = 0.42f;
        Player.SwingTimer = 0.30f;

        const FVector3 Eye = GetEyePosition();
        const FVector3 Look = GetLookDirection();

        const float Damage = 16.0f + float(Player.Level) * 6.0f;

        //~ Anything in the arc takes the hit, and only an empty swing reaches the terrain.

        bool bStruck = false;

        for (FEntity& Entity : Entities)
        {
            if (!Entity.bActive || Entity.Health <= 0.0f)
            {
                continue;
            }

            const bool bHostile = Entity.Kind == EEntityKind::Husk || Entity.Kind == EEntityKind::Wisp
                               || Entity.Kind == EEntityKind::Golem;
            if (!bHostile && Entity.Kind != EEntityKind::Chest)
            {
                continue;
            }

            const FVector3 ToTarget
            {
                Entity.Body.Position.x - Eye.x,
                Entity.Body.Position.y - Eye.y,
                Entity.Body.Position.z - Eye.z,
            };

            const float Range = Length(ToTarget);
            if (Range > kSwingReach + Entity.Body.HalfExtent.y)
            {
                continue;
            }

            const FVector3 Direction = Normalized(ToTarget);
            if (Direction.x * Look.x + Direction.y * Look.y + Direction.z * Look.z < 1.0f - kSwingArc)
            {
                continue;
            }

            DamageEntity(Entity, Damage, Eye);
            bStruck = true;
        }

        if (bStruck)
        {
            return;
        }

        const FVoxelRayHit Hit = Physics.Raycast(Eye, Look, kSwingReach);
        if (!Hit.bHit)
        {
            return;
        }

        const FVector3 Center
        {
            Hit.Position.x - Hit.Normal.x * 0.12f,
            Hit.Position.y - Hit.Normal.y * 0.12f,
            Hit.Position.z - Hit.Normal.z * 0.12f,
        };

        World->CarveSphere(Center, kDigRadius);

        FDestroyRequest Request;
        Request.bExplicit = true;
        Request.Center = Center;
        Request.Radius = kDigRadius;
        Renderer.QueueDestroy(Request);

        SpawnDebris(Hit.Position, { Hit.Normal.x * 2.5f, 2.0f, Hit.Normal.z * 2.5f }, Hit.Material, 6);

        // Ore and crystal are the reason to dig at all, so only those pay out.
        if (Hit.Material == uint8(EMaterial::Crystal) || Hit.Material == uint8(EMaterial::Amethyst))
        {
            SpawnShard(Hit.Position, 1 + int32(RandomUnit() * 2.0f));
        }
        else if (Hit.Material == uint8(EMaterial::Ore) && RandomUnit() < 0.45f)
        {
            SpawnShard(Hit.Position, 1);
            GrantExperience(6);
        }
    }

    void FGame::Cast()
    {
        FEntity* Bolt = Allocate();
        if (Bolt == nullptr)
        {
            return;
        }

        Player.CastCooldown = 0.62f;
        Player.Focus -= kFocusPerCast;

        const FVector3 Eye = GetEyePosition();
        const FVector3 Look = GetLookDirection();

        Bolt->Kind = EEntityKind::Bolt;
        Bolt->Model = EModel::Bolt;
        Bolt->Scale = 1.1f;
        Bolt->Emissive = 1.0f;
        Bolt->Tint = { 1.5f, 0.9f, 0.45f };
        Bolt->Body.HalfExtent = Models.HalfExtentOf(EModel::Bolt, 1.1f);
        Bolt->Body.Position = { Eye.x + Look.x * 0.8f, Eye.y + Look.y * 0.8f, Eye.z + Look.z * 0.8f };
        Bolt->Body.Velocity = { Look.x * 34.0f, Look.y * 34.0f + 2.0f, Look.z * 34.0f };
        Bolt->Lifetime = 4.0f;
        Bolt->bGravity = true;
    }

    void FGame::Explode(const FVector3& Center, float Radius, float Damage, FRenderer& Renderer)
    {
        World->CarveSphere(Center, Radius);

        FDestroyRequest Request;
        Request.bExplicit = true;
        Request.Center = Center;
        Request.Radius = Radius;
        Renderer.QueueDestroy(Request);

        SpawnSpark(Center, { 2.6f, 1.3f, 0.4f }, 20);
        SpawnDebris(Center, { 0.0f, 5.0f, 0.0f }, uint8(EMaterial::Stone), 12);

        for (FEntity& Entity : Entities)
        {
            if (!Entity.bActive || Entity.Health <= 0.0f)
            {
                continue;
            }

            const bool bHostile = Entity.Kind == EEntityKind::Husk || Entity.Kind == EEntityKind::Wisp
                               || Entity.Kind == EEntityKind::Golem;
            if (!bHostile)
            {
                continue;
            }

            const float Range = Distance(Entity.Body.Position, Center);
            if (Range > Radius * 2.6f)
            {
                continue;
            }

            const float Falloff = 1.0f - Math::Min(Range / (Radius * 2.6f), 1.0f);
            DamageEntity(Entity, Damage * Falloff, Center);
        }

        const float PlayerRange = Distance(Player.Body.Position, Center);
        if (PlayerRange < Radius * 2.2f)
        {
            DamagePlayer(Damage * 0.35f * (1.0f - PlayerRange / (Radius * 2.2f)));
        }
    }

    void FGame::UpdateEntities(float Delta, FRenderer& Renderer)
    {
        const FVoxelPhysics Physics(*World);
        const FVector3 PlayerCenter = Player.Body.Position;

        for (FEntity& Entity : Entities)
        {
            if (!Entity.bActive)
            {
                continue;
            }

            Entity.Age += Delta;
            Entity.HurtFlash = Math::Max(Entity.HurtFlash - Delta * 3.4f, 0.0f);
            Entity.Cooldown = Math::Max(Entity.Cooldown - Delta, 0.0f);

            if (Entity.Lifetime > 0.0f && Entity.Age >= Entity.Lifetime)
            {
                Entity.bActive = false;
                continue;
            }

            const float Range = Distance(Entity.Body.Position, PlayerCenter);

            switch (Entity.Kind)
            {
            case EEntityKind::Beacon:
            {
                if (Entity.bLit)
                {
                    Entity.Emissive = 1.0f + Math::Sin(Entity.Age * 2.1f) * 0.25f;
                }
                continue;
            }

            case EEntityKind::Shard:
            {
                Entity.Bob += Delta * 2.6f;
                Entity.Yaw += Delta * 1.9f;
                Entity.Emissive = 1.0f + Math::Sin(Entity.Bob) * 0.35f;

                if (Range < 2.2f && Player.bAlive)
                {
                    Entity.bActive = false;
                    ++Player.Shards;
                    GrantExperience(12);
                    Floater(Entity.Body.Position, FString("+1 shard"), { 0.45f, 1.60f, 2.10f });
                    RefreshObjectives();
                    continue;
                }
                break;
            }

            case EEntityKind::Bolt:
            {
                const FVector3 Before = Entity.Body.Position;
                Entity.Emissive = 1.6f;

                Entity.Body.Velocity.y += kGravity * 0.45f * Delta;

                const FVector3 Step
                {
                    Entity.Body.Velocity.x * Delta,
                    Entity.Body.Velocity.y * Delta,
                    Entity.Body.Velocity.z * Delta,
                };

                const FVoxelRayHit Hit = Physics.Raycast(Before, Step, Length(Step));

                bool bDetonate = Hit.bHit;
                FVector3 Impact = Hit.bHit ? Hit.Position : FVector3{ Before.x + Step.x,
                                                                      Before.y + Step.y,
                                                                      Before.z + Step.z };

                if (!bDetonate)
                {
                    for (const FEntity& Other : Entities)
                    {
                        if (!Other.bActive || Other.Health <= 0.0f)
                        {
                            continue;
                        }

                        const bool bHostile = Other.Kind == EEntityKind::Husk
                                           || Other.Kind == EEntityKind::Wisp
                                           || Other.Kind == EEntityKind::Golem;
                        if (!bHostile)
                        {
                            continue;
                        }

                        if (Distance(Other.Body.Position, Impact) < Other.Body.HalfExtent.y + 0.8f)
                        {
                            bDetonate = true;
                            break;
                        }
                    }
                }

                if (bDetonate)
                {
                    Entity.bActive = false;
                    Explode(Impact, kBoltRadius, 58.0f + float(Player.Level) * 12.0f, Renderer);
                    continue;
                }

                Entity.Body.Position = Impact;
                SpawnSpark(Impact, { 1.8f, 0.9f, 0.3f }, 1);
                continue;
            }

            case EEntityKind::Spark:
            {
                Entity.Body.Velocity.y += kGravity * 0.6f * Delta;
                Entity.Body.Position.x += Entity.Body.Velocity.x * Delta;
                Entity.Body.Position.y += Entity.Body.Velocity.y * Delta;
                Entity.Body.Position.z += Entity.Body.Velocity.z * Delta;
                Entity.Emissive = 2.0f * Math::Max(1.0f - Entity.Age / Math::Max(Entity.Lifetime, 0.01f), 0.0f);
                continue;
            }

            case EEntityKind::Debris:
            {
                Entity.Body.Velocity.y += kGravity * Delta;
                Entity.Yaw += Delta * 5.0f;

                const FVector3 Step
                {
                    Entity.Body.Velocity.x * Delta,
                    Entity.Body.Velocity.y * Delta,
                    Entity.Body.Velocity.z * Delta,
                };

                Physics.MoveBody(Entity.Body, Step, 0.0f);

                if (Entity.Body.bGrounded)
                {
                    Entity.Body.Velocity.x *= 0.62f;
                    Entity.Body.Velocity.z *= 0.62f;
                }
                continue;
            }

            default:
                break;
            }

            //~ From here the entity is hostile, so it hunts, strikes and falls under gravity.

            if (Entity.Health <= 0.0f)
            {
                Entity.bActive = false;
                continue;
            }

            const bool bAware = Player.bAlive && Range < kLeashRange;

            FVector3 Wish { 0.0f, 0.0f, 0.0f };
            if (bAware)
            {
                const FVector3 ToPlayer
                {
                    PlayerCenter.x - Entity.Body.Position.x,
                    PlayerCenter.y - Entity.Body.Position.y,
                    PlayerCenter.z - Entity.Body.Position.z,
                };

                Wish = Normalized({ ToPlayer.x, 0.0f, ToPlayer.z });
                Entity.Yaw = Math::Atan2(Wish.z, Wish.x);

                const float Reach = Entity.Kind == EEntityKind::Golem ? 3.2f : 2.4f;

                if (Entity.Kind == EEntityKind::Wisp)
                {
                    // A wisp keeps its distance and drifts to the player's head height.
                    const float Preferred = 7.0f;
                    const float Push = Range < Preferred ? -1.0f : 1.0f;
                    Wish = { Wish.x * Push, 0.0f, Wish.z * Push };

                    const float TargetY = PlayerCenter.y + 1.6f + Math::Sin(Entity.Age * 1.3f) * 0.8f;
                    Entity.Body.Velocity.y = Math::Lerp(Entity.Body.Velocity.y,
                        (TargetY - Entity.Body.Position.y) * 1.8f, 1.0f - Math::Exp(-3.0f * Delta));

                    // A wisp only fires when it can actually see the player, or cover means nothing.
                    if (Range < 16.0f && Entity.Cooldown <= 0.0f)
                    {
                        const FVector3 ToEye
                        {
                            PlayerCenter.x - Entity.Body.Position.x,
                            PlayerCenter.y - Entity.Body.Position.y,
                            PlayerCenter.z - Entity.Body.Position.z,
                        };

                        const FVoxelRayHit Blocked = Physics.Raycast(Entity.Body.Position, ToEye, Range);
                        if (!Blocked.bHit)
                        {
                            Entity.Cooldown = 2.8f;
                            DamagePlayer(5.0f + float(Player.Level) * 1.1f);
                            SpawnSpark(Entity.Body.Position, { 0.5f, 1.8f, 2.2f }, 6);
                        }
                        else
                        {
                            Entity.Cooldown = 0.6f;
                        }
                    }
                }
                else if (Range < Reach && Entity.Cooldown <= 0.0f)
                {
                    Entity.Cooldown = Entity.Kind == EEntityKind::Golem ? 2.1f : 1.3f;
                    DamagePlayer(Entity.Kind == EEntityKind::Golem ? 17.0f : 8.0f);
                    SpawnSpark(Player.Body.Position, { 1.6f, 0.3f, 0.25f }, 5);
                }
            }

            const float Speed = Entity.Kind == EEntityKind::Wisp ? 4.4f
                              : (Entity.Kind == EEntityKind::Golem ? 2.6f : 4.0f);

            const float Blend = 1.0f - Math::Exp(-7.0f * Delta);
            Entity.Body.Velocity.x = Math::Lerp(Entity.Body.Velocity.x, Wish.x * Speed, Blend);
            Entity.Body.Velocity.z = Math::Lerp(Entity.Body.Velocity.z, Wish.z * Speed, Blend);

            if (Entity.bGravity)
            {
                Entity.Body.Velocity.y += kGravity * Delta;

                // A walker that runs into a wall hops, which is enough to follow the player up a slope.
                if (Entity.Body.bHitWall && Entity.Body.bGrounded)
                {
                    Entity.Body.Velocity.y = 6.4f;
                }
            }

            const FVector3 Step
            {
                Entity.Body.Velocity.x * Delta,
                Entity.Body.Velocity.y * Delta,
                Entity.Body.Velocity.z * Delta,
            };

            Physics.MoveBody(Entity.Body, Step, kStepHeight);
        }
    }

    void FGame::UpdateSpawning(float Delta)
    {
        if (!Player.bAlive)
        {
            return;
        }

        int32 Live = 0;
        for (const FEntity& Entity : Entities)
        {
            if (Entity.bActive && (Entity.Kind == EEntityKind::Husk || Entity.Kind == EEntityKind::Wisp
                                || Entity.Kind == EEntityKind::Golem))
            {
                ++Live;
            }
        }

        const int32 Allowed = Math::Min(3 + Player.Level * 2, kMaxLiveEnemies);

        SpawnTimer -= Delta;
        if (SpawnTimer > 0.0f || Live >= Allowed)
        {
            return;
        }

        // Night is when the vale turns dangerous, which is the whole reason the cycle is visible.
        const float Night = 1.0f - Sky.DayFactor;
        SpawnTimer = Math::Lerp(7.0f, 2.4f, Night);

        const FVoxelPhysics Physics(*World);

        for (int32 Attempt = 0; Attempt < 24; ++Attempt)
        {
            const FVector3 Direction = RandomHorizontal();
            const float Range = 22.0f + RandomUnit() * 16.0f;

            const float X = Math::Clamp(Player.Body.Position.x + Direction.x * Range, 3.0f, kWorldSizeX - 3.0f);
            const float Z = Math::Clamp(Player.Body.Position.z + Direction.z * Range, 3.0f, kWorldSizeZ - 3.0f);

            float Ground = World->SampleHeight(X, Z);
            if (Ground < kSeaLevel + 0.5f)
            {
                continue;
            }

            if (!Physics.FindGround(X, Z, Ground + 12.0f, Ground))
            {
                continue;
            }

            const float Roll = RandomUnit();
            EEntityKind Kind = EEntityKind::Husk;

            if (Roll > 0.88f && Player.Level >= 3)
            {
                Kind = EEntityKind::Golem;
            }
            else if (Roll > 0.62f)
            {
                Kind = EEntityKind::Wisp;
            }

            SpawnEnemy(Kind, { X, Ground + 0.2f, Z });
            return;
        }
    }

    void FGame::UpdateCamera(float Delta)
    {
        const FVoxelPhysics Physics(*World);

        const FVector3 Eye = GetEyePosition();
        const FVector3 Look = GetLookDirection();

        const FVector3 Back { -Look.x, -Look.y, -Look.z };

        // Marched rather than raycast, so an eye that starts buried still resolves to a clear boom.
        constexpr float kWantedBoom = 4.6f;
        constexpr int32 kBoomSteps = 18;

        const FVector3 Probe { 0.16f, 0.16f, 0.16f };
        float Allowed = 0.0f;

        for (int32 Step = 1; Step <= kBoomSteps; ++Step)
        {
            const float Reach = kWantedBoom * float(Step) / float(kBoomSteps);
            const FVector3 Candidate
            {
                Eye.x + Back.x * Reach,
                Eye.y + Back.y * Reach + 0.30f * (Reach / kWantedBoom),
                Eye.z + Back.z * Reach,
            };

            if (Physics.OverlapsSolid(Candidate, Probe))
            {
                break;
            }
            Allowed = Reach;
        }

        // Pushing out slowly and pulling in at once keeps a wall from clipping the view for a frame.
        CameraBoom = Allowed < CameraBoom
            ? Allowed
            : Math::Lerp(CameraBoom, Allowed, 1.0f - Math::Exp(-7.0f * Delta));

        // Below this the view would sit inside the delver, so it drops to first person instead.
        bFirstPerson = CameraBoom < 1.5f;

        CameraPosition = bFirstPerson
            ? Eye
            : FVector3
              {
                  Eye.x + Back.x * CameraBoom,
                  Eye.y + Back.y * CameraBoom + 0.30f * (CameraBoom / kWantedBoom),
                  Eye.z + Back.z * CameraBoom,
              };
    }

    void FGame::DamagePlayer(float Amount)
    {
        if (!Player.bAlive || Amount <= 0.0f)
        {
            return;
        }

        Player.Health -= Amount;
        Player.HurtTimer = 0.45f;

        Floater(GetEyePosition(), Format("{}", int32(Amount)), { 2.2f, 0.35f, 0.25f });

        if (Player.Health <= 0.0f)
        {
            Player.Health = 0.0f;
            Player.bAlive = false;
            Player.DeathTimer = 0.0f;
            Say("You fall.", 3.0f);
        }
    }

    void FGame::DamageEntity(FEntity& Target, float Amount, const FVector3& From)
    {
        Target.Health -= Amount;
        Target.HurtFlash = 1.0f;

        const FVector3 Knock = Normalized({ Target.Body.Position.x - From.x, 0.0f,
                                            Target.Body.Position.z - From.z });
        Target.Body.Velocity.x += Knock.x * 5.2f;
        Target.Body.Velocity.z += Knock.z * 5.2f;

        Floater({ Target.Body.Position.x, Target.Body.Position.y + Target.Body.HalfExtent.y, Target.Body.Position.z },
                Format("{}", int32(Amount)), { 2.4f, 1.9f, 0.9f });

        if (Target.Health > 0.0f)
        {
            return;
        }

        Target.bActive = false;
        ++Player.Kills;

        const FVector3 Grave = Target.Body.Position;

        switch (Target.Kind)
        {
        case EEntityKind::Golem:
            SpawnShard(Grave, 3);
            SpawnDebris(Grave, { 0.0f, 4.0f, 0.0f }, uint8(EMaterial::Rock), 14);
            GrantExperience(120);
            break;

        case EEntityKind::Wisp:
            SpawnShard(Grave, 1);
            SpawnSpark(Grave, { 0.5f, 1.9f, 2.3f }, 14);
            GrantExperience(45);
            break;

        default:
            if (RandomUnit() < 0.55f)
            {
                SpawnShard(Grave, 1);
            }
            SpawnSpark(Grave, { 1.4f, 0.5f, 0.4f }, 10);
            GrantExperience(30);
            break;
        }

        RefreshObjectives();
    }

    void FGame::GrantExperience(int32 Amount)
    {
        Player.Experience += Amount;

        while (Player.Experience >= GetExperienceForNext())
        {
            Player.Experience -= GetExperienceForNext();
            ++Player.Level;

            Player.MaxHealth += 18.0f;
            Player.MaxStamina += 6.0f;
            Player.MaxFocus += 8.0f;
            Player.Health = Player.MaxHealth;
            Player.Focus = Player.MaxFocus;

            Say(FString("Level ") .append(Format("{}", Player.Level)).c_str(), 3.0f);
            SpawnSpark(Player.Body.Position, { 2.2f, 1.9f, 0.7f }, 22);
        }
    }

    int32 FGame::CountOf(EEntityKind Kind) const
    {
        int32 Total = 0;
        for (const FEntity& Entity : Entities)
        {
            if (Entity.bActive && Entity.Kind == Kind)
            {
                ++Total;
            }
        }
        return Total;
    }

    void FGame::Say(const char* Text, float Life)
    {
        Banner.Text = Text;
        Banner.Age = 0.0f;
        Banner.Life = Life;
    }

    void FGame::Floater(const FVector3& World, const FString& Text, const FVector3& Color)
    {
        if (Floaters.size() >= 48)
        {
            Floaters.erase(Floaters.begin());
        }

        FFloatingText Entry;
        Entry.World = World;
        Entry.Text = Text;
        Entry.Color = Color;
        Floaters.push_back(Move(Entry));
    }

    void FGame::RefreshObjectives()
    {
        Objectives.clear();

        FObjective Light;
        Light.Text = "Light the beacons";
        Light.Progress = BeaconsLit;
        Light.Target = Math::Max(TotalBeacons, 1);
        Light.bComplete = BeaconsLit >= TotalBeacons;
        Objectives.push_back(Move(Light));

        FObjective Collect;
        Collect.Text = "Carry crystal shards";
        Collect.Progress = Player.Shards;
        Collect.Target = 4;
        Collect.bComplete = Player.Shards >= 4;
        Objectives.push_back(Move(Collect));

        FObjective Cull;
        Cull.Text = "Clear the husks";
        Cull.Progress = Player.Kills;
        Cull.Target = 12;
        Cull.bComplete = Player.Kills >= 12;
        Objectives.push_back(Move(Cull));
    }
}
