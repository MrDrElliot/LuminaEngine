#include "RuntimePCH.h"
#include "Simulation.h"

#include "Core/Math/Hash/Hash.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Serialization/NetArchive.h"
#include "Log/Log.h"
#include "Containers/Algorithm.h"
#include "Memory/Memcpy.h"
#include "Memory/Memory.h"
#include "Simulation/SimulationComponents.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/World.h"

namespace Lumina
{
    namespace SimulationInternal
    {
        // Covers the input delay plus every tick a peer can lag behind, since a slot is reused once its tick is that old.
        constexpr int64 kInputSlots = 256;

        // Ticks of local input repeated in every send, so one lost packet never stalls a peer.
        constexpr uint32 kRedundantTicks = 8;

        uint64 Mix(uint64 Hash, const void* Data, size_t Size)
        {
            return Hash::GetHash64(Data, Size, Hash);
        }

        bool BytesEqual(TSpan<const uint8> A, TSpan<const uint8> B)
        {
            if (A.Num() != B.Num())
            {
                return false;
            }
            for (int32 i = 0; i < (int32)A.Num(); ++i)
            {
                if (A[i] != B[i])
                {
                    return false;
                }
            }
            return true;
        }
    }

    FSimSessionDesc FSimSessionDesc::FromProjectSettings()
    {
        const CSimulationSettings* Settings = GetDefault<CSimulationSettings>();
        FSimSessionDesc Desc;
        Desc.Mode             = Settings->Mode;
        Desc.TickRate         = Settings->TickRate;
        Desc.InputDelayTicks  = Settings->InputDelayTicks;
        Desc.MaxRollbackTicks = Settings->MaxRollbackTicks;
        Desc.ChecksumInterval = Settings->ChecksumInterval;
        return Desc;
    }

    FSimulation::FSimulation(CWorld& InWorld)
        : World(InWorld)
    {
    }

    FSimulation::~FSimulation()
    {
        Stop();
    }

    bool FSimulation::Start(const FSimSessionDesc& Desc)
    {
        using namespace SimulationInternal;

        Stop();
        if (Desc.InputStruct == nullptr || Desc.InputStruct->GetSize() == 0 || Desc.NumPlayers == 0 || Desc.LocalPlayer >= Desc.NumPlayers)
        {
            LOG_ERROR("Simulation: a session needs an input struct and a local player inside its {} player(s).", Desc.NumPlayers);
            return false;
        }

        Session          = Desc;
        Mode             = Desc.Mode;
        TickSeconds      = 1.0f / (float)Math::Clamp(Desc.TickRate, 10, 240);
        InputDelay       = Math::Clamp(Desc.InputDelayTicks, 0, 15);
        MaxRollback      = Math::Clamp(Desc.MaxRollbackTicks, 1, 30);
        ChecksumInterval = Math::Max(Desc.ChecksumInterval, 0);
        InputSize        = Desc.InputStruct->GetSize();

        CurrentTick = SimulatingTick = 0;
        ConfirmedTick = EarliestMispredicted = DesyncTick = -1;
        NextLocalTick = InputDelay;
        NextChecksumTick = ChecksumInterval;
        RollbackCount = ResimulatedTicks = VerifiedChecksums = 0;
        Accumulator = 0.0;
        NextSimId = 1;
        Random.Seed(Desc.Seed);
        EntitiesBySimId.Reset();

        PendingLocalInput.SetNumZeroed(InputSize);

        Inputs.Reset();
        Inputs.SetNum(Desc.NumPlayers);
        for (TVector<FInputSlot>& PlayerSlots : Inputs)
        {
            PlayerSlots.SetNum((int32)kInputSlots);
            for (FInputSlot& Entry : PlayerSlots)
            {
                Entry.Bytes.SetNumZeroed(InputSize);
            }
        }

        // The delay's first ticks have no input from anyone, so every peer agrees they are empty.
        for (uint32 Player = 0; Player < Desc.NumPlayers; ++Player)
        {
            for (int64 Tick = 0; Tick < InputDelay; ++Tick)
            {
                FInputSlot& Entry = Slot(Player, Tick);
                Entry.Tick = Tick;
                Entry.bConfirmed = true;
            }
        }
        TickInputs.SetNum(Desc.NumPlayers);

        // Ordered by name, so every peer walks the same types in the same order.
        Columns.Reset();
        ForEachComponentStruct([this](CStruct* Struct)
        {
            if (Struct->HasMeta("Rollback") && Struct->GetComponentOps() != nullptr && Struct->GetSize() != 0)
            {
                Columns.Add({ Struct, Struct->GetComponentOps(), Align(Struct->GetSize(), Math::Max<uint32>(Struct->GetAlignment(), 16u)) });
            }
        });
        Algo::Sort(Columns, [](const FColumn& A, const FColumn& B) { return A.Struct->GetName().ToString() < B.Struct->GetName().ToString(); });

        Snapshots.SetNum(Mode == ESimulationMode::Rollback ? MaxRollback + 2 : 1);

        bRunning = true;
        LOG_INFO("Simulation: started {} for {} player(s) as player {}, {} Hz, {} rollback component type(s).",
            Mode == ESimulationMode::Rollback ? "rollback" : "lockstep", Desc.NumPlayers, Desc.LocalPlayer, (int32)(1.0f / TickSeconds + 0.5f), Columns.Num());
        return true;
    }

    void FSimulation::Stop()
    {
        for (FSnapshot& Snapshot : Snapshots)
        {
            ReleaseValues(Snapshot);
        }
        Snapshots.Reset();
        LocalChecksums.Reset();
        RemoteChecksums.Reset();
        OutgoingInputs.Reset();
        OutgoingChecksums.Reset();
        bRunning = false;
    }

    FSimulation::FInputSlot& FSimulation::Slot(uint32 Player, int64 Tick)
    {
        return Inputs[Player][(int32)(Tick % SimulationInternal::kInputSlots)];
    }

    const FSimulation::FInputSlot& FSimulation::Slot(uint32 Player, int64 Tick) const
    {
        return Inputs[Player][(int32)(Tick % SimulationInternal::kInputSlots)];
    }

    const void* FSimulation::GetInput(uint32 Player) const
    {
        return Player < (uint32)TickInputs.Num() ? TickInputs[Player] : nullptr;
    }

    void FSimulation::SubmitLocalInput(const void* Input)
    {
        if (!bRunning || Input == nullptr)
        {
            return;
        }
        Memory::Memcpy(PendingLocalInput.GetData(), Input, InputSize);
    }

    void FSimulation::StampLocalInput(int64 Tick)
    {
        // The last submitted input stands for every tick until a new one arrives, so a quiet frame repeats it.
        for (; NextLocalTick <= Tick; ++NextLocalTick)
        {
            FInputSlot& Entry = Slot(Session.LocalPlayer, NextLocalTick);
            Entry.Tick = NextLocalTick;
            Entry.bConfirmed = true;
            Entry.bUsed = false;
            Memory::Memcpy(Entry.Bytes.GetData(), PendingLocalInput.GetData(), InputSize);
        }
    }

    bool FSimulation::AllInputsConfirmed(int64 Tick) const
    {
        for (uint32 Player = 0; Player < Session.NumPlayers; ++Player)
        {
            const FInputSlot& Entry = Slot(Player, Tick);
            if (Entry.Tick != Tick || !Entry.bConfirmed)
            {
                return false;
            }
        }
        return true;
    }

    void FSimulation::UpdateConfirmedTick()
    {
        // Bounded by the slots, since a slot past them has not been written yet.
        while (ConfirmedTick + 1 < (int64)CurrentTick + SimulationInternal::kInputSlots - 1 && AllInputsConfirmed(ConfirmedTick + 1))
        {
            ++ConfirmedTick;
        }
    }

    void FSimulation::BindInputsFor(int64 Tick)
    {
        for (uint32 Player = 0; Player < Session.NumPlayers; ++Player)
        {
            FInputSlot& Entry = Slot(Player, Tick);
            if (Entry.Tick != Tick || !Entry.bConfirmed)
            {
                // A missing input is predicted as the newest one confirmed, which is what a held button or stick most often is.
                const FInputSlot* Latest = nullptr;
                for (int64 Back = Tick - 1; Back >= 0 && Back > Tick - SimulationInternal::kInputSlots; --Back)
                {
                    const FInputSlot& Candidate = Slot(Player, Back);
                    if (Candidate.Tick == Back && Candidate.bConfirmed)
                    {
                        Latest = &Candidate;
                        break;
                    }
                }
                Entry.Tick = Tick;
                Entry.bConfirmed = false;
                if (Latest != nullptr)
                {
                    Memory::Memcpy(Entry.Bytes.GetData(), Latest->Bytes.GetData(), InputSize);
                }
                else
                {
                    Memory::Memzero(Entry.Bytes.GetData(), InputSize);
                }
            }
            Entry.bUsed = true;
            TickInputs[Player] = Entry.Bytes.GetData();
        }
    }

    void FSimulation::ReceiveInputs(const FSimInputRun& Run)
    {
        using namespace SimulationInternal;

        if (!bRunning || Run.Player >= Session.NumPlayers || Run.Player == Session.LocalPlayer
            || (uint64)Run.Bytes.Num() != (uint64)Run.NumTicks * InputSize)
        {
            return;
        }

        for (uint32 Index = 0; Index < Run.NumTicks; ++Index)
        {
            const int64 Tick = (int64)Run.FirstTick + Index;
            if (Tick <= ConfirmedTick || Tick >= (int64)CurrentTick + kInputSlots - 1)
            {
                continue;
            }

            FInputSlot& Entry = Slot(Run.Player, Tick);
            if (Entry.Tick == Tick && Entry.bConfirmed)
            {
                continue;
            }

            const TSpan<const uint8> Arrived(Run.Bytes.GetData() + (size_t)Index * InputSize, InputSize);
            const bool bWasPredicted = Entry.Tick == Tick && Entry.bUsed;
            if (bWasPredicted && !BytesEqual(Arrived, TSpan<const uint8>(Entry.Bytes.GetData(), InputSize)))
            {
                EarliestMispredicted = EarliestMispredicted < 0 ? Tick : Math::Min(EarliestMispredicted, Tick);
            }

            Entry.Tick = Tick;
            Entry.bConfirmed = true;
            Entry.bUsed = bWasPredicted;
            Memory::Memcpy(Entry.Bytes.GetData(), Arrived.GetData(), InputSize);
        }
        UpdateConfirmedTick();
    }

    void FSimulation::TakeOutgoingInputs(TVector<FSimInputRun>& Out)
    {
        using namespace SimulationInternal;

        if (!bRunning || NextLocalTick <= InputDelay)
        {
            return;
        }

        FSimInputRun& Run = Out.EmplaceGetRef();
        Run.Player    = Session.LocalPlayer;
        Run.FirstTick = (uint32)Math::Max<int64>(InputDelay, NextLocalTick - kRedundantTicks);
        Run.NumTicks  = (uint32)(NextLocalTick - Run.FirstTick);
        Run.Bytes.SetNum((int32)(Run.NumTicks * InputSize));
        for (uint32 Index = 0; Index < Run.NumTicks; ++Index)
        {
            Memory::Memcpy(Run.Bytes.GetData() + (size_t)Index * InputSize, Slot(Session.LocalPlayer, Run.FirstTick + Index).Bytes.GetData(), InputSize);
        }
    }

    void FSimulation::TakeOutgoingChecksums(TVector<FSimChecksum>& Out)
    {
        Out.Append(OutgoingChecksums);
        OutgoingChecksums.Reset();
    }

    ECS::FEntity FSimulation::Spawn()
    {
        ECS::FRegistry& Registry = World.GetEntityRegistry();
        const ECS::FEntity Entity = Registry.Create();
        SSimEntity& Sim = Registry.Emplace<SSimEntity>(Entity);
        Sim.SimId     = NextSimId++;
        Sim.SpawnTick = SimulatingTick;
        EntitiesBySimId.Add(Sim.SimId, Entity);
        return Entity;
    }

    void FSimulation::Destroy(ECS::FEntity Entity)
    {
        ECS::FRegistry& Registry = World.GetEntityRegistry();
        const SSimEntity* Sim = Registry.IsValid(Entity) ? Registry.TryGet<SSimEntity>(Entity) : nullptr;
        if (Sim == nullptr || Registry.TryGet<SSimPendingDestroy>(Entity) != nullptr)
        {
            return;
        }

        // Nothing rewinds in lockstep, so the entity can go at once.
        if (Mode == ESimulationMode::Lockstep)
        {
            EntitiesBySimId.Remove(Sim->SimId);
            Registry.Destroy(Entity);
            return;
        }

        for (const FColumn& Column : Columns)
        {
            Column.Ops->Remove(Registry, Entity);
        }
        SSimPendingDestroy& Pending = Registry.Emplace<SSimPendingDestroy>(Entity);
        Pending.Tick = SimulatingTick;
        if (!Registry.HasAny<SDisabledTag>(Entity))
        {
            Registry.Emplace<SDisabledTag>(Entity);
            Pending.bAddedDisabledTag = true;
        }
    }

    ECS::FEntity FSimulation::FindEntity(uint32 SimId) const
    {
        const ECS::FEntity* Found = EntitiesBySimId.Find(SimId);
        return Found != nullptr ? *Found : ECS::NullEntity;
    }

    void FSimulation::GetEntities(TVector<ECS::FEntity>& Out) const
    {
        ECS::FRegistry& Registry = World.GetEntityRegistry();
        TVector<TPair<uint32, ECS::FEntity>> Alive;
        Registry.View<SSimEntity>().ForEach([&](ECS::FEntity Entity, const SSimEntity& Sim)
        {
            if (!Registry.HasAny<SSimPendingDestroy>(Entity))
            {
                Alive.Emplace(Sim.SimId, Entity);
            }
        });
        Algo::Sort(Alive, [](const TPair<uint32, ECS::FEntity>& A, const TPair<uint32, ECS::FEntity>& B) { return A.Key < B.Key; });
        Out.Reset();
        for (const TPair<uint32, ECS::FEntity>& Entry : Alive)
        {
            Out.Add(Entry.Value);
        }
    }

    void FSimulation::Advance(double FrameSeconds)
    {
        if (!bRunning)
        {
            return;
        }

        // A stall must not leave a backlog larger than the run-ahead it could ever catch up through.
        Accumulator = Math::Min(Accumulator + FrameSeconds, (double)TickSeconds * (MaxRollback + 1));
        UpdateConfirmedTick();

        if (Mode == ESimulationMode::Rollback && EarliestMispredicted >= 0 && EarliestMispredicted < (int64)CurrentTick)
        {
            FSnapshot* Snapshot = FindSnapshot(EarliestMispredicted);
            if (Snapshot != nullptr)
            {
                Restore(*Snapshot);
                ++RollbackCount;
                for (uint32 Tick = (uint32)EarliestMispredicted; Tick < CurrentTick; ++Tick)
                {
                    SimulateTick(Tick, true);
                    ++ResimulatedTicks;
                }
            }
            else
            {
                LOG_ERROR("Simulation: tick {} needs a rewind but has no snapshot left, so this peer will desync.", EarliestMispredicted);
            }
        }
        EarliestMispredicted = -1;

        while (Accumulator >= TickSeconds)
        {
            const uint32 Tick = CurrentTick;
            StampLocalInput((int64)Tick + InputDelay);

            const bool bWaitForInputs = Mode == ESimulationMode::Lockstep ? !AllInputsConfirmed(Tick) : (int64)Tick - ConfirmedTick > MaxRollback;
            if (bWaitForInputs)
            {
                break;
            }

            SimulateTick(Tick, false);
            ++CurrentTick;
            Accumulator -= TickSeconds;
            UpdateConfirmedTick();
        }

        FinalizeDestroys();
        QueueChecksums();
    }

    void FSimulation::SimulateTick(uint32 Tick, bool bResim)
    {
        SimulatingTick = Tick;
        bResimulating  = bResim;

        if (Mode == ESimulationMode::Rollback)
        {
            Capture(Snapshots[(int32)(Tick % (uint32)Snapshots.Num())], Tick);
        }
        else if (ChecksumInterval > 0 && Tick != 0 && Tick % (uint32)ChecksumInterval == 0)
        {
            // Lockstep only runs a tick once every input is in, so the state it starts from is already final.
            Capture(Snapshots[0], Tick);
            FSimChecksum Checksum = ChecksumOf(Snapshots[0]);
            LocalChecksums.Add(Tick, Checksum);
            OutgoingChecksums.Add(Move(Checksum));
            CompareChecksums(Tick);
        }

        BindInputsFor(Tick);
        World.RunSimulationStage(TickSeconds, Tick);
        bResimulating = false;
    }

    FSimulation::FSnapshot* FSimulation::FindSnapshot(int64 Tick)
    {
        if (Tick < 0 || Snapshots.IsEmpty())
        {
            return nullptr;
        }
        FSnapshot& Snapshot = Snapshots[(int32)(Tick % Snapshots.Num())];
        return Snapshot.Tick == Tick ? &Snapshot : nullptr;
    }

    void FSimulation::ReleaseValues(FSnapshot& Snapshot)
    {
        for (int32 C = 0; C < Snapshot.ColumnValues.Num(); ++C)
        {
            uint8* Values = Snapshot.ColumnValues[C];
            if (Values == nullptr)
            {
                continue;
            }
            const FColumn& Column = Columns[C];
            for (int32 i = 0; i < Snapshot.ColumnIds[C].Num(); ++i)
            {
                Column.Struct->DestroyStruct(Values + (size_t)i * Column.Stride);
            }
            Memory::Free(Values);
        }
        Snapshot.ColumnValues.Reset();
        Snapshot.ColumnIds.Reset();
        Snapshot.ColumnCapacity.Reset();
        Snapshot.Tick = -1;
    }

    void FSimulation::Capture(FSnapshot& Snapshot, uint32 Tick)
    {
        ECS::FRegistry& Registry = World.GetEntityRegistry();

        // Values are destroyed but their buffers kept, so a steady population captures without allocating.
        for (int32 C = 0; C < Snapshot.ColumnValues.Num(); ++C)
        {
            for (int32 i = 0; i < Snapshot.ColumnIds[C].Num(); ++i)
            {
                Columns[C].Struct->DestroyStruct(Snapshot.ColumnValues[C] + (size_t)i * Columns[C].Stride);
            }
            Snapshot.ColumnIds[C].Reset();
        }
        if (Snapshot.ColumnValues.Num() != Columns.Num())
        {
            ReleaseValues(Snapshot);
            Snapshot.ColumnIds.SetNum(Columns.Num());
            Snapshot.ColumnValues.SetNum(Columns.Num(), nullptr);
            Snapshot.ColumnCapacity.SetNum(Columns.Num(), 0u);
        }

        Snapshot.Tick      = Tick;
        Snapshot.NextSimId = NextSimId;
        Snapshot.Random    = Random;

        TVector<TPair<uint32, ECS::FEntity>> Alive;
        Registry.View<SSimEntity>().ForEach([&](ECS::FEntity Entity, const SSimEntity& Sim)
        {
            if (Registry.TryGet<SSimPendingDestroy>(Entity) == nullptr)
            {
                Alive.Emplace(Sim.SimId, Entity);
            }
        });
        Algo::Sort(Alive, [](const TPair<uint32, ECS::FEntity>& A, const TPair<uint32, ECS::FEntity>& B) { return A.Key < B.Key; });

        Snapshot.AliveSimIds.Reset();
        for (const TPair<uint32, ECS::FEntity>& Entry : Alive)
        {
            Snapshot.AliveSimIds.Add(Entry.Key);
        }

        for (int32 C = 0; C < Columns.Num(); ++C)
        {
            const FColumn& Column = Columns[C];
            uint32 Count = 0;
            for (const TPair<uint32, ECS::FEntity>& Entry : Alive)
            {
                Count += Column.Ops->Has(Registry, Entry.Value) != 0 ? 1u : 0u;
            }
            if (Count > Snapshot.ColumnCapacity[C])
            {
                Memory::Free(Snapshot.ColumnValues[C]);
                Snapshot.ColumnValues[C]   = static_cast<uint8*>(Memory::Malloc((size_t)Count * Column.Stride, Math::Max<uint32>(Column.Struct->GetAlignment(), 16u)));
                Snapshot.ColumnCapacity[C] = Count;
            }

            for (const TPair<uint32, ECS::FEntity>& Entry : Alive)
            {
                if (Column.Ops->Has(Registry, Entry.Value) == 0)
                {
                    continue;
                }
                uint8* Value = Snapshot.ColumnValues[C] + (size_t)Snapshot.ColumnIds[C].Num() * Column.Stride;
                Column.Struct->InitializeStruct(Value);
                Column.Struct->CopyStruct(Value, Column.Ops->GetUnmarked(Registry, Entry.Value));
                Snapshot.ColumnIds[C].Add(Entry.Key);
            }
        }
    }

    void FSimulation::Restore(const FSnapshot& Snapshot)
    {
        ECS::FRegistry& Registry = World.GetEntityRegistry();

        THashSet<uint32> AliveThen;
        for (uint32 SimId : Snapshot.AliveSimIds)
        {
            AliveThen.Add(SimId);
        }

        // Spawned after the snapshot goes, and destroyed at or after it comes back, both to be replayed by the resimulation.
        TVector<ECS::FEntity> Spawned;
        TVector<ECS::FEntity> Revived;
        Registry.View<SSimEntity>().ForEach([&](ECS::FEntity Entity, const SSimEntity& Sim)
        {
            const SSimPendingDestroy* Pending = Registry.TryGet<SSimPendingDestroy>(Entity);
            if (!AliveThen.Contains(Sim.SimId))
            {
                if (Pending == nullptr || Pending->Tick >= (uint32)Snapshot.Tick)
                {
                    Spawned.Add(Entity);
                }
            }
            else if (Pending != nullptr)
            {
                Revived.Add(Entity);
            }
        });
        for (ECS::FEntity Entity : Spawned)
        {
            EntitiesBySimId.Remove(Registry.Get<SSimEntity>(Entity).SimId);
            Registry.Destroy(Entity);
        }
        for (ECS::FEntity Entity : Revived)
        {
            if (Registry.Get<SSimPendingDestroy>(Entity).bAddedDisabledTag)
            {
                Registry.Remove<SDisabledTag>(Entity);
            }
            Registry.Remove<SSimPendingDestroy>(Entity);
        }

        for (int32 C = 0; C < Columns.Num(); ++C)
        {
            const FColumn& Column = Columns[C];
            const TVector<uint32>& Ids = Snapshot.ColumnIds[C];

            THashSet<uint32> Held;
            for (uint32 SimId : Ids)
            {
                Held.Add(SimId);
            }

            TVector<ECS::FEntity> Stale;
            Registry.View<SSimEntity>().ForEach([&](ECS::FEntity Entity, const SSimEntity& Sim)
            {
                if (!Held.Contains(Sim.SimId) && Column.Ops->Has(Registry, Entity) != 0)
                {
                    Stale.Add(Entity);
                }
            });
            for (ECS::FEntity Entity : Stale)
            {
                Column.Ops->Remove(Registry, Entity);
            }

            for (int32 i = 0; i < Ids.Num(); ++i)
            {
                const ECS::FEntity Entity = FindEntity(Ids[i]);
                if (Entity == ECS::NullEntity)
                {
                    continue;
                }
                const uint8* Value = Snapshot.ColumnValues[C] + (size_t)i * Column.Stride;
                if (Column.Ops->Has(Registry, Entity) != 0)
                {
                    Column.Struct->CopyStruct(Column.Ops->Get(Registry, Entity), Value);
                }
                else
                {
                    Column.Ops->EmplaceCopy(Registry, Entity, Value);
                }
            }
        }

        NextSimId = Snapshot.NextSimId;
        Random    = Snapshot.Random;
    }

    void FSimulation::FinalizeDestroys()
    {
        ECS::FRegistry& Registry = World.GetEntityRegistry();
        TVector<ECS::FEntity> Final;
        Registry.View<SSimPendingDestroy>().ForEach([&](ECS::FEntity Entity, const SSimPendingDestroy& Pending)
        {
            if ((int64)Pending.Tick <= ConfirmedTick && Pending.Tick < CurrentTick)
            {
                Final.Add(Entity);
            }
        });
        for (ECS::FEntity Entity : Final)
        {
            if (const SSimEntity* Sim = Registry.TryGet<SSimEntity>(Entity))
            {
                EntitiesBySimId.Remove(Sim->SimId);
            }
            Registry.Destroy(Entity);
        }
    }

    FSimChecksum FSimulation::ChecksumOf(const FSnapshot& Snapshot) const
    {
        using namespace SimulationInternal;

        FSimChecksum Out;
        Out.Tick = (uint32)Snapshot.Tick;

        uint64 Total = Mix(0x51AC0DE5ull, &Snapshot.NextSimId, sizeof(Snapshot.NextSimId));
        Total = Mix(Total, &Snapshot.Random, sizeof(Snapshot.Random));
        Total = Mix(Total, Snapshot.AliveSimIds.GetData(), (size_t)Snapshot.AliveSimIds.Num() * sizeof(uint32));

        // Hashed through the net serializer, which writes field values only, so padding and pointers never reach the digest.
        TVector<uint8> Scratch;
        for (int32 C = 0; C < Columns.Num(); ++C)
        {
            uint64 Column = 0xC0FFEEull;
            for (int32 i = 0; i < Snapshot.ColumnIds[C].Num(); ++i)
            {
                Scratch.Reset();
                FNetArchive Writer(Scratch);
                Columns[C].Struct->NetSerializeAll(Writer, Snapshot.ColumnValues[C] + (size_t)i * Columns[C].Stride);
                Column = Mix(Column, &Snapshot.ColumnIds[C][i], sizeof(uint32));
                Column = Mix(Column, Scratch.GetData(), (size_t)Scratch.Num());
            }
            Out.PerComponent.Add(Column);
            Total = Mix(Total, &Column, sizeof(Column));
        }
        Out.Total = Total;
        return Out;
    }

    FSimChecksum FSimulation::ComputeChecksum() const
    {
        FSnapshot Scratch;
        const_cast<FSimulation*>(this)->Capture(Scratch, CurrentTick);
        FSimChecksum Out = ChecksumOf(Scratch);
        const_cast<FSimulation*>(this)->ReleaseValues(Scratch);
        return Out;
    }

    void FSimulation::QueueChecksums()
    {
        if (Mode != ESimulationMode::Rollback || ChecksumInterval <= 0)
        {
            return;
        }

        // The state a tick starts from is final once every input before it is known and the tick has run.
        while (NextChecksumTick <= ConfirmedTick + 1 && NextChecksumTick < (int64)CurrentTick)
        {
            const FSnapshot* Snapshot = FindSnapshot(NextChecksumTick);
            if (Snapshot != nullptr)
            {
                FSimChecksum Checksum = ChecksumOf(*Snapshot);
                LocalChecksums.Add((uint32)NextChecksumTick, Checksum);
                OutgoingChecksums.Add(Move(Checksum));
                CompareChecksums((uint32)NextChecksumTick);
            }
            NextChecksumTick += ChecksumInterval;
        }
    }

    void FSimulation::ReceiveChecksum(uint32 Player, const FSimChecksum& Checksum)
    {
        if (!bRunning || Player >= Session.NumPlayers || Player == Session.LocalPlayer)
        {
            return;
        }
        RemoteChecksums.Add(((uint64)Checksum.Tick << 8) | Player, Checksum);
        CompareChecksums(Checksum.Tick);
    }

    void FSimulation::CompareChecksums(uint32 Tick)
    {
        const FSimChecksum* Local = LocalChecksums.Find(Tick);
        if (Local == nullptr)
        {
            return;
        }

        for (uint32 Player = 0; Player < Session.NumPlayers; ++Player)
        {
            const uint64 Key = ((uint64)Tick << 8) | Player;
            const FSimChecksum* Remote = RemoteChecksums.Find(Key);
            if (Remote == nullptr)
            {
                continue;
            }

            if (Remote->Total != Local->Total && DesyncTick < 0)
            {
                DesyncTick = Tick;
                FString Diverged;
                for (int32 C = 0; C < Columns.Num() && C < Remote->PerComponent.Num(); ++C)
                {
                    if (Remote->PerComponent[C] != Local->PerComponent[C])
                    {
                        Diverged += Diverged.IsEmpty() ? "" : ", ";
                        Diverged += Columns[C].Struct->GetName().ToString();
                    }
                }
                LOG_ERROR("Simulation: desync with player {} at tick {}. Diverged state [{}]{}.", Player, Tick,
                    Diverged.IsEmpty() ? FString("entity set, random stream or spawn counter") : Diverged,
                    Remote->PerComponent.Num() != Columns.Num() ? FString(", and the peers disagree on the rollback component types") : FString());
            }
            VerifiedChecksums += Remote->Total == Local->Total ? 1u : 0u;
            RemoteChecksums.Remove(Key);
        }

        // Kept only while a checksum from another peer for it could still arrive.
        if (Tick > 600u * (uint32)Math::Max(ChecksumInterval, 1))
        {
            LocalChecksums.Remove(Tick - 600u * (uint32)Math::Max(ChecksumInterval, 1));
        }
    }
}
