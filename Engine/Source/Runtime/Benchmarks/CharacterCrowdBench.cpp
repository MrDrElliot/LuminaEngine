#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <thread>

#include "Containers/Algorithm.h"
#include "Containers/HashTable.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Physics/Physics.h"
#include "Physics/PhysicsScene.h"
#include "Physics/API/Box3D/Box3DCharacterHandle.h"
#include "Physics/API/Box3D/Box3DPhysicsScene.h"
#include "Platform/Process/PlatformProcess.h"
#include "Platform/Time/PlatformTime.h"
#include "TaskSystem/TaskSystem.h"
#include "World/World.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "World/Entity/Components/TerrainComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Subsystems/WorldSettings.h"

// Run with ./LuminaBuild.bat Test Runtime -Benchmarks -Filter="CharacterCrowdBench.*", where LUMINA_CROWD_COUNTS and LUMINA_CROWD_SCENARIOS narrow the sweep.

using namespace Lumina;

namespace
{
    enum class EScenario : uint8
    {
        Open,
        Dense,
        Terrain,
        Mixed,
    };

    constexpr const char* kScenarioNames[] = { "open", "dense", "terrain", "mixed" };

    enum class EActivity : uint8
    {
        Idle,
        Walk,
        Sprint,
        Jumper,
        Seek,
    };

    constexpr float kRadius      = 0.35f;
    constexpr float kHalfHeight  = 0.55f;
    constexpr float kWalkSpeed   = 4.0f;
    constexpr float kSprintSpeed = 7.0f;
    constexpr double kFrameDt    = 1.0 / 60.0;

    // Dense packs a capsule every meter, the rest one per four square meters.
    constexpr float kOpenSpacing  = 2.0f;
    constexpr float kDenseSpacing = 1.0f;
    constexpr int32 kDenseBlock   = 32;

    struct FRandom
    {
        uint64 State;

        explicit FRandom(uint64 Seed) : State(Seed * 0x9E3779B97F4A7C15ull + 1) {}

        uint32 Next()
        {
            State ^= State << 13;
            State ^= State >> 7;
            State ^= State << 17;
            return (uint32)(State >> 16);
        }

        float Unit() { return (float)(Next() & 0xFFFFFF) / (float)0xFFFFFF; }
        float Range(float Min, float Max) { return Min + (Max - Min) * Unit(); }
    };

    float Hills(float X, float Z)
    {
        return 4.0f * std::sin(X * 0.031f) * std::cos(Z * 0.027f) + 1.2f * std::sin(X * 0.11f + 1.3f) * std::sin(Z * 0.09f);
    }

    struct FAgent
    {
        ECS::FEntity Entity = ECS::NullEntity;
        EActivity    Activity = EActivity::Idle;
        float        Heading = 0.0f;
        int32        NextTurnFrame = 0;
        int32        NextJumpFrame = 0;
        FVector3     Goal = FVector3(0.0f);
        FVector3     MeasureStart = FVector3(0.0f);
    };

    struct FFrameSample
    {
        double FrameMs = 0.0;
        Physics::FPhysicsUpdateTimings Timings;
    };

    struct FCorrectness
    {
        int32 Grounded = 0;
        int32 Sunk = 0;
        int32 FellThrough = 0;
        int32 OverlappingPairs = 0;
        float WorstOverlap = 0.0f;
        int32 InsidePillars = 0;
        double SpeedRatio = 0.0;
        int32 SpeedSamples = 0;
        int32 ProxiesEnabled = 0;
        int32 FarCharacters = 0;
        int32 WorstA = -1;
        int32 WorstB = -1;
    };

    // One crowd on a fresh world, stepped the way an engine frame steps it.
    class FCrowdWorld
    {
    public:

        FCrowdWorld(EScenario InScenario, int32 InCount)
            : Scenario(InScenario), Count(InCount)
        {
            RegisterPendingTypes();
            World = NewObject<CWorld>(nullptr, FName("CharacterCrowdBenchWorld"), FGuid::New(), OF_Transient);
            WorldOwner = World;
            ECS::FRegistry& R = Registry();
            R.Ctx().Emplace<CWorld*>(World);
            R.GetSignals<STransformComponent>().OnConstruct.Connect<&FCrowdWorld::BindTransform>();
            if (Physics::GetPhysicsContext() == nullptr)
            {
                Physics::Initialize();
            }
            World->EnsurePhysicsScene();
            Scene = static_cast<Physics::FBox3DPhysicsScene*>(World->GetPhysicsScene());

            // LUMINA_CROWD_LOD in meters turns on character LOD around the crowd's center, where a player would stand.
            if (const char* Lod = std::getenv("LUMINA_CROWD_LOD"))
            {
                World->GetDefaultWorldSettings().CharacterLODDistance = (float)std::atof(Lod);
            }
            if (const char* Interval = std::getenv("LUMINA_CROWD_LOD_INTERVAL"))
            {
                World->GetDefaultWorldSettings().CharacterLODStepInterval = (uint32)std::atoi(Interval);
            }

            const float Spacing = Scenario == EScenario::Dense ? kDenseSpacing : kOpenSpacing;
            Side = Math::Max(64.0f, std::sqrt((float)Count) * Spacing);
        }

        ~FCrowdWorld()
        {
            WorldOwner.Reset();
        }

        FCrowdWorld(const FCrowdWorld&) = delete;
        FCrowdWorld& operator=(const FCrowdWorld&) = delete;

        ECS::FRegistry& Registry() const { return World->GetEntityRegistry(); }

        bool HasTerrain() const { return Scenario == EScenario::Terrain || Scenario == EScenario::Mixed; }

        float GroundHeight(float X, float Z) const { return HasTerrain() ? Hills(X, Z) : 0.0f; }

        void Build()
        {
            if (HasTerrain())
            {
                SpawnTerrain();
            }
            else
            {
                SpawnFloor();
            }
            if (Scenario == EScenario::Mixed)
            {
                SpawnPillars();
            }
            SpawnCharacters();
            if (Scenario == EScenario::Mixed)
            {
                SpawnCrates();
            }
        }

        void SetInputs(int32 Frame)
        {
            ECS::FRegistry& R = Registry();
            for (FAgent& Agent : Agents)
            {
                SCharacterControllerComponent& Controller = R.Get<SCharacterControllerComponent>(Agent.Entity);
                if (Agent.Activity == EActivity::Idle)
                {
                    continue;
                }

                const FVector3 Position = R.Get<STransformComponent>(Agent.Entity).GetLocalLocation();
                FVector3 Direction;
                if (Agent.Activity == EActivity::Seek)
                {
                    Direction = FVector3(Agent.Goal.x - Position.x, 0.0f, Agent.Goal.z - Position.z);
                    if (Math::LengthSquared(Direction) < 0.25f)
                    {
                        continue;
                    }
                    Direction = Math::Normalize(Direction);
                }
                else
                {
                    if (Frame >= Agent.NextTurnFrame)
                    {
                        FRandom Turn((uint64)Agent.Entity.Value * 31 + (uint64)Frame);
                        Agent.Heading += Turn.Range(-1.2f, 1.2f);
                        Agent.NextTurnFrame = Frame + 90 + (int32)(Turn.Next() % 150);
                    }
                    // Walkers near the edge head back in, so the crowd keeps its density.
                    const float Limit = Side * 0.45f;
                    if (Math::Abs(Position.x) > Limit || Math::Abs(Position.z) > Limit)
                    {
                        Agent.Heading = Math::Atan2(-Position.x, -Position.z);
                    }
                    Direction = FVector3(std::sin(Agent.Heading), 0.0f, std::cos(Agent.Heading));
                }

                Controller.AddWorldMovementInput(Direction);
                Controller.Buttons = Agent.Activity == EActivity::Sprint ? 1u : 0u;
                if (Agent.Activity == EActivity::Jumper && Frame >= Agent.NextJumpFrame)
                {
                    Controller.Jump();
                    Agent.NextJumpFrame = Frame + 100 + (int32)(Agent.Entity.Value % 60);
                }
            }
        }

        double Frame(double Dt)
        {
            ECS::FRegistry& R = Registry();
            PlatformTime::FStopwatch Watch;
            ECS::Utils::ResolveAllDirtyTransforms(R);
            Scene->Update(Dt);
            Scene->DispatchPendingEvents();
            ECS::Utils::ResolveAllDirtyTransforms(R);
            World->FlushCommands();
            World->GetEvents().Advance();
            return Watch.ElapsedMilliseconds();
        }

        void DescribeAgent(const char* Label, const FAgent& Agent) const
        {
            const SCharacterPhysicsComponent& Physics = Registry().Get<SCharacterPhysicsComponent>(Agent.Entity);
            const SCharacterMovementComponent& Movement = Registry().Get<SCharacterMovementComponent>(Agent.Entity);
            const FVector3 P = SimulatedPosition(Agent.Entity);
            std::printf("    %s entity %u at (%.2f %.2f %.2f) ground %.2f activity %d interval %d held %d resting %d grounded %d proxy %d vel (%.2f %.2f %.2f)\n",
                Label, Agent.Entity.Value, P.x, P.y, P.z, GroundHeight(P.x, P.z), (int)Agent.Activity,
                Physics.Character ? Physics.Character->LODInterval : -1, Physics.Character ? Physics.Character->LODStepsHeld : -1,
                Physics.Character ? (int)Physics.Character->bResting : -1, (int)Movement.bGrounded,
                Physics.Character ? (int)Physics.Character->bProxyEnabled : -1, Movement.Velocity.x, Movement.Velocity.y, Movement.Velocity.z);
        }

        // The simulated pose rather than the drawn one, which a far character's interpolation runs behind on purpose.
        FVector3 SimulatedPosition(ECS::FEntity Entity) const
        {
            const SCharacterPhysicsComponent& Physics = Registry().Get<SCharacterPhysicsComponent>(Entity);
            return Physics.Character ? Physics.Character->Position : Registry().Get<STransformComponent>(Entity).GetLocalLocation();
        }

        void BeginMeasure()
        {
            for (FAgent& Agent : Agents)
            {
                Agent.MeasureStart = SimulatedPosition(Agent.Entity);
            }
        }

        FCorrectness Check(double MeasuredSeconds) const
        {
            ECS::FRegistry& R = Registry();
            FCorrectness Out;
            for (const FAgent& Agent : Agents)
            {
                const SCharacterPhysicsComponent& Physics = R.Get<SCharacterPhysicsComponent>(Agent.Entity);
                if (Physics.Character)
                {
                    Out.ProxiesEnabled += Physics.Character->bProxyEnabled ? 1 : 0;
                    Out.FarCharacters += Physics.Character->LODInterval > 1 ? 1 : 0;
                }
            }

            TVector<FVector3> Positions;
            Positions.Reserve(Agents.Num());
            for (const FAgent& Agent : Agents)
            {
                const FVector3 P = SimulatedPosition(Agent.Entity);
                Positions.Add(P);
                const float Ground = GroundHeight(P.x, P.z);
                const float Rest = Ground + kHalfHeight + kRadius;
                Out.Grounded += R.Get<SCharacterMovementComponent>(Agent.Entity).bGrounded ? 1 : 0;
                Out.Sunk += P.y < Rest - 0.25f ? 1 : 0;
                Out.FellThrough += P.y < Ground - 1.0f ? 1 : 0;
                if (std::getenv("LUMINA_CROWD_DIAG") != nullptr && P.y < Rest - 0.25f)
                {
                    DescribeAgent("sunk", Agent);
                }

                if (Scenario == EScenario::Mixed && InsidePillar(P))
                {
                    ++Out.InsidePillars;
                }

                // Only free walkers at open density are expected to reach their commanded speed.
                if ((Agent.Activity == EActivity::Walk || Agent.Activity == EActivity::Sprint) && Scenario != EScenario::Dense)
                {
                    const float Commanded = Agent.Activity == EActivity::Sprint ? kSprintSpeed : kWalkSpeed;
                    const float Moved = Math::Length(FVector3(P.x - Agent.MeasureStart.x, 0.0f, P.z - Agent.MeasureStart.z));
                    Out.SpeedRatio += Moved / (Commanded * (float)MeasuredSeconds);
                    ++Out.SpeedSamples;
                }
            }
            if (Out.SpeedSamples > 0)
            {
                Out.SpeedRatio /= Out.SpeedSamples;
            }

            CountOverlaps(Positions, Out);
            if (std::getenv("LUMINA_CROWD_DIAG") != nullptr && Out.WorstA >= 0)
            {
                DescribeAgent("worst overlap A", Agents[Out.WorstA]);
                DescribeAgent("worst overlap B", Agents[Out.WorstB]);
            }
            return Out;
        }

        EScenario Scenario;
        int32     Count;
        float     Side = 0.0f;
        CWorld*   World = nullptr;
        Physics::FBox3DPhysicsScene* Scene = nullptr;
        TVector<FAgent> Agents;
        TVector<FVector3> Pillars;

    private:

        static void BindTransform(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            STransformComponent& Transform = Registry.Get<STransformComponent>(Entity);
            Transform.Bind(Registry, Entity);
            Transform.ResetDirtyState();
        }

        static constexpr float kPillarHalf = 0.6f;
        static constexpr float kPillarSpacing = 14.0f;

        bool InsidePillar(const FVector3& P) const
        {
            const float Cell = std::round(P.x / kPillarSpacing) * kPillarSpacing;
            const float CellZ = std::round(P.z / kPillarSpacing) * kPillarSpacing;
            const float OutX = Math::Max(Math::Abs(P.x - Cell) - kPillarHalf, 0.0f);
            const float OutZ = Math::Max(Math::Abs(P.z - CellZ) - kPillarHalf, 0.0f);
            const float Inset = kRadius - 0.05f;
            return OutX * OutX + OutZ * OutZ < Inset * Inset && IsPillarCell(Cell, CellZ);
        }

        bool IsPillarCell(float X, float Z) const
        {
            const float Limit = Side * 0.5f;
            return Math::Abs(X) < Limit && Math::Abs(Z) < Limit && !(X == 0.0f && Z == 0.0f);
        }

        void SpawnFloor()
        {
            ECS::FRegistry& R = Registry();
            const ECS::FEntity Floor = R.Create();
            R.Emplace<STransformComponent>(Floor).SetLocalLocation(FVector3(0.0f, -1.0f, 0.0f));
            R.Emplace<SRigidBodyComponent>(Floor).BodyType = EBodyType::Static;
            R.Emplace<SBoxColliderComponent>(Floor).HalfExtent = FVector3(Side, 1.0f, Side);
        }

        void SpawnTerrain()
        {
            ECS::FRegistry& R = Registry();
            const ECS::FEntity Entity = R.Create();
            R.Emplace<STransformComponent>(Entity).SetLocalLocation(FVector3(0.0f));

            constexpr float kMaxHeight = 16.0f;
            const float TileWorldSize = Side + 32.0f;
            const int32 Resolution = Math::Clamp((int32)(TileWorldSize / 0.75f) | 1, 129, 2049);

            STerrainComponent Terrain;
            Terrain.Resolution = Resolution;
            Terrain.TileWorldSize = TileWorldSize;
            Terrain.MaxHeight = kMaxHeight;
            Terrain.Heightmap.SetNum(Resolution * Resolution);
            const float Stride = TileWorldSize / float(Resolution - 1);
            for (int32 Z = 0; Z < Resolution; ++Z)
            {
                for (int32 X = 0; X < Resolution; ++X)
                {
                    // Offset so the hills sit inside the height range, which starts at the terrain origin.
                    const float H = Hills(-TileWorldSize * 0.5f + X * Stride, -TileWorldSize * 0.5f + Z * Stride);
                    Terrain.Heightmap[Z * Resolution + X] = (H + kMaxHeight * 0.5f) / kMaxHeight;
                }
            }
            R.Get<STransformComponent>(Entity).SetLocalLocation(FVector3(0.0f, -kMaxHeight * 0.5f, 0.0f));
            R.Emplace<STerrainComponent>(Entity, std::move(Terrain));
            R.Emplace<STerrainColliderComponent>(Entity);
        }

        void SpawnPillars()
        {
            ECS::FRegistry& R = Registry();
            const int32 Half = (int32)(Side * 0.5f / kPillarSpacing);
            for (int32 Z = -Half; Z <= Half; ++Z)
            {
                for (int32 X = -Half; X <= Half; ++X)
                {
                    const float PX = (float)X * kPillarSpacing;
                    const float PZ = (float)Z * kPillarSpacing;
                    if (!IsPillarCell(PX, PZ))
                    {
                        continue;
                    }
                    const ECS::FEntity Pillar = R.Create();
                    R.Emplace<STransformComponent>(Pillar).SetLocalLocation(FVector3(PX, GroundHeight(PX, PZ) + 1.0f, PZ));
                    R.Emplace<SRigidBodyComponent>(Pillar).BodyType = EBodyType::Static;
                    R.Emplace<SBoxColliderComponent>(Pillar).HalfExtent = FVector3(kPillarHalf, 2.5f, kPillarHalf);
                    Pillars.Add(FVector3(PX, 0.0f, PZ));
                }
            }
        }

        // One crate per two hundred characters, dropped among them so walkers shove them around.
        void SpawnCrates()
        {
            ECS::FRegistry& R = Registry();
            FRandom Random(99);
            const int32 Crates = Math::Max(1, Count / 200);
            for (int32 i = 0; i < Crates; ++i)
            {
                const float X = Random.Range(-Side * 0.45f, Side * 0.45f);
                const float Z = Random.Range(-Side * 0.45f, Side * 0.45f);
                const ECS::FEntity Crate = R.Create();
                R.Emplace<STransformComponent>(Crate).SetLocalLocation(FVector3(X, GroundHeight(X, Z) + 2.5f, Z));
                R.Emplace<SRigidBodyComponent>(Crate).BodyType = EBodyType::Dynamic;
                R.Emplace<SBoxColliderComponent>(Crate).HalfExtent = FVector3(0.4f);
            }
        }

        void SpawnCharacters()
        {
            ECS::FRegistry& R = Registry();
            FRandom Random(7);
            const float Spacing = Scenario == EScenario::Dense ? kDenseSpacing : kOpenSpacing;
            const int32 PerRow = Math::Max(1, (int32)std::ceil(std::sqrt((float)Count)));
            const float Origin = -(float)(PerRow - 1) * Spacing * 0.5f;

            Agents.Reserve(Count);
            const char* IdleEnv = std::getenv("LUMINA_CROWD_IDLE");
            const int32 IdlePercent = IdleEnv != nullptr ? std::atoi(IdleEnv) : -1;
            for (int32 i = 0; i < Count; ++i)
            {
                const int32 Col = i % PerRow;
                const int32 Row = i / PerRow;
                const float Jitter = Spacing * 0.15f;
                float X = Origin + (float)Col * Spacing + Random.Range(-Jitter, Jitter);
                float Z = Origin + (float)Row * Spacing + Random.Range(-Jitter, Jitter);
                if (Scenario == EScenario::Mixed)
                {
                    // Clear of the pillar footprints, so nothing spawns inside geometry.
                    const float CellX = std::round(X / kPillarSpacing) * kPillarSpacing;
                    const float CellZ = std::round(Z / kPillarSpacing) * kPillarSpacing;
                    if (Math::Abs(X - CellX) < kPillarHalf + kRadius + 0.1f && Math::Abs(Z - CellZ) < kPillarHalf + kRadius + 0.1f)
                    {
                        X += kPillarHalf * 2.0f + kRadius * 2.0f;
                    }
                }

                const ECS::FEntity Entity = R.Create();
                R.Emplace<STransformComponent>(Entity).SetLocalLocation(FVector3(X, GroundHeight(X, Z) + kHalfHeight + kRadius + 0.05f, Z));
                SCharacterPhysicsComponent& Physics = R.Emplace<SCharacterPhysicsComponent>(Entity);
                Physics.Radius = kRadius;
                Physics.HalfHeight = kHalfHeight;
                SCharacterMovementComponent& Movement = R.Emplace<SCharacterMovementComponent>(Entity);
                Movement.MoveSpeed = kWalkSpeed;
                Movement.SprintSpeed = kSprintSpeed;
                Movement.bOrientRotationToMovement = true;
                R.Emplace<SCharacterControllerComponent>(Entity);

                FAgent& Agent = Agents.EmplaceGetRef();
                Agent.Entity = Entity;
                Agent.Heading = Random.Range(0.0f, 6.2831853f);
                Agent.NextTurnFrame = (int32)(Random.Next() % 120);
                Agent.NextJumpFrame = (int32)(Random.Next() % 120);

                if (Col == PerRow / 2 && Row == PerRow / 2)
                {
                    Physics.bLODRelevanceSource = true;
                }
                if (Scenario == EScenario::Dense)
                {
                    // Each block of the grid crushes in on its own center.
                    const int32 BlockX = Col / kDenseBlock;
                    const int32 BlockZ = Row / kDenseBlock;
                    const float Center = (float)kDenseBlock * Spacing * 0.5f;
                    Agent.Activity = EActivity::Seek;
                    Agent.Goal = FVector3(Origin + (float)BlockX * kDenseBlock * Spacing + Center, 0.0f,
                                          Origin + (float)BlockZ * kDenseBlock * Spacing + Center);
                    continue;
                }

                // LUMINA_CROWD_IDLE as a percentage overrides the mix, so a still crowd can be measured on its own.
                const uint32 Roll = Random.Next() % 100;
                if (IdlePercent >= 0)
                {
                    Agent.Activity = (int32)Roll < IdlePercent ? EActivity::Idle : EActivity::Walk;
                    continue;
                }
                Agent.Activity = Roll < 20 ? EActivity::Idle
                               : Roll < 85 ? EActivity::Walk
                               : Roll < 95 ? EActivity::Sprint
                               : EActivity::Jumper;
            }
        }

        // Capsules sharing floor space by more than a couple of centimeters, found through a hashed grid of meter cells.
        void CountOverlaps(const TVector<FVector3>& Positions, FCorrectness& Out) const
        {
            constexpr float kCell = 1.0f;
            constexpr float kTolerance = 0.03f;
            const int32 Cells = Math::Max(1, (int32)std::ceil(Side * 1.5f / kCell));
            const float Offset = Side * 0.75f;

            THashMap<int64, TVector<int32>> Grid;
            const auto Key = [&](int32 X, int32 Z) { return (int64)Z * Cells + X; };
            const auto CellOf = [&](float V) { return Math::Clamp((int32)((V + Offset) / kCell), 0, Cells - 1); };
            for (int32 i = 0; i < Positions.Num(); ++i)
            {
                Grid[Key(CellOf(Positions[i].x), CellOf(Positions[i].z))].Add(i);
            }

            const float Contact = 2.0f * kRadius - kTolerance;
            const float Tall = 2.0f * (kHalfHeight + kRadius);
            for (int32 i = 0; i < Positions.Num(); ++i)
            {
                const FVector3& A = Positions[i];
                const int32 CX = CellOf(A.x);
                const int32 CZ = CellOf(A.z);
                for (int32 DZ = -1; DZ <= 1; ++DZ)
                {
                    for (int32 DX = -1; DX <= 1; ++DX)
                    {
                        const TVector<int32>* Found = Grid.Find(Key(CX + DX, CZ + DZ));
                        if (Found == nullptr)
                        {
                            continue;
                        }
                        for (int32 j : *Found)
                        {
                            if (j <= i)
                            {
                                continue;
                            }
                            const FVector3& B = Positions[j];
                            if (Math::Abs(A.y - B.y) > Tall)
                            {
                                continue;
                            }
                            // Upright capsules of one size are nearest along the vertical gap past their segments.
                            const float Gap = Math::Max(Math::Abs(A.y - B.y) - 2.0f * kHalfHeight, 0.0f);
                            const float D = Math::Length(FVector3(A.x - B.x, Gap, A.z - B.z));
                            if (D < Contact)
                            {
                                ++Out.OverlappingPairs;
                                if (2.0f * kRadius - D > Out.WorstOverlap)
                                {
                                    Out.WorstOverlap = 2.0f * kRadius - D;
                                    Out.WorstA = i;
                                    Out.WorstB = j;
                                }
                            }
                        }
                    }
                }
            }
        }

        TStrongObjectPtr<CWorld> WorldOwner;
    };

    TVector<int32> ParseCounts()
    {
        TVector<int32> Counts;
        const char* Env = std::getenv("LUMINA_CROWD_COUNTS");
        const char* Text = Env != nullptr ? Env : "1000,10000,50000,100000";
        while (*Text != '\0')
        {
            const int32 Value = std::atoi(Text);
            if (Value > 0)
            {
                Counts.Add(Value);
            }
            while (*Text != '\0' && *Text != ',') { ++Text; }
            if (*Text == ',') { ++Text; }
        }
        return Counts;
    }

    bool ScenarioEnabled(EScenario Scenario)
    {
        const char* Env = std::getenv("LUMINA_CROWD_SCENARIOS");
        return Env == nullptr || std::strstr(Env, kScenarioNames[(int32)Scenario]) != nullptr;
    }

    int32 EnvInt(const char* Name, int32 Default)
    {
        const char* Env = std::getenv(Name);
        return Env != nullptr ? std::atoi(Env) : Default;
    }

    double Percentile(TVector<double> Values, double Fraction)
    {
        if (Values.IsEmpty())
        {
            return 0.0;
        }
        Algo::Sort(Values);
        const int32 Index = Math::Clamp((int32)(Fraction * (double)(Values.Num() - 1) + 0.5), 0, Values.Num() - 1);
        return Values[Index];
    }

    void RunScenario(EScenario Scenario, int32 Count)
    {
        const int32 WarmFrames = EnvInt("LUMINA_CROWD_WARM", 90);
        const int32 Frames = EnvInt("LUMINA_CROWD_FRAMES", 120);
        // A real frame feeds its own duration back, which is how a slow step turns into several the next frame.
        const bool bRealTime = EnvInt("LUMINA_CROWD_REALTIME", 0) != 0;

        const size_t MemoryBefore = Platform::GetProcessMemoryUsageBytes();

        FCrowdWorld Crowd(Scenario, Count);
        PlatformTime::FStopwatch BuildWatch;
        Crowd.Build();
        const double SpawnMs = BuildWatch.ElapsedMilliseconds();
        BuildWatch.Restart();
        const double FirstFrameMs = Crowd.Frame(kFrameDt);

        double Dt = kFrameDt;
        for (int32 Frame = 0; Frame < WarmFrames; ++Frame)
        {
            Crowd.SetInputs(Frame);
            const double Ms = Crowd.Frame(Dt);
            if (const char* Trace = std::getenv("LUMINA_CROWD_TRACE"))
            {
                for (const FAgent& Agent : Crowd.Agents)
                {
                    if (Agent.Entity.Value == (uint32)std::atoi(Trace))
                    {
                        Crowd.DescribeAgent("trace", Agent);
                    }
                }
            }
            Dt = bRealTime ? Math::Max(kFrameDt, Ms * 0.001) : kFrameDt;
        }

        const size_t MemoryAfter = Platform::GetProcessMemoryUsageBytes();
        const b3Counters Counters = Crowd.Scene->GetWorldCounters();

        Crowd.BeginMeasure();
        TVector<FFrameSample> Samples;
        Samples.Reserve(Frames);
        TVector<double> FrameTimes;
        double InputMs = 0.0;
        const double CPUStart = Platform::GetProcessCPUSeconds();
        PlatformTime::FStopwatch Wall;
        double SimulatedSeconds = 0.0;
        for (int32 Frame = 0; Frame < Frames; ++Frame)
        {
            PlatformTime::FStopwatch InputWatch;
            Crowd.SetInputs(WarmFrames + Frame);
            InputMs += InputWatch.ElapsedMilliseconds();

            FFrameSample& Sample = Samples.EmplaceGetRef();
            Sample.FrameMs = Crowd.Frame(Dt);
            if (const char* Trace = std::getenv("LUMINA_CROWD_TRACE"))
            {
                for (const FAgent& Agent : Crowd.Agents)
                {
                    if (Agent.Entity.Value == (uint32)std::atoi(Trace))
                    {
                        Crowd.DescribeAgent("trace", Agent);
                    }
                }
            }
            Sample.Timings = Crowd.Scene->GetLastUpdateTimings();
            FrameTimes.Add(Sample.FrameMs);
            SimulatedSeconds += (double)Sample.Timings.Steps / 60.0;
            Dt = bRealTime ? Math::Max(kFrameDt, Sample.FrameMs * 0.001) : kFrameDt;
        }
        const double WallSeconds = Wall.ElapsedSeconds();
        const double CPUSeconds = Platform::GetProcessCPUSeconds() - CPUStart;

        const FCorrectness Correct = Crowd.Check(Math::Max(SimulatedSeconds, 1e-3));

        Physics::FPhysicsUpdateTimings Sum;
        double FrameSum = 0.0;
        // Every field of the profile is a float in milliseconds, summed in declaration order.
        constexpr int32 kProfileFields = (int32)(sizeof(b3Profile) / sizeof(float));
        double ProfileSum[kProfileFields] = {};
        for (const FFrameSample& Sample : Samples)
        {
            FrameSum += Sample.FrameMs;
            Sum.SyncMs += Sample.Timings.SyncMs;
            Sum.InterpMs += Sample.Timings.InterpMs;
            Sum.LatchMs += Sample.Timings.LatchMs;
            Sum.WorldStepMs += Sample.Timings.WorldStepMs;
            Sum.DrainMs += Sample.Timings.DrainMs;
            Sum.CharacterPrepMs += Sample.Timings.CharacterPrepMs;
            Sum.CharacterStepMs += Sample.Timings.CharacterStepMs;
            Sum.CharacterContactMs += Sample.Timings.CharacterContactMs;
            Sum.CharacterApplyMs += Sample.Timings.CharacterApplyMs;
            Sum.Steps += Sample.Timings.Steps;
            Sum.SteppedCharacters += Sample.Timings.SteppedCharacters;
            Sum.RestingCharacters += Sample.Timings.RestingCharacters;
            float Fields[kProfileFields];
            std::memcpy(Fields, &Sample.Timings.LastStepProfile, sizeof(Fields));
            for (int32 Field = 0; Field < kProfileFields; ++Field)
            {
                ProfileSum[Field] += Fields[Field];
            }
        }
        const double N = (double)Samples.Num();
        const double Steps = Math::Max((double)Sum.Steps, 1.0);
        const uint32 Threads = Math::Max(std::thread::hardware_concurrency(), 1u);
        const double Other = FrameSum - Sum.SyncMs - Sum.InterpMs - Sum.LatchMs - Sum.WorldStepMs - Sum.DrainMs
                           - Sum.CharacterPrepMs - Sum.CharacterStepMs - Sum.CharacterContactMs - Sum.CharacterApplyMs;

        std::printf("\n[crowd %s %d] spawn %.1f ms, first frame %.1f ms, %d threads%s\n",
            kScenarioNames[(int32)Scenario], Count, SpawnMs, FirstFrameMs, Threads, bRealTime ? ", real-time dt" : "");
        std::printf("  frame   mean %.2f  p50 %.2f  p95 %.2f  max %.2f ms, %.2f steps/frame, inputs %.2f ms/frame (not in frame)\n",
            FrameSum / N, Percentile(FrameTimes, 0.5), Percentile(FrameTimes, 0.95), Percentile(FrameTimes, 1.0), Steps / N, InputMs / N);
        std::printf("  per step  world %.2f  char prep %.2f  char step %.2f  contacts %.2f  char apply %.2f  drain %.2f  sync %.2f  latch %.2f ms\n",
            Sum.WorldStepMs / Steps, Sum.CharacterPrepMs / Steps, Sum.CharacterStepMs / Steps, Sum.CharacterContactMs / Steps, Sum.CharacterApplyMs / Steps,
            Sum.DrainMs / Steps, Sum.SyncMs / Steps, Sum.LatchMs / Steps);
        std::printf("  per frame interp %.2f  dispatch+resolve+other %.2f ms\n", Sum.InterpMs / N, Other / N);
        constexpr const char* kProfileNames[] = { "step", "pairs", "collide", "solve", "solverSetup", "constraints", "prepare",
            "integrateVel", "warmStart", "solveImpulses", "integratePos", "relax", "restitution", "store", "splitIslands",
            "transforms", "sensorHits", "jointEvents", "hitEvents", "refit", "bullets", "sleepIslands", "sensors" };
        static_assert(std::size(kProfileNames) == kProfileFields);
        std::printf("  box3d    ");
        for (int32 Field = 0; Field < kProfileFields; ++Field)
        {
            if (ProfileSum[Field] / N >= 0.01)
            {
                std::printf(" %s %.2f", kProfileNames[Field], ProfileSum[Field] / N);
            }
        }
        std::printf(" ms, last step of each frame\n");
        std::printf("  stepped %.0f  resting %.0f per step, %.2f us/stepped character\n",
            Sum.SteppedCharacters / Steps, Sum.RestingCharacters / Steps,
            Sum.SteppedCharacters > 0 ? Sum.CharacterStepMs * 1000.0 / Sum.SteppedCharacters : 0.0);
        std::printf("  cpu       %.1f ms cpu per frame, %.1f of %u threads busy (%.0f%%)\n",
            CPUSeconds * 1000.0 / N, CPUSeconds / WallSeconds, Threads, 100.0 * CPUSeconds / (WallSeconds * Threads));
        std::printf("  memory    %.1f MB process delta (%.0f bytes/character), box3d %.1f MB, %d bodies, %d contacts\n",
            (double)(MemoryAfter - Math::Min(MemoryBefore, MemoryAfter)) / (1024.0 * 1024.0),
            (double)(MemoryAfter - Math::Min(MemoryBefore, MemoryAfter)) / Count,
            (double)Counters.byteCount / (1024.0 * 1024.0), Counters.bodyCount, Counters.contactCount);
        std::printf("  lod       %d proxies on, %d characters past the LOD distance\n", Correct.ProxiesEnabled, Correct.FarCharacters);
        std::printf("  correct   grounded %.1f%%  sunk %d  fell %d  overlaps %d (worst %.3f m)  in pillars %d  speed %.2f of commanded\n",
            100.0 * Correct.Grounded / Count, Correct.Sunk, Correct.FellThrough, Correct.OverlappingPairs, Correct.WorstOverlap,
            Correct.InsidePillars, Correct.SpeedRatio);
        std::fflush(stdout);
    }
}

TEST(CharacterCrowdBench, Sweep)
{
    for (int32 Count : ParseCounts())
    {
        for (int32 Scenario = 0; Scenario < (int32)std::size(kScenarioNames); ++Scenario)
        {
            if (ScenarioEnabled((EScenario)Scenario))
            {
                RunScenario((EScenario)Scenario, Count);
            }
        }
    }
}
