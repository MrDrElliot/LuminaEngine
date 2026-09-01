#include "Swarm.h"

#include "Memory/Memcpy.h"
#include "TaskSystem/TaskSystem.h"

namespace Umbral
{
    namespace
    {
        constexpr float kSeparationPush = 900.0f;
        constexpr int32 kMinParallelRange = 2048;

        uint32 PackAgent(EAgentKind Kind, float HealthFraction, float Flash, uint32 Seed)
        {
            const uint32 KindBits   = uint32(Kind) & 0xFFu;
            const uint32 HealthBits = uint32(Math::Clamp(HealthFraction, 0.0f, 1.0f) * 255.0f) & 0xFFu;
            const uint32 FlashBits  = uint32(Math::Clamp(Flash, 0.0f, 1.0f) * 255.0f) & 0xFFu;
            return KindBits | (HealthBits << 8) | (FlashBits << 16) | ((Seed & 0xFFu) << 24);
        }
    }

    void FSwarm::Initialize()
    {
        PositionX.resize(kMaxAgents);
        PositionY.resize(kMaxAgents);
        VelocityX.resize(kMaxAgents);
        VelocityY.resize(kMaxAgents);
        Health.resize(kMaxAgents);
        MaxHealth.resize(kMaxAgents);
        Flash.resize(kMaxAgents);
        Kind.resize(kMaxAgents);
        Dead.resize(kMaxAgents);
        AgentCell.resize(kMaxAgents);
        CellAgents.resize(kMaxAgents);

        CellStart.resize(kGridCells + 1);
        ChunkCounts.resize(size_t(kGridChunks) * size_t(kGridCells));

        Count = 0;
        DeadCount = 0;
    }

    void FSwarm::Reset()
    {
        Count = 0;
        DeadCount = 0;
    }

    int32 FSwarm::CellOf(float X, float Y) const
    {
        const int32 CellX = Math::Clamp(int32(X / kGridCell), 0, kGridSide - 1);
        const int32 CellY = Math::Clamp(int32(Y / kGridCell), 0, kGridSide - 1);
        return CellY * kGridSide + CellX;
    }

    void FSwarm::Spawn(EAgentKind InKind, const FVector2& Position, FRandom& Rng)
    {
        if (Count >= kMaxAgents)
        {
            return;
        }

        const FAgentStats Stats = StatsFor(InKind);
        const int32 Index = Count++;

        PositionX[Index] = Position.x;
        PositionY[Index] = Position.y;
        VelocityX[Index] = Rng.Range(-40.0f, 40.0f);
        VelocityY[Index] = Rng.Range(-40.0f, 40.0f);
        Health[Index]    = Stats.Health;
        MaxHealth[Index] = Stats.Health;
        Flash[Index]     = 0.0f;
        Kind[Index]      = uint8(InKind);
        Dead[Index]      = 0;
    }

    void FSwarm::BuildGrid()
    {
        if (Count == 0)
        {
            for (int32 Cell = 0; Cell <= kGridCells; ++Cell)
            {
                CellStart[Cell] = 0;
            }
            return;
        }

        const int32 PerChunk = (Count + kGridChunks - 1) / kGridChunks;

        Task::ParallelFor(uint32(Count), [this](const Task::FParallelRange& Range)
        {
            for (uint32 Index = Range.Start; Index < Range.End; ++Index)
            {
                AgentCell[Index] = CellOf(PositionX[Index], PositionY[Index]);
            }
        }, kMinParallelRange);

        Memory::Memzero(ChunkCounts.data(), ChunkCounts.size() * sizeof(int32));

        Task::ParallelFor(uint32(kGridChunks), [this, PerChunk](const Task::FParallelRange& Range)
        {
            for (uint32 Chunk = Range.Start; Chunk < Range.End; ++Chunk)
            {
                int32* Counts = ChunkCounts.data() + size_t(Chunk) * size_t(kGridCells);
                const int32 First = int32(Chunk) * PerChunk;
                const int32 Last = Math::Min(First + PerChunk, Count);

                for (int32 Index = First; Index < Last; ++Index)
                {
                    ++Counts[AgentCell[Index]];
                }
            }
        }, 1);

        int32 Running = 0;
        for (int32 Cell = 0; Cell < kGridCells; ++Cell)
        {
            CellStart[Cell] = Running;
            for (int32 Chunk = 0; Chunk < kGridChunks; ++Chunk)
            {
                int32& Slot = ChunkCounts[size_t(Chunk) * size_t(kGridCells) + size_t(Cell)];
                const int32 Amount = Slot;
                Slot = Running;
                Running += Amount;
            }
        }
        CellStart[kGridCells] = Running;

        Task::ParallelFor(uint32(kGridChunks), [this, PerChunk](const Task::FParallelRange& Range)
        {
            for (uint32 Chunk = Range.Start; Chunk < Range.End; ++Chunk)
            {
                int32* Offsets = ChunkCounts.data() + size_t(Chunk) * size_t(kGridCells);
                const int32 First = int32(Chunk) * PerChunk;
                const int32 Last = Math::Min(First + PerChunk, Count);

                for (int32 Index = First; Index < Last; ++Index)
                {
                    CellAgents[Offsets[AgentCell[Index]]++] = Index;
                }
            }
        }, 1);
    }

    void FSwarm::Advance(float Delta, const FVector2& Target)
    {
        if (Count == 0)
        {
            return;
        }

        Task::ParallelFor(uint32(Count), [this, Delta, Target](const Task::FParallelRange& Range)
        {
            for (uint32 Index = Range.Start; Index < Range.End; ++Index)
            {
                if (Dead[Index] != 0)
                {
                    continue;
                }

                const FAgentStats Stats = StatsFor(EAgentKind(Kind[Index]));

                float X = PositionX[Index];
                float Y = PositionY[Index];

                float ToX = Target.x - X;
                float ToY = Target.y - Y;
                const float Distance = Math::Sqrt(ToX * ToX + ToY * ToY) + 0.0001f;
                ToX /= Distance;
                ToY /= Distance;

                // The neighbor counts double as a density field, so crowds spread without pairwise tests.
                const int32 CellX = Math::Clamp(int32(X / kGridCell), 1, kGridSide - 2);
                const int32 CellY = Math::Clamp(int32(Y / kGridCell), 1, kGridSide - 2);
                const int32 Center = CellY * kGridSide + CellX;

                const int32 Left  = CellStart[Center]     - CellStart[Center - 1];
                const int32 Right = CellStart[Center + 2] - CellStart[Center + 1];
                const int32 Up    = CellStart[Center - kGridSide + 1] - CellStart[Center - kGridSide];
                const int32 Down  = CellStart[Center + kGridSide + 1] - CellStart[Center + kGridSide];

                const float GradX = float(Left - Right);
                const float GradY = float(Up - Down);
                const float GradLength = Math::Sqrt(GradX * GradX + GradY * GradY) + 0.0001f;

                const float Crowd = Math::Min(float(CellStart[Center + 1] - CellStart[Center]) / 24.0f, 3.0f);
                const float PushX = GradX / GradLength * Crowd * kSeparationPush;
                const float PushY = GradY / GradLength * Crowd * kSeparationPush;

                float VX = VelocityX[Index];
                float VY = VelocityY[Index];

                VX += (ToX * Stats.Speed - VX) * Math::Min(1.0f, Delta * 3.0f) + PushX * Delta;
                VY += (ToY * Stats.Speed - VY) * Math::Min(1.0f, Delta * 3.0f) + PushY * Delta;

                const float Speed = Math::Sqrt(VX * VX + VY * VY);
                const float Limit = Stats.Speed * 1.9f;
                if (Speed > Limit)
                {
                    VX = VX / Speed * Limit;
                    VY = VY / Speed * Limit;
                }

                X += VX * Delta;
                Y += VY * Delta;

                PositionX[Index] = Math::Clamp(X, 8.0f, kWorldSize - 8.0f);
                PositionY[Index] = Math::Clamp(Y, 8.0f, kWorldSize - 8.0f);
                VelocityX[Index] = VX;
                VelocityY[Index] = VY;
                Flash[Index] = Math::Max(0.0f, Flash[Index] - Delta * 5.0f);
            }
        }, kMinParallelRange);
    }

    float FSwarm::ApplyDamage(TSpan<const FDamageVolume> Volumes, TVector<FVector2>& OutDeaths,
                              TVector<FVector4>& OutColors, int64& OutKills)
    {
        float Souls = 0.0f;

        for (const FDamageVolume& Volume : Volumes)
        {
            const float RadiusSquared = Volume.Radius * Volume.Radius;

            const int32 MinX = Math::Clamp(int32((Volume.Center.x - Volume.Radius) / kGridCell), 0, kGridSide - 1);
            const int32 MaxX = Math::Clamp(int32((Volume.Center.x + Volume.Radius) / kGridCell), 0, kGridSide - 1);
            const int32 MinY = Math::Clamp(int32((Volume.Center.y - Volume.Radius) / kGridCell), 0, kGridSide - 1);
            const int32 MaxY = Math::Clamp(int32((Volume.Center.y + Volume.Radius) / kGridCell), 0, kGridSide - 1);

            for (int32 CellY = MinY; CellY <= MaxY; ++CellY)
            {
                for (int32 CellX = MinX; CellX <= MaxX; ++CellX)
                {
                    const int32 Cell = CellY * kGridSide + CellX;
                    for (int32 Slot = CellStart[Cell]; Slot < CellStart[Cell + 1]; ++Slot)
                    {
                        const int32 Index = CellAgents[Slot];
                        if (Dead[Index] != 0)
                        {
                            continue;
                        }

                        const float DX = PositionX[Index] - Volume.Center.x;
                        const float DY = PositionY[Index] - Volume.Center.y;
                        if (DX * DX + DY * DY > RadiusSquared)
                        {
                            continue;
                        }

                        Health[Index] -= Volume.Damage;
                        Flash[Index] = 1.0f;

                        if (Volume.Knock != 0.0f || Volume.Pull != 0.0f)
                        {
                            const float Length = Math::Sqrt(DX * DX + DY * DY) + 0.0001f;
                            const float Push = Volume.Knock - Volume.Pull;
                            VelocityX[Index] += DX / Length * Push;
                            VelocityY[Index] += DY / Length * Push;
                        }

                        if (Volume.Slow > 0.0f)
                        {
                            const float Keep = Math::Max(0.0f, 1.0f - Volume.Slow);
                            VelocityX[Index] *= Keep;
                            VelocityY[Index] *= Keep;
                        }

                        if (Health[Index] <= 0.0f)
                        {
                            Dead[Index] = 1;
                            ++DeadCount;
                            ++OutKills;
                            Souls += StatsFor(EAgentKind(Kind[Index])).Souls;

                            if (OutDeaths.size() < 96)
                            {
                                OutDeaths.push_back({ PositionX[Index], PositionY[Index] });
                                OutColors.push_back(ColorFor(EAgentKind(Kind[Index])));
                            }
                        }
                    }
                }
            }
        }

        return Souls;
    }

    void FSwarm::Compact()
    {
        if (DeadCount == 0 || DeadCount * 12 < Count)
        {
            return;
        }

        int32 Write = 0;
        for (int32 Read = 0; Read < Count; ++Read)
        {
            if (Dead[Read] != 0)
            {
                continue;
            }

            if (Write != Read)
            {
                PositionX[Write] = PositionX[Read];
                PositionY[Write] = PositionY[Read];
                VelocityX[Write] = VelocityX[Read];
                VelocityY[Write] = VelocityY[Read];
                Health[Write]    = Health[Read];
                MaxHealth[Write] = MaxHealth[Read];
                Flash[Write]     = Flash[Read];
                Kind[Write]      = Kind[Read];
                Dead[Write]      = 0;
            }
            ++Write;
        }
        Count = Write;
        DeadCount = 0;
    }

    float FSwarm::SampleContact(const FVector2& Position, float Radius, FVector2& OutPush) const
    {
        float Damage = 0.0f;
        OutPush = { 0.0f, 0.0f };

        const int32 MinX = Math::Clamp(int32((Position.x - Radius) / kGridCell), 0, kGridSide - 1);
        const int32 MaxX = Math::Clamp(int32((Position.x + Radius) / kGridCell), 0, kGridSide - 1);
        const int32 MinY = Math::Clamp(int32((Position.y - Radius) / kGridCell), 0, kGridSide - 1);
        const int32 MaxY = Math::Clamp(int32((Position.y + Radius) / kGridCell), 0, kGridSide - 1);
        const float RadiusSquared = Radius * Radius;

        for (int32 CellY = MinY; CellY <= MaxY; ++CellY)
        {
            for (int32 CellX = MinX; CellX <= MaxX; ++CellX)
            {
                const int32 Cell = CellY * kGridSide + CellX;
                for (int32 Slot = CellStart[Cell]; Slot < CellStart[Cell + 1]; ++Slot)
                {
                    const int32 Index = CellAgents[Slot];
                    if (Dead[Index] != 0)
                    {
                        continue;
                    }

                    const float DX = PositionX[Index] - Position.x;
                    const float DY = PositionY[Index] - Position.y;
                    if (DX * DX + DY * DY > RadiusSquared)
                    {
                        continue;
                    }

                    Damage += StatsFor(EAgentKind(Kind[Index])).Damage;
                    OutPush.x -= DX;
                    OutPush.y -= DY;
                }
            }
        }

        return Damage;
    }

    int32 FSwarm::CountNear(const FVector2& Position, float Radius) const
    {
        int32 Total = 0;

        const int32 MinX = Math::Clamp(int32((Position.x - Radius) / kGridCell), 0, kGridSide - 1);
        const int32 MaxX = Math::Clamp(int32((Position.x + Radius) / kGridCell), 0, kGridSide - 1);
        const int32 MinY = Math::Clamp(int32((Position.y - Radius) / kGridCell), 0, kGridSide - 1);
        const int32 MaxY = Math::Clamp(int32((Position.y + Radius) / kGridCell), 0, kGridSide - 1);

        for (int32 CellY = MinY; CellY <= MaxY; ++CellY)
        {
            for (int32 CellX = MinX; CellX <= MaxX; ++CellX)
            {
                const int32 Cell = CellY * kGridSide + CellX;
                Total += CellStart[Cell + 1] - CellStart[Cell];
            }
        }

        return Total;
    }

    bool FSwarm::FindNearest(const FVector2& Position, float Radius, FVector2& OutPosition) const
    {
        float BestSquared = Radius * Radius;
        bool bFound = false;

        const int32 MinX = Math::Clamp(int32((Position.x - Radius) / kGridCell), 0, kGridSide - 1);
        const int32 MaxX = Math::Clamp(int32((Position.x + Radius) / kGridCell), 0, kGridSide - 1);
        const int32 MinY = Math::Clamp(int32((Position.y - Radius) / kGridCell), 0, kGridSide - 1);
        const int32 MaxY = Math::Clamp(int32((Position.y + Radius) / kGridCell), 0, kGridSide - 1);

        for (int32 CellY = MinY; CellY <= MaxY; ++CellY)
        {
            for (int32 CellX = MinX; CellX <= MaxX; ++CellX)
            {
                const int32 Cell = CellY * kGridSide + CellX;
                for (int32 Slot = CellStart[Cell]; Slot < CellStart[Cell + 1]; ++Slot)
                {
                    const int32 Index = CellAgents[Slot];
                    if (Dead[Index] != 0)
                    {
                        continue;
                    }

                    const float DX = PositionX[Index] - Position.x;
                    const float DY = PositionY[Index] - Position.y;
                    const float Squared = DX * DX + DY * DY;
                    if (Squared < BestSquared)
                    {
                        BestSquared = Squared;
                        OutPosition = { PositionX[Index], PositionY[Index] };
                        bFound = true;
                    }
                }
            }
        }

        return bFound;
    }

    int32 FSwarm::GatherVisible(const FVector2& ViewMin, const FVector2& ViewMax,
                                FAgentInstance* Out, int32 Capacity) const
    {
        int32 Written = 0;

        const int32 MinX = Math::Clamp(int32(ViewMin.x / kGridCell), 0, kGridSide - 1);
        const int32 MaxX = Math::Clamp(int32(ViewMax.x / kGridCell), 0, kGridSide - 1);
        const int32 MinY = Math::Clamp(int32(ViewMin.y / kGridCell), 0, kGridSide - 1);
        const int32 MaxY = Math::Clamp(int32(ViewMax.y / kGridCell), 0, kGridSide - 1);

        for (int32 CellY = MinY; CellY <= MaxY && Written < Capacity; ++CellY)
        {
            for (int32 CellX = MinX; CellX <= MaxX && Written < Capacity; ++CellX)
            {
                const int32 Cell = CellY * kGridSide + CellX;
                const int32 End = Math::Min(CellStart[Cell + 1], CellStart[Cell] + (Capacity - Written));

                for (int32 Slot = CellStart[Cell]; Slot < End; ++Slot)
                {
                    const int32 Index = CellAgents[Slot];
                    if (Dead[Index] != 0)
                    {
                        continue;
                    }

                    const EAgentKind AgentKind = EAgentKind(Kind[Index]);

                    FAgentInstance& Instance = Out[Written++];
                    Instance.Position = { PositionX[Index], PositionY[Index] };
                    Instance.Radius   = StatsFor(AgentKind).Radius;
                    Instance.Packed   = PackAgent(AgentKind, Health[Index] / Math::Max(MaxHealth[Index], 1.0f),
                        Flash[Index], uint32(Index));
                }
            }
        }

        return Written;
    }
}
