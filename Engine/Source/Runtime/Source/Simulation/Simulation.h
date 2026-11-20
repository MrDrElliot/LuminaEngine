#pragma once

#include "Containers/HashTable.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Core/Math/Random.h"
#include "Simulation/SimulationSettings.h"
#include "World/ECS/Entity.h"

namespace Lumina
{
    class CWorld;
    class CStruct;
    struct FComponentOps;

    struct FSimSessionDesc
    {
        uint32 NumPlayers  = 1;
        uint32 LocalPlayer = 0;
        uint64 Seed        = 0;

        // Plain data, copied and sent as raw bytes, so every peer must run the same build.
        const CStruct* InputStruct = nullptr;

        ESimulationMode Mode             = ESimulationMode::Rollback;
        int32           TickRate         = 60;
        int32           InputDelayTicks  = 2;
        int32           MaxRollbackTicks = 8;
        int32           ChecksumInterval = 10;

        // Every peer has to agree on these, so a session is described from the one project setting.
        RUNTIME_API static FSimSessionDesc FromProjectSettings();
    };

    // Inputs for consecutive ticks of one player, as they cross the network. Each send repeats recent ticks, so a lost packet costs nothing.
    struct FSimInputRun
    {
        uint32         Player    = 0;
        uint32         FirstTick = 0;
        uint32         NumTicks  = 0;
        TVector<uint8> Bytes;
    };

    // A digest of the state a tick starts from, one per rollback component type, so a mismatch names the type that diverged.
    struct FSimChecksum
    {
        uint32          Tick  = 0;
        uint64          Total = 0;
        TVector<uint64> PerComponent;
    };

    // One world's deterministic simulation, stepping the Simulation stage from player inputs alone and rewinding when a predicted input proves wrong.
    class RUNTIME_API FSimulation
    {
    public:

        explicit FSimulation(CWorld& InWorld);
        ~FSimulation();

        FSimulation(const FSimulation&) = delete;
        FSimulation& operator=(const FSimulation&) = delete;

        bool Start(const FSimSessionDesc& Desc);
        void Stop();
        NODISCARD bool IsRunning() const { return bRunning; }

        NODISCARD ESimulationMode GetMode() const { return Mode; }
        NODISCARD uint32 GetNumPlayers() const { return Session.NumPlayers; }
        NODISCARD uint32 GetLocalPlayer() const { return Session.LocalPlayer; }
        NODISCARD const CStruct* GetInputStruct() const { return Session.InputStruct; }

        //~ For systems in the Simulation stage.

        // The tick being simulated, which is also the count of ticks that came before it.
        NODISCARD uint32 GetTick() const { return SimulatingTick; }
        NODISCARD float GetTickSeconds() const { return TickSeconds; }

        // A replay after a misprediction, where effects already played the first time should not play again.
        NODISCARD bool IsResimulating() const { return bResimulating; }

        // Seeded per session and saved with every snapshot, so a replay draws the same numbers.
        NODISCARD FRandomStream& GetRandom() { return Random; }

        NODISCARD const void* GetInput(uint32 Player) const;

        template<typename T>
        NODISCARD const T& GetInput(uint32 Player) const { return *static_cast<const T*>(GetInput(Player)); }

        // Creates an entity with a SimId the same on every peer. Only the simulation may spawn simulation entities.
        ECS::FEntity Spawn();

        // Takes the entity's rollback state away now and destroys it once no rewind can reach this tick.
        void Destroy(ECS::FEntity Entity);

        NODISCARD ECS::FEntity FindEntity(uint32 SimId) const;

        // Live simulation entities in SimId order, the one order every peer agrees on, unlike a view's.
        void GetEntities(TVector<ECS::FEntity>& Out) const;

        //~ For the game.

        // The local player's input for the next tick to be stamped, held until a tick takes it.
        void SubmitLocalInput(const void* Input);

        template<typename T>
        void SubmitLocalInput(const T& Input) { SubmitLocalInput(static_cast<const void*>(&Input)); }

        // The same held input, for a caller that fills it in place.
        NODISCARD void* GetLocalInput() { return bRunning ? PendingLocalInput.GetData() : nullptr; }

        // Runs every tick the frame owes, rewinding first if a late input proved a prediction wrong.
        void Advance(double FrameSeconds);

        // How far this frame sits between the last tick and the next, so rendering can blend two tick states instead of stepping.
        NODISCARD float GetInterpolationAlpha() const
        {
            const double Alpha = TickSeconds > 0.0f ? Accumulator / TickSeconds : 0.0;
            return (float)(Alpha < 1.0 ? Alpha : 1.0);
        }

        //~ For a transport.

        void ReceiveInputs(const FSimInputRun& Run);
        void ReceiveChecksum(uint32 Player, const FSimChecksum& Checksum);

        // Local inputs not yet sent, each run repeating the last few ticks.
        void TakeOutgoingInputs(TVector<FSimInputRun>& Out);
        void TakeOutgoingChecksums(TVector<FSimChecksum>& Out);

        //~ Diagnostics.

        // The newest tick every player's input is known for, from which nothing can be rewound.
        NODISCARD int64 GetConfirmedTick() const { return ConfirmedTick; }
        NODISCARD uint32 GetCurrentTick() const { return CurrentTick; }
        NODISCARD uint32 GetRollbackCount() const { return RollbackCount; }
        NODISCARD uint32 GetResimulatedTicks() const { return ResimulatedTicks; }
        NODISCARD bool HasDesynced() const { return DesyncTick >= 0; }
        NODISCARD uint32 GetVerifiedChecksums() const { return VerifiedChecksums; }
        NODISCARD int64 GetDesyncTick() const { return DesyncTick; }

        NODISCARD FSimChecksum ComputeChecksum() const;
        NODISCARD uint32 GetInputSize() const { return InputSize; }

    private:

        struct FInputSlot
        {
            int64          Tick = -1;
            bool           bConfirmed = false;
            // Set once a tick ran on this slot, so a confirmation that differs proves the prediction wrong.
            bool           bUsed = false;
            TVector<uint8> Bytes;
        };

        struct FColumn
        {
            const CStruct*       Struct = nullptr;
            const FComponentOps* Ops    = nullptr;
            uint32               Stride = 0;
        };

        struct FSnapshot
        {
            int64            Tick = -1;
            uint32           NextSimId = 1;
            FRandomStream    Random;
            TVector<uint32>  AliveSimIds;
            // Per column, the SimIds holding it and their values back to back at the column's stride.
            TVector<TVector<uint32>> ColumnIds;
            TVector<uint8*>  ColumnValues;
            TVector<uint32>  ColumnCapacity;
        };

        FInputSlot& Slot(uint32 Player, int64 Tick);
        const FInputSlot& Slot(uint32 Player, int64 Tick) const;
        void StampLocalInput(int64 Tick);
        bool AllInputsConfirmed(int64 Tick) const;
        void BindInputsFor(int64 Tick);
        void UpdateConfirmedTick();

        void SimulateTick(uint32 Tick, bool bResim);
        void Capture(FSnapshot& Snapshot, uint32 Tick);
        void Restore(const FSnapshot& Snapshot);
        void ReleaseValues(FSnapshot& Snapshot);
        FSnapshot* FindSnapshot(int64 Tick);
        void FinalizeDestroys();
        void QueueChecksums();
        void CompareChecksums(uint32 Tick);
        FSimChecksum ChecksumOf(const FSnapshot& Snapshot) const;

        CWorld&         World;
        FSimSessionDesc Session;
        ESimulationMode Mode = ESimulationMode::Rollback;
        bool            bRunning = false;
        bool            bResimulating = false;

        float  TickSeconds = 1.0f / 60.0f;
        double Accumulator = 0.0;
        int32  InputDelay = 2;
        int32  MaxRollback = 8;
        int32  ChecksumInterval = 10;

        uint32 InputSize = 0;
        uint32 CurrentTick = 0;
        uint32 SimulatingTick = 0;
        int64  ConfirmedTick = -1;
        int64  EarliestMispredicted = -1;
        int64  NextLocalTick = 0;
        int64  NextChecksumTick = 0;
        int64  DesyncTick = -1;
        uint32 RollbackCount = 0;
        uint32 ResimulatedTicks = 0;
        uint32 VerifiedChecksums = 0;

        uint32 NextSimId = 1;
        FRandomStream Random;
        THashMap<uint32, ECS::FEntity> EntitiesBySimId;

        TVector<uint8>          PendingLocalInput;
        TVector<TVector<FInputSlot>> Inputs;
        // The inputs the tick being simulated reads, one per player.
        TVector<const uint8*>   TickInputs;

        TVector<FColumn>        Columns;
        TVector<FSnapshot>      Snapshots;

        TVector<FSimInputRun>   OutgoingInputs;
        TVector<FSimChecksum>   OutgoingChecksums;
        THashMap<uint32, FSimChecksum> LocalChecksums;
        // Keyed by tick and player, waiting for the local checksum of the same tick.
        THashMap<uint64, FSimChecksum> RemoteChecksums;
    };
}
