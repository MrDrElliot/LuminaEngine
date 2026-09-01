#pragma once

#include "GameTypes.h"

namespace Umbral
{
    // Packed for the instance buffer; the swarm never becomes ECS entities, so this is its whole GPU form.
    struct FAgentInstance
    {
        FVector2 Position;
        float    Radius;
        uint32   Packed;
    };

    static_assert(sizeof(FAgentInstance) == 16, "The agent shader assumes a packed 16 byte instance.");

    struct FDamageVolume
    {
        FVector2 Center { 0.0f, 0.0f };
        float    Radius = 0.0f;
        float    Damage = 0.0f;
        float    Knock  = 0.0f;
        float    Pull   = 0.0f;
        float    Slow   = 0.0f;
    };

    // Structure of arrays rather than one entity per agent, which is what makes six figures affordable.
    class FSwarm
    {
    public:

        void Initialize();
        void Reset();

        NODISCARD int32 Num() const { return Count; }

        void Spawn(EAgentKind Kind, const FVector2& Position, FRandom& Rng);

        void BuildGrid();
        void Advance(float Delta, const FVector2& Target);

        // Returns souls earned; kills are appended to OutDeaths for the effect layer to consume.
        float ApplyDamage(TSpan<const FDamageVolume> Volumes, TVector<FVector2>& OutDeaths,
                          TVector<FVector4>& OutColors, int64& OutKills);

        void Compact();

        NODISCARD int32 NumDead() const { return DeadCount; }

        // Sum of contact damage inside Radius, and the push direction away from the densest side.
        float SampleContact(const FVector2& Position, float Radius, FVector2& OutPush) const;

        NODISCARD int32 CountNear(const FVector2& Position, float Radius) const;
        NODISCARD bool FindNearest(const FVector2& Position, float Radius, FVector2& OutPosition) const;

        // Writes visible agents into a caller owned instance block and returns how many landed.
        int32 GatherVisible(const FVector2& ViewMin, const FVector2& ViewMax, FAgentInstance* Out, int32 Capacity) const;

    private:

        NODISCARD int32 CellOf(float X, float Y) const;

        TVector<float>  PositionX;
        TVector<float>  PositionY;
        TVector<float>  VelocityX;
        TVector<float>  VelocityY;
        TVector<float>  Health;
        TVector<float>  MaxHealth;
        TVector<float>  Flash;
        TVector<uint8>  Kind;
        TVector<uint8>  Dead;

        TVector<int32>  CellStart;
        TVector<int32>  CellAgents;
        TVector<int32>  AgentCell;

        // One count table per chunk, so the counting sort never needs a serial pass over every agent.
        TVector<int32>  ChunkCounts;

        int32 Count = 0;
        int32 DeadCount = 0;
    };
}
