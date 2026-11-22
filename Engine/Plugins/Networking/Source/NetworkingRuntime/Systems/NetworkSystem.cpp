#include "RuntimePCH.h"
#include "NetworkSystem.h"
#include "World/Entity/Components/EntityTags.h"
#include "World/ECS/Registry.h"

#include "World/Entity/Systems/SystemContext.h"
#include "World/World.h"
#include "World/WorldContext.h"
#include "World/WorldManager.h"
#include "Net/NetWorldState.h"
#include "Core/Engine/Engine.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectCore.h"
#include "Net/NetReplication.h"
#include "Net/ScriptRepState.h"
#include "World/Entity/EntityUtils.h"
#include "Components/NetworkComponent.h"
#include "World/Entity/Components/TransformComponent.h"
#include "World/Entity/Components/TagComponent.h"
#include "Components/RepTransformComponent.h"
#include "Net/NetReplicationGraph.h"
#include "Net/NetPrediction.h"
#include "Net/NetCountingTransport.h"
#include "TaskSystem/TaskSystem.h"
#include "Config/NetworkSettings.h"
#include "World/Subsystems/WorldSettings.h"
#include "NetworkGlobals.h"
#include "Platform/Time/PlatformTime.h"
#include "Platform/Process/PlatformProcess.h"
#include "Networking/INetworkTransport.h"
#include "Physics/PhysicsScene.h"
#include "Physics/PhysicsTypes.h"
#include "World/Entity/Components/CharacterComponent.h"
#include "World/Entity/Components/CharacterControllerComponent.h"
#include "World/Entity/Components/PhysicsComponent.h"
#include "Core/Serialization/NetArchive.h"
#include "Core/Serialization/NetQuantize.h"
#include "Log/Log.h"
#include "Core/CommandLine/CommandLine.h"
#include "Scripting/EntityScript.h"
#include "Scripting/ScriptNet.h"
#include "Net/NetMessages.h"
#include "LuminaNetworkRuntime.h"
#include "World/Entity/Systems/ScriptSyncSystem.h"
#include "Networking/NetworkEvents.h"
#include "Net/NetSimulation.h"
#include "Core/Object/ScriptClass.h"
#include "World/Entity/Systems/LifetimeSystem.h"

namespace Lumina
{
    namespace NetworkSystemInternal
    {
        constexpr uint16 NetDefaultPort = 7777;

        bool IsServerMode(ENetMode Mode)
        {
            return Mode == ENetMode::ListenServer || Mode == ENetMode::DedicatedServer;
        }

        const char* NetModeToString(ENetMode Mode)
        {
            switch (Mode)
            {
            case ENetMode::Standalone:      return "Standalone";
            case ENetMode::Client:          return "Client";
            case ENetMode::ListenServer:    return "ListenServer";
            case ENetMode::DedicatedServer: return "DedicatedServer";
            }
            return "Unknown";
        }

        void InheritNetworking(ECS::FRegistry& Registry, ECS::FEntity Parent, TVector<ECS::FEntity>* OutAdded)
        {
            Registry.GetHierarchy().ForEachChild(Parent, [&](ECS::FEntity Child)
            {
                if (!Registry.HasAll<SNetworkComponent>(Child))
                {
                    // Copied out first, since adding to the same pool can move the parent's component.
                    SNetworkComponent Inherited = Registry.Get<ECS::Read<SNetworkComponent>>(Parent);
                    Inherited.NetGUID       = SNetGUID{};
                    Inherited.LocalRole     = ENetRole::None;
                    Inherited.RemoteRole    = ENetRole::None;
                    Inherited.PredictedBy   = 0;
                    Inherited.PredictionKey = 0;
                    Inherited.bInherited    = true;
                    Inherited.bProxyPhysicsConfigured = false;
                    Registry.Emplace<SNetworkComponent>(Child, Inherited);
                    if (OutAdded != nullptr)
                    {
                        OutAdded->Add(Child);
                    }
                }
                InheritNetworking(Registry, Child, OutAdded);
            });
        }

        // Sorted by entity id, which deserialization recreates identically, so references match with no spawn.
        void AdoptStableNetworkEntities(ECS::FRegistry& Registry, FNetWorldState& State, bool bAuthority)
        {
            TVector<ECS::FEntity> Networked;
            for (ECS::FEntity Entity : Registry.View<ECS::Read<SNetworkComponent>>())
            {
                Networked.Add(Entity);
            }

            // Both peers load the same map, so both inherit into the same children and number them alike.
            for (int32 Index = 0, Authored = Networked.Num(); Index < Authored; ++Index)
            {
                InheritNetworking(Registry, Networked[Index], &Networked);
            }

            Algo::Sort(Networked, [](ECS::FEntity A, ECS::FEntity B)
            {
                return (A).Value < (B).Value;
            });

            uint32 StableId = 1; // stable range is [1, NetGUID_DynamicStart)
            for (ECS::FEntity Entity : Networked)
            {
                SNetworkComponent& Net = Registry.Get<ECS::Write<SNetworkComponent>>(Entity);
                if (!Net.bNetLoadOnClient)
                {
                    continue;
                }
                Net.NetGUID            = SNetGUID{ StableId++ };
                Net.OwningConnectionId = 0;
                State.GuidTable.Register(Net.NetGUID, Entity);

                // The server tracks live stable ids so a runtime destroy can be despawned to clients.
                if (bAuthority)
                {
                    State.KnownStableGuids.Add(Net.NetGUID.Value);
                }
            }

            LOG_DISPLAY("[Net] Adopted {} stable networked entities ({})", Networked.Num(), bAuthority ? "authority" : "proxy");
        }

        template<typename TTag>
        void SyncTag(ECS::FRegistry& Registry, ECS::FEntity Entity, bool bWanted)
        {
            const bool bHas = Registry.HasAny<TTag>(Entity);
            if (bWanted && !bHas)
            {
                Registry.Emplace<TTag>(Entity);
            }
            else if (!bWanted && bHas)
            {
                Registry.Remove<TTag>(Entity);
            }
        }

        void RefreshNetRoles(ECS::FRegistry& Registry, const FNetWorldState& State, bool bServer)
        {
            LUMINA_PROFILE_SCOPE();
            Registry.View<ECS::Write<SNetworkComponent>>().ForEach([&](ECS::FEntity Entity, ECS::TMut<SNetworkComponent> NetMut)
            {
                const SNetworkComponent& Net = NetMut.Get();
                if (bServer)
                {
                    NetMut.SetIfChanged(&SNetworkComponent::LocalRole, ENetRole::Authority);
                    NetMut.SetIfChanged(&SNetworkComponent::RemoteRole, (Net.OwningConnectionId != 0) ? ENetRole::AutonomousProxy : ENetRole::SimulatedProxy);
                }
                else
                {
                    const bool bOwnedHere = (Net.OwningConnectionId != 0) && (Net.OwningConnectionId == State.LocalPeerId);
                    NetMut.SetIfChanged(&SNetworkComponent::LocalRole, bOwnedHere ? ENetRole::AutonomousProxy : ENetRole::SimulatedProxy);
                    NetMut.SetIfChanged(&SNetworkComponent::RemoteRole, ENetRole::Authority);
                }

                const bool bMirrored = !bServer && Net.LocalRole == ENetRole::SimulatedProxy;
                SyncTag<SNetProxyTag>(Registry, Entity, bMirrored);
                SyncTag<SNetDrivenTransformTag>(Registry, Entity, bMirrored && Net.bReplicatesMovement);
            });
        }

        // One message per client, since each only learns owners of entities it holds plus the ones it owns.
        void AppendOwnership(const FNetWorldState& State, uint32 Thread, uint32 Connection, const TVector<FNetOwnerRecord>& Changes,
                             const TVector<FNetOwnerRecord>* JoinerTable, TVector<uint8>& Reliable)
        {
            TVector<FNetOwnerRecord>& Picked = State.OwnershipPickedScratch[Thread];
            Picked.Reset();
            for (const FNetOwnerRecord& Record : Changes)
            {
                if (Record.Owner == Connection || State.HoldsEntity(Connection, Record.Guid))
                {
                    Picked.Add(Record);
                }
            }
            if (JoinerTable != nullptr)
            {
                Picked.Append(JoinerTable->begin(), JoinerTable->end());
            }

            Net::AppendCountedMessages(Reliable, ENetMessage::OwnershipUpdate, Picked.Num(), [&Picked](FNetArchive& Writer, int32 Index)
            {
                Net::WriteNetGuid(Writer, Picked[Index].Guid);
                WriteVarUInt(Writer, Picked[Index].Owner);
                return true;
            });
        }

        // Client, apply an ownership table. Roles refresh from it on the same tick.
        void ApplyOwnershipUpdate(ECS::FRegistry& Registry, FNetWorldState& State, const uint8* Data, SIZE_T Size)
        {
            FNetArchive Reader(Data, Size);
            uint8  Type  = 0;
            uint16 Count = 0;
            Reader << Type;
            Reader << Count;
            for (uint16 i = 0; i < Count; ++i)
            {
                const uint32 Guid  = Net::ReadNetGuid(Reader);
                const uint32 Owner = ReadVarUInt(Reader);
                if (Reader.HasError())
                {
                    break;
                }
                const ECS::FEntity Entity = State.GuidTable.Find(SNetGUID{ Guid });
                if (Entity != ECS::NullEntity && Registry.IsValid(Entity))
                {
                    SNetworkComponent* Net = Registry.TryGet<ECS::Write<SNetworkComponent>>(Entity);
                    if (Net != nullptr && Net->OwningConnectionId != Owner)
                    {
                        SNetOwnershipChangedEvent Changed;
                        Changed.Entity             = Entity;
                        Changed.PreviousOwner      = Net->OwningConnectionId;
                        Changed.OwningConnectionId = Owner;
                        Net->OwningConnectionId    = Owner;
                        ECS::SendEventIfRead(State.Events, Changed);
                    }
                }
            }
        }

        // Writes the components and script fields the last collect left in the replication scratch.
        void WritePropertyUpdate(ECS::FRegistry& Registry, FNetWorldState& State, ECS::FEntity Entity, uint32 Guid, TVector<uint8>& OutMsg)
        {
            FNetArchive Writer(OutMsg);
            Net::BindWriters(Writer, State);
            uint8 Type = static_cast<uint8>(ENetMessage::PropertyUpdate);
            Writer << Type;
            Net::WriteNetGuid(Writer, Guid);
            Net::WriteEntityComponents(Writer, Registry, Entity, &State.ReplicationScratch.Comps);
            Net::WriteEntityScripts(Writer, State.ReplicationScratch.Scripts);
        }

        // Seeds DiffState and appends a full PropertyUpdate to OutMsg, each only when given.
        void CollectBaseline(ECS::FRegistry& Registry, FNetWorldState& State, ECS::FEntity Entity, uint32 Guid,
            FComponentRepState* DiffState, TVector<uint8>* OutMsg)
        {
            TVector<Net::FComponentRepOut>& Comps   = State.ReplicationScratch.Comps;
            TVector<Net::FScriptRepOut>&    Scripts = State.ReplicationScratch.Scripts;
            Net::CollectComponentFieldsInto(Registry, Entity, State, /*bBaseline*/true, DiffState, Comps);
            Net::CollectScriptFieldsInto(Registry, Entity, State, /*bBaseline*/true, DiffState, Scripts);
            if (OutMsg != nullptr)
            {
                WritePropertyUpdate(Registry, State, Entity, Guid, *OutMsg);
            }
        }

        void WritePropertyResync(ECS::FRegistry& Registry, FNetWorldState& State, ECS::FEntity Entity, uint32 Guid, TVector<uint8>& OutMsg)
        {
            // No diff state, so the shared baseline every other client's deltas come from stays where it was.
            CollectBaseline(Registry, State, Entity, Guid, nullptr, nullptr);
            TVector<Net::FComponentRepOut>& Comps = State.ReplicationScratch.Comps;
            if (const FComponentRepState* Diff = Registry.TryGet<ECS::Read<FComponentRepState>>(Entity))
            {
                for (uint32 WireIndex : Diff->RemovedWireIndices)
                {
                    Net::FComponentRepOut& Removed = Comps.EmplaceGetRef();
                    Removed.WireIndex = WireIndex;
                    Removed.bRemoved  = true;
                    Removed.Block.Reset();
                }
            }
            WritePropertyUpdate(Registry, State, Entity, Guid, OutMsg);
        }

        // Carries the baseline so later updates send only changes, and is recipient-independent.
        void WriteSpawnMessage(ECS::FRegistry& Registry, FNetWorldState& State, ECS::FEntity Entity, uint32 Guid, uint32 Owner,
            const FVector3& Pos, const FQuat& Rot, const FVector3& Scale, TVector<uint8>& OutMsg)
        {
            FNetArchive Writer(OutMsg);
            Net::BindWriters(Writer, State);
            uint8 Type = static_cast<uint8>(ENetMessage::SpawnEntity);
            Writer << Type;
            Net::WriteNetGuid(Writer, Guid);
            WriteVarUInt(Writer, Owner);

            // The diff baselines are seeded here, so later updates carry only what changes after the spawn.
            CollectBaseline(Registry, State, Entity, Guid, &Registry.GetOrEmplace<FComponentRepState>(Entity), nullptr);

            Net::WriteSpawnComponents(Writer, Registry, State, Entity);
            Net::WriteScriptManifest(Writer, Registry, Entity);
            Net::WriteScriptStates(Writer, Registry, Entity);

            NetQuantize::FQuantizedVector::FromVector(Pos).Write(Writer);
            NetQuantize::FQuantizedQuat::FromQuat(Rot).Write(Writer);
            NetQuantize::FQuantizedVector::FromVector(Scale, NetQuantize::ScaleQuantum).Write(Writer);
        }

        void WriteDespawnMessage(uint32 Guid, TVector<uint8>& OutMsg)
        {
            FNetArchive Writer(OutMsg);
            uint8 Type = static_cast<uint8>(ENetMessage::DespawnEntity);
            Writer << Type;
            Net::WriteNetGuid(Writer, Guid);
        }

        // Runs on the game thread with synchronous signals, so the GuidTable mutation is safe.
        void OnNetworkComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            FNetWorldState* State = Registry.Ctx().Find<FNetWorldState>();
            if (State == nullptr)
            {
                return;
            }
            State->PendingAttach.Remove(static_cast<uint32>(Entity.Value));

            // A server's stable GUID is still despawned for clients, since ReplicateStableDespawns treats a missing entry as dead.
            const SNetworkComponent& Net = Registry.Get<ECS::Read<SNetworkComponent>>(Entity);
            if (State->GuidTable.Find(Net.NetGUID) == Entity)
            {
                State->GuidTable.Unregister(Net.NetGUID);
            }
        }

        // A client may have destroyed its copy locally, which leaves nothing to despawn and must not block a respawn.
        ECS::FEntity FindLiveEntity(ECS::FRegistry& Registry, const FNetWorldState& State, uint32 Guid)
        {
            const ECS::FEntity Entity = State.GuidTable.Find(SNetGUID{ Guid });
            return Registry.IsValid(Entity) ? Entity : ECS::NullEntity;
        }

        // Assigns NetGUIDs to newly-spawned dynamic entities, with destruction handled by the destroy hook.
        void MaintainDynamicLifetime(ECS::FRegistry& Registry, FNetWorldState& State)
        {
            LUMINA_PROFILE_SCOPE();
            TVector<ECS::FEntity> Minted;
            for (auto [Entity, Net] : Registry.View<ECS::Write<SNetworkComponent>>().Each())
            {
                if (Net.Get().bNetLoadOnClient && Net.Get().NetGUID.Value == 0)
                {
                    Net->NetGUID = State.GuidTable.AllocateDynamic();
                    State.GuidTable.Register(Net.Get().NetGUID, Entity);
                    Minted.Add(Entity);
                }
            }

            // A spawned prefab's children get their ids in the same tick as its root, so they spawn together.
            TVector<ECS::FEntity> Inherited;
            for (ECS::FEntity Entity : Minted)
            {
                InheritNetworking(Registry, Entity, &Inherited);
            }
            for (ECS::FEntity Entity : Inherited)
            {
                SNetworkComponent& Net = Registry.Get<ECS::Write<SNetworkComponent>>(Entity);
                if (Net.bNetLoadOnClient)
                {
                    Net.NetGUID = State.GuidTable.AllocateDynamic();
                    State.GuidTable.Register(Net.NetGUID, Entity);
                }
            }
        }
        
        void ReplicateStableDespawns(ECS::FRegistry& Registry, FNetWorldState& State, TVector<uint8>& Batch)
        {
            LUMINA_PROFILE_SCOPE();
            for (int32 i = 0; i < State.KnownStableGuids.Num(); )
            {
                const uint32 Guid = State.KnownStableGuids[i];
                const ECS::FEntity Entity = State.GuidTable.Find(SNetGUID{ Guid });
                if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
                {
                    TVector<uint8> Buffer;
                    WriteDespawnMessage(Guid, Buffer);
                    Net::AppendFramedMessage(Batch, Buffer.GetData(), static_cast<SIZE_T>(Buffer.Num()));
                    ++State.Stats.DespawnsSent;

                    State.GuidTable.Unregister(SNetGUID{ Guid });
                    State.DestroyedStableGuids.Add(Guid);
                    State.KnownStableGuids.RemoveAt(i);
                }
                else
                {
                    ++i;
                }
            }
        }

        // Diffs each dirty entity once for every client, then clears the tag. Each client's copy goes out in its property packets.
        void ReplicateDirtyProperties(ECS::FRegistry& Registry, FNetWorldState& State, double NowTime)
        {
            LUMINA_PROFILE_SCOPE();
            const int32 BudgetBytes = GetDefault<CNetworkSettings>()->MaxPropertyBytesPerTick;

            // Clearing the flag from entities that cannot replicate stops them re-evaluating every tick.
            struct FDirtyEntry { ECS::FEntity Entity; double LastTime; uint32 Guid; };
            TVector<FDirtyEntry> Dirty;
            {
                auto View = Registry.View<ECS::Read<SNetworkComponent>, FNetDirty>();
                for (ECS::FEntity Entity : View)
                {
                    const SNetworkComponent& Net = View.Get<ECS::Read<SNetworkComponent>>(Entity);
                    if (!Net.bReplicates || Net.NetGUID.Value == 0)
                    {
                        Registry.Remove<FNetDirty>(Entity);
                        continue;
                    }
                    const FComponentRepState* Cs = Registry.TryGet<ECS::Read<FComponentRepState>>(Entity);
                    Dirty.Emplace(Entity, Cs ? Cs->LastReplicatedTime : 0.0, Net.NetGUID.Value);
                }
            }

            // Oldest first with a GUID tie-break, so a backlog spreads across ticks without starving one.
            Algo::Sort(Dirty, [](const FDirtyEntry& A, const FDirtyEntry& B)
            {
                return (A.LastTime != B.LastTime) ? (A.LastTime < B.LastTime) : (A.Guid < B.Guid);
            });

            int32 BytesThisTick = 0;
            for (const FDirtyEntry& D : Dirty)
            {
                // Only sent entities Collect, so a deferred entity's change is never lost from the diff baseline.
                if (BudgetBytes > 0 && BytesThisTick >= BudgetBytes)
                {
                    break;
                }

                const ECS::FEntity Entity = D.Entity;
                const SNetworkComponent& Net = Registry.Get<ECS::Read<SNetworkComponent>>(Entity);

                // Diff the native replicated component fields once (recipient-independent); updates the snapshot.
                FComponentRepState& CompDiff = Registry.GetOrEmplace<FComponentRepState>(Entity);

                // Parked across entities so a tick's worth of updates reuses one set of buffers.
                TVector<Net::FComponentRepOut>& Comps   = State.ReplicationScratch.Comps;
                TVector<Net::FScriptRepOut>&    Scripts = State.ReplicationScratch.Scripts;
                TVector<uint8>&                 Buffer  = State.ReplicationScratch.Buffer;
                Net::CollectComponentFieldsInto(Registry, Entity, State, /*bBaseline*/false, &CompDiff, Comps);
                const bool bHeldBack = Net::CollectScriptFieldsInto(Registry, Entity, State, /*bBaseline*/false, &CompDiff, Scripts);
                // Waiting on a rate-limited field with nothing else to say, so this tick sends nothing at all.
                if (bHeldBack && Comps.IsEmpty() && Scripts.IsEmpty())
                {
                    continue;
                }

                // Written once and copied only to clients that hold the entity, which happens after relevancy runs.
                Buffer.Reset();
                WritePropertyUpdate(Registry, State, Entity, Net.NetGUID.Value, Buffer);
                const int32 Before = State.PendingPropertyBytes.Num();
                Net::AppendFramedMessage(State.PendingPropertyBytes, Buffer.GetData(), static_cast<SIZE_T>(Buffer.Num()));
                const uint32 Framed = static_cast<uint32>(State.PendingPropertyBytes.Num() - Before);
                State.PendingProperties.Emplace(Net.NetGUID.Value, static_cast<uint32>(Before), Framed);
                BytesThisTick += static_cast<int32>(Framed);

                CompDiff.LastReplicatedTime = NowTime; // mark sent -> drives the oldest-first fairness above
                if (!bHeldBack)
                {
                    Registry.Remove<FNetDirty>(Entity); // sent this tick; deferred entities keep the flag
                }
                ++State.Stats.PropertyUpdatesSent;
            }

            LUMINA_PROFILE_VALUE("Net/DirtyEntities",  Dirty.Num());
            LUMINA_PROFILE_VALUE("Net/DirtyPropBytes", BytesThisTick);
        }
        
        // On the host's copy of a kept stand-in, so the stand-in goes when the host's copy does.
        struct FNetStandIn
        {
            ECS::FEntity Entity = ECS::NullEntity;
        };

        uint32 BeginPredictedSpawn(FNetWorldState& State, ECS::FEntity StandIn, bool bKeepStandIn, double Expire)
        {
            const uint32 Key = State.NextPredictionKey;
            State.NextPredictionKey = Key == UINT32_MAX ? 1 : Key + 1;
            State.PredictedSpawns.Add(Key, FNetWorldState::FPredictedSpawn{ StandIn, Expire, bKeepStandIn });
            return Key;
        }

        void ResolvePredictedSpawn(ECS::FRegistry& Registry, FNetWorldState& State, ECS::FEntity Entity)
        {
            const SNetworkComponent& Net = Registry.Get<ECS::Read<SNetworkComponent>>(Entity);
            FNetWorldState::FPredictedSpawn Spawn;
            if (Net.PredictionKey == 0 || Net.PredictedBy != State.LocalPeerId || !State.PredictedSpawns.RemoveAndCopyValue(Net.PredictionKey, Spawn))
            {
                return;
            }

            SNetPredictedSpawnEvent Resolved;
            Resolved.StandIn       = Spawn.StandIn;
            Resolved.Authoritative = Entity;
            Resolved.Key           = Net.PredictionKey;
            Resolved.bConfirmed    = true;

            // A kept stand-in already shows this spawn here, even if it has since hit something, so the host's copy stays hidden.
            if (Spawn.bKeepStandIn)
            {
                Registry.Emplace<SDisabledTag>(Entity);
                if (Registry.IsValid(Spawn.StandIn))
                {
                    Registry.Emplace<FNetStandIn>(Entity).Entity = Spawn.StandIn;
                }
            }
            else if (Registry.IsValid(Spawn.StandIn))
            {
                ECS::Utils::DestroyEntity(Registry, Spawn.StandIn);
            }
            ECS::SendEventIfRead(State.Events, Resolved);
        }

        void ExpirePredictedSpawns(ECS::FRegistry& Registry, FNetWorldState& State, double Now)
        {
            for (auto It = State.PredictedSpawns.begin(); It != State.PredictedSpawns.end();)
            {
                if (It->Value.Expire > Now)
                {
                    ++It;
                    continue;
                }
                SNetPredictedSpawnEvent Refused;
                Refused.StandIn = It->Value.StandIn;
                Refused.Key     = It->Key;
                if (Registry.IsValid(It->Value.StandIn))
                {
                    ECS::Utils::DestroyEntity(Registry, It->Value.StandIn);
                }
                ECS::SendEventIfRead(State.Events, Refused);
                It = State.PredictedSpawns.Remove(It);
            }
        }

        // Client, create + link a server-spawned entity, then apply its components.
        void ApplySpawnEntity(ECS::FRegistry& Registry, FNetWorldState& State, uint32 SenderConn, const uint8* Data, SIZE_T Size)
        {
            LUMINA_PROFILE_SCOPE();
            FNetArchive Reader(Data, Size);
            uint8 Type = 0;
            Reader << Type;
            const uint32 Guid  = Net::ReadNetGuid(Reader);
            const uint32 Owner = ReadVarUInt(Reader);
            if (Reader.HasError() || FindLiveEntity(Registry, State, Guid) != ECS::NullEntity)
            {
                return; // malformed, or we already have this entity
            }

            Net::BindReaders(Reader, State, SenderConn);
            const ECS::FEntity Entity = Registry.Create();
            Net::ReadSpawnComponents(Reader, Registry, State, Entity);
            if (!Reader.HasError())
            {
                const uint32 ScriptCount = Net::ReadScriptManifest(Reader, Registry, Entity);
                Net::ReadScriptStates(Reader, Registry, Entity, ScriptCount);
            }

            // Spawn pose
            NetQuantize::FQuantizedVector QPos;
            QPos.Read(Reader);
            NetQuantize::FQuantizedQuat   QRot;
            QRot.Read(Reader);
            NetQuantize::FQuantizedVector QScale;
            QScale.Read(Reader);

            // A script's OnAttach ran while the states were read, and it may have destroyed what it was attached to.
            if (!Registry.IsValid(Entity))
            {
                return;
            }
            if (Reader.HasError())
            {
                ECS::Utils::DestroyEntity(Registry, Entity);
                return;
            }

            if (STransformComponent* T = Registry.TryGet<ECS::Write<STransformComponent>>(Entity))
            {
                T->SetRaw(QPos.ToVector(), QRot.ToQuat(), QScale.ToVector(NetQuantize::ScaleQuantum));
            }

            // A spawned tag arrives as the component alone, and lookups read the per-tag storage it belongs in.
            if (const STagComponent* Tag = Registry.TryGet<ECS::Read<STagComponent>>(Entity))
            {
                const FName TagName = Tag->Tag;
                ECS::Utils::SetEntityTag(Registry, Entity, TagName);
            }

            if (!Registry.IsValid(Entity))
            {
                return;
            }
            SNetworkComponent& Net = Registry.GetOrEmplace<SNetworkComponent>(Entity);
            Net.NetGUID            = SNetGUID{ Guid };
            Net.OwningConnectionId = Owner;
            State.GuidTable.Register(Net.NetGUID, Entity);
            Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
            // Now that this entity's NetGUID is registered, reparent any children that were waiting on it.
            Net::DrainPendingAttach(Registry, State, Guid, Entity);

            SNetEntitySpawnedEvent Spawned;
            Spawned.Entity             = Entity;
            Spawned.NetGUID            = Guid;
            Spawned.OwningConnectionId = Owner;
            ECS::SendEventIfRead(State.Events, Spawned);

            ResolvePredictedSpawn(Registry, State, Entity);
        }

        // False when the entity has not spawned here yet or a reference's export has not arrived, so the host should send it again.
        bool ApplyPropertyUpdate(ECS::FRegistry& Registry, FNetWorldState& State, uint32 SenderConn, const uint8* Data, SIZE_T Size)
        {
            LUMINA_PROFILE_SCOPE();
            FNetArchive Reader(Data, Size);
            uint8 Type = 0;
            Reader << Type;
            const uint32 Guid = Net::ReadNetGuid(Reader);
            if (Reader.HasError())
            {
                return true;
            }
            const ECS::FEntity Entity = State.GuidTable.Find(SNetGUID{ Guid });
            if (Entity == ECS::NullEntity)
            {
                return false;
            }
            if (!Registry.IsValid(Entity))
            {
                return true;
            }

            Net::BindReaders(Reader, State, SenderConn);
            Net::ReadEntityComponents(Reader, Registry, State, Entity);
            if (!Reader.HasError() && Registry.IsValid(Entity))
            {
                Net::ReadEntityScripts(Reader, Registry, Entity);
            }
            if (Registry.IsValid(Entity))
            {
                Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);

                SNetReplicatedEvent Replicated;
                Replicated.Entity = Entity;
                ECS::SendEventIfRead(State.Events, Replicated);
            }
            return !Reader.bUnresolvedReference;
        }

        void ApplyPropertyPacket(ECS::FRegistry& Registry, FNetWorldState& State, uint32 SenderConn, const uint8* Data, SIZE_T Size)
        {
            uint32 Seq = 0;
            const SIZE_T Start = NetProperties::ReadPacketHeader(Data, Size, Seq);
            if (Start == 0 || Seq < State.PropertyAcks.Floor)
            {
                return;
            }

            // Left unacknowledged when anything in it could not land, so the host sends those entities' current state again.
            bool bLanded = true;
            Net::ForEachFramedMessage(Data + Start, Size - Start, [&](const uint8* Msg, SIZE_T MsgSize)
            {
                if (MsgSize > 0 && static_cast<ENetMessage>(Msg[0]) == ENetMessage::PropertyUpdate)
                {
                    bLanded &= ApplyPropertyUpdate(Registry, State, SenderConn, Msg, MsgSize);
                }
            });
            if (bLanded)
            {
                NetProperties::NoteReceived(State.PropertyAcks, Seq);
            }
        }

        // Client, destroy a despawned entity.
        void ApplyDespawnEntity(ECS::FRegistry& Registry, FNetWorldState& State, const uint8* Data, SIZE_T Size)
        {
            FNetArchive Reader(Data, Size);
            uint8 Type = 0;
            Reader << Type;
            const uint32 Guid = Net::ReadNetGuid(Reader);
            if (Reader.HasError())
            {
                return;
            }
            const ECS::FEntity Entity = FindLiveEntity(Registry, State, Guid);
            State.GuidTable.Unregister(SNetGUID{ Guid });
            if (Entity != ECS::NullEntity)
            {
                SNetEntityDespawnedEvent Despawned;
                Despawned.Entity  = Entity;
                Despawned.NetGUID = Guid;
                ECS::SendEventIfRead(State.Events, Despawned);

                if (const FNetStandIn* StandIn = Registry.TryGet<ECS::Read<FNetStandIn>>(Entity); StandIn != nullptr && Registry.IsValid(StandIn->Entity))
                {
                    ECS::Utils::DestroyEntity(Registry, StandIn->Entity);
                }
                ECS::Utils::DestroyEntity(Registry, Entity);
            }
        }
        
        void MarkReplicatedChanges(ECS::FRegistry& Registry, FNetWorldState& State)
        {
            LUMINA_PROFILE_SCOPE();
            const ECS::FSparseSet* Networked = Registry.FindStorage(static_cast<uint32>(ECS::GetComponentTypeID<SNetworkComponent>()));
            const uint32 Since = State.ReplicatedChangeTick;
            State.ReplicatedChangeTick = ECS::FSparseSet::AdvanceChangeTick();
            if (Networked == nullptr || Networked->GetDenseSize() == 0)
            {
                return;
            }

            const TVector<int32>& TypeIdToIndex = State.ReplTypes.TypeIdToIndex;
            if (State.ReplicatedWriteEpochs.Num() < TypeIdToIndex.Num())
            {
                State.ReplicatedWriteEpochs.SetNum(TypeIdToIndex.Num(), ~0ull);
            }

            TVector<ECS::FEntity>& Changed = State.ReplicatedChangeScratch;
            Changed.Reset();
            for (int32 TypeID = 0; TypeID < TypeIdToIndex.Num(); ++TypeID)
            {
                ECS::FSparseSet* Pool = TypeIdToIndex[TypeID] >= 0 ? Registry.FindStorage(static_cast<uint32>(TypeID)) : nullptr;
                if (Pool == nullptr)
                {
                    continue;
                }
                // Tracking stamps the whole pool at once, so the first scan of a pool sends everything it holds.
                if (!Pool->IsTrackingChanges())
                {
                    Pool->TrackChanges();
                }
                const uint64 Epoch = Pool->GetWriteEpoch();
                if (State.ReplicatedWriteEpochs[TypeID] == Epoch)
                {
                    continue;
                }
                State.ReplicatedWriteEpochs[TypeID] = Epoch;

                const ECS::FEntity* Dense = Pool->GetDenseData();
                const uint32*       Ticks = Pool->GetChangeTickData();
                for (int32 Index = 0; Index < Pool->GetDenseSize(); ++Index)
                {
                    const ECS::FEntity Entity = Dense[Index];
                    if (!Entity.IsTombstone() && ECS::FSparseSet::IsNewerTick(Ticks[Index], Since)
                        && Networked->Contains(Entity))
                    {
                        Changed.Add(Entity);
                    }
                }
            }

            for (ECS::FEntity Entity : Changed)
            {
                if (!Registry.HasAll<FNetDirty>(Entity))
                {
                    Registry.Emplace<FNetDirty>(Entity);
                }
            }
        }

        void WriteTearOffMessage(uint32 Guid, TVector<uint8>& OutMsg)
        {
            FNetArchive Writer(OutMsg);
            uint8 Type = static_cast<uint8>(ENetMessage::TearOffEntity);
            Writer << Type;
            Net::WriteNetGuid(Writer, Guid);
        }

        // Leaves Entity a plain local entity, its solver body back to the motion it was authored with if a proxy had frozen it.
        void StripNetworking(ECS::FRegistry& Registry, CWorld* World, FNetWorldState& State, ECS::FEntity Entity)
        {
            const SNetworkComponent& Net = Registry.Get<ECS::Read<SNetworkComponent>>(Entity);
            Physics::IPhysicsScene* PhysScene = World != nullptr ? World->GetPhysicsScene() : nullptr;
            const SRigidBodyComponent* Body = Registry.TryGet<ECS::Read<SRigidBodyComponent>>(Entity);
            if (Net.bProxyPhysicsConfigured && PhysScene != nullptr && Body != nullptr
                && PhysScene->GetBodyStatus(Entity) == Physics::EPhysicsBodyStatus::Ready)
            {
                PhysScene->ChangeBodyMotionType(Entity, Body->BodyType);
            }

            Registry.Remove<SNetworkComponent, FRepTransform, FComponentRepState, FNetDirty, FScriptSyncState, FNetPredictionClient,
                            FNetPredictionServer, FNetCharacterDrive, SNetProxyTag, SNetDrivenTransformTag>(Entity);
            Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);

            SNetEntityTornOffEvent TornOff;
            TornOff.Entity = Entity;
            ECS::SendEventIfRead(State.Events, TornOff);
        }

        void CollectNetworkedHierarchy(ECS::FRegistry& Registry, ECS::FEntity Entity, TVector<ECS::FEntity>& Out)
        {
            if (Registry.HasAll<SNetworkComponent>(Entity) && !Out.Contains(Entity))
            {
                Out.Add(Entity);
            }
            Registry.GetHierarchy().ForEachChild(Entity, [&](ECS::FEntity Child)
            {
                CollectNetworkedHierarchy(Registry, Child, Out);
            });
        }

        // Ahead of any replication this tick, so nothing more is sent for a torn-off entity and no client despawns its copy.
        void ProcessTearOffs(CWorld* World, ECS::FRegistry& Registry, FNetWorldState& State)
        {
            if (State.PendingTearOffs.IsEmpty())
            {
                return;
            }

            TVector<ECS::FEntity> Torn;
            for (ECS::FEntity Root : State.PendingTearOffs)
            {
                if (Registry.IsValid(Root))
                {
                    CollectNetworkedHierarchy(Registry, Root, Torn);
                }
            }
            State.PendingTearOffs.Reset();

            for (ECS::FEntity Entity : Torn)
            {
                const uint32 Guid = Registry.Get<ECS::Read<SNetworkComponent>>(Entity).NetGUID.Value;
                if (Guid != 0)
                {
                    TVector<uint8> Msg;
                    WriteTearOffMessage(Guid, Msg);
                    for (auto& [Connection, View] : State.ClientViews)
                    {
                        if (View.Relevant.Remove(Guid) > 0)
                        {
                            Net::AppendFramedMessage(View.PendingReliable, Msg.GetData(), static_cast<SIZE_T>(Msg.Num()));
                        }
                    }
                    State.KnownStableGuids.RemoveSingleSwap(Guid);
                    if (State.GuidTable.Find(SNetGUID{ Guid }) == Entity)
                    {
                        State.GuidTable.Unregister(SNetGUID{ Guid });
                    }
                }
                StripNetworking(Registry, World, State, Entity);
            }
        }

        void ApplyTearOff(ECS::FRegistry& Registry, CWorld* World, FNetWorldState& State, const uint8* Data, SIZE_T Size)
        {
            FNetArchive Reader(Data, Size);
            uint8 Type = 0;
            Reader << Type;
            const uint32 Guid = Net::ReadNetGuid(Reader);
            if (Reader.HasError())
            {
                return;
            }
            const ECS::FEntity Entity = FindLiveEntity(Registry, State, Guid);
            State.GuidTable.Unregister(SNetGUID{ Guid });
            if (Entity != ECS::NullEntity && Registry.HasAll<SNetworkComponent>(Entity))
            {
                StripNetworking(Registry, World, State, Entity);
            }
        }

        void ConfigureProxyPhysics(ECS::FRegistry& Registry, CWorld* World)
        {
            Physics::IPhysicsScene* PhysScene = World->GetPhysicsScene();
            if (PhysScene == nullptr)
            {
                return;
            }

            Registry.View<ECS::Write<SNetworkComponent>>().ForEach([&](ECS::FEntity Entity, ECS::TMut<SNetworkComponent> NetMut)
            {
                const SNetworkComponent& Current = NetMut.Get();
                const bool bIsProxy = Current.LocalRole == ENetRole::SimulatedProxy || Current.LocalRole == ENetRole::AutonomousProxy;
                if (Current.bProxyPhysicsConfigured || !bIsProxy)
                {
                    return;
                }

                SNetworkComponent& Net = NetMut.GetMut();
                // A character has no solver body to freeze, since the network drives its capsule directly.
                if (Registry.HasAny<SCharacterPhysicsComponent>(Entity))
                {
                    Net.bProxyPhysicsConfigured = true;
                    return;
                }

                const Physics::EPhysicsBodyStatus Status = PhysScene->GetBodyStatus(Entity);
                if (Status == Physics::EPhysicsBodyStatus::Ready)
                {
                    PhysScene->ChangeBodyMotionType(Entity, EBodyType::Kinematic);
                    Net.bProxyPhysicsConfigured = true;
                }
                else if (Status == Physics::EPhysicsBodyStatus::Missing || Status == Physics::EPhysicsBodyStatus::Failed)
                {
                    Net.bProxyPhysicsConfigured = !Registry.HasAny<SRigidBodyComponent>(Entity);
                }
            });
        }

        // Quantum is coarser the farther away and both peers derive it from the wire tier; the anchor rides once per snapshot, so 1 cm stays cheap.
        constexpr double SnapshotAnchorQuantum = 0.01;

        double TierPosQuantum(ENetLODTier Tier)
        {
            switch (Tier)
            {
            case ENetLODTier::Near: return 0.001;
            case ENetLODTier::Mid:  return 0.01;
            default:                return 0.1;
            }
        }

        constexpr uint32 YawBits    = 12;
        constexpr uint32 FarYawBits = 8;

        // An upright pose turns about Y alone, which covers characters and most props on the ground.
        bool IsYawOnly(const FQuat& Rot)
        {
            return Math::Abs(Rot.x) < 1.0e-3f && Math::Abs(Rot.z) < 1.0e-3f;
        }

        void WriteYaw(FNetArchive& Writer, const FQuat& Rot, uint32 Bits)
        {
            float Norm = Math::YawFirstEulerAngles(Rot).y / Math::TwoPi<float>() + 0.5f;
            Norm -= std::floor(Norm);
            uint32 Packed = static_cast<uint32>(Norm * static_cast<float>(1u << Bits) + 0.5f) & ((1u << Bits) - 1u);
            Writer.SerializeBits(&Packed, Bits);
        }

        FQuat ReadYaw(FNetArchive& Reader, uint32 Bits)
        {
            uint32 Packed = 0;
            Reader.SerializeBits(&Packed, Bits);
            const float Yaw = (static_cast<float>(Packed) / static_cast<float>(1u << Bits) - 0.5f) * Math::TwoPi<float>();
            return FQuat(FVector3(0.0f, Yaw, 0.0f));
        }

        void WriteTransformRecord(FNetArchive& Writer, const FTransformSendRecord& R, const FVector3& Anchor)
        {
            uint8 Tier = static_cast<uint8>(R.Tier);
            Writer.SerializeBits(&Tier, 2);
            Net::WriteNetGuid(Writer, R.Guid);

            // Relative to the receiver's own position the numbers stay small, but an attached child's local pose does not.
            const double Quantum = TierPosQuantum(R.Tier);
            const NetQuantize::FQuantizedVector Relative = NetQuantize::FQuantizedVector::FromVector(R.Pos - Anchor, Quantum);
            const NetQuantize::FQuantizedVector Absolute = NetQuantize::FQuantizedVector::FromVector(R.Pos, Quantum);
            auto Magnitude = [](const NetQuantize::FQuantizedVector& V) { return std::llabs(V.X) + std::llabs(V.Y) + std::llabs(V.Z); };
            bool bAnchored = Magnitude(Relative) < Magnitude(Absolute);
            Writer.SerializeBit(bAnchored);
            (bAnchored ? Relative : Absolute).Write(Writer);

            if (R.Tier == ENetLODTier::Far)
            {
                // Far entities need no full orientation fidelity, so only the heading rides.
                WriteYaw(Writer, R.Rot, FarYawBits);
            }
            else
            {
                bool bYawOnly = IsYawOnly(R.Rot);
                Writer.SerializeBit(bYawOnly);
                if (bYawOnly)
                {
                    WriteYaw(Writer, R.Rot, YawBits);
                }
                else
                {
                    NetQuantize::FQuantizedQuat::FromQuat(R.Rot).Write(Writer);
                }
            }

            bool bScale = R.bScale;
            Writer.SerializeBit(bScale); // 1-bit has-scale flag
            if (bScale)
            {
                NetQuantize::FQuantizedVector::FromVector(R.Scale, NetQuantize::ScaleQuantum).Write(Writer);
            }

            bool bPitch = R.bPitch;
            Writer.SerializeBit(bPitch);
            if (bPitch)
            {
                uint8 Packed = static_cast<uint8>(R.Pitch);
                Writer.SerializeBits(&Packed, 8);
            }
        }

        constexpr SIZE_T TransformHeaderBytes = 7 + 30;
        constexpr SIZE_T MaxRecordBytes       = 5 + 30 + 6 + 1 + 30;
        constexpr int32  MaxRecordsPerFrame   = (Net::MaxFramedMessageSize - TransformHeaderBytes) / MaxRecordBytes;
        static_assert(MaxRecordsPerFrame >= 1 && MaxRecordsPerFrame <= 0xFFFF, "frame chunk size invalid");

        // Frames Records as messages of at most MaxRecordsPerFrame, each led by WriteHeader, and returns the largest message size.
        uint32 AppendTransformFrames(TVector<uint8>& Batch, TVector<uint8>& Scratch, const TVector<FTransformSendRecord>& Records, const FVector3& Anchor,
                                     TFunctionRef<void(FNetArchive&, uint16)> WriteHeader)
        {
            uint32 LargestFrame = 0;
            for (int32 Begin = 0; Begin < Records.Num(); Begin += MaxRecordsPerFrame)
            {
                const int32 End = Math::Min(Records.Num(), Begin + MaxRecordsPerFrame);
                Scratch.Reset();
                FNetArchive Writer(Scratch);
                WriteHeader(Writer, static_cast<uint16>(End - Begin));
                for (int32 i = Begin; i < End; ++i) { WriteTransformRecord(Writer, Records[i], Anchor); }
                LargestFrame = Math::Max(LargestFrame, static_cast<uint32>(Scratch.Num()));
                Net::AppendFramedMessage(Batch, Scratch.GetData(), static_cast<SIZE_T>(Scratch.Num()));
            }
            return LargestFrame;
        }

        // Returns true to send, and bOutScale says whether scale must ride along on the wire.
        bool PrepareTransformSend(SNetworkComponent& Net, FRepTransform& Rep, const FVector3& Pos, const FQuat& Rot, const FVector3& Scale, float DeltaTime, bool bForceResend, bool& bOutScale)
        {
            Rep.TimeSinceLastSend += DeltaTime;

            const NetQuantize::FQuantizedVector QPos   = NetQuantize::FQuantizedVector::FromVector(Pos);
            const NetQuantize::FQuantizedQuat   QRot   = NetQuantize::FQuantizedQuat::FromQuat(Rot);
            const NetQuantize::FQuantizedVector QScale = NetQuantize::FQuantizedVector::FromVector(Scale, NetQuantize::ScaleQuantum);

            const bool bPoseChanged  = !Rep.bSendCacheValid || QPos != Rep.LastSentPos || QRot != Rep.LastSentRot;
            const bool bScaleChanged = !Rep.bSendCacheValid || QScale != Rep.LastSentScale;

            bOutScale = false;
            if (!bForceResend)
            {
                if (Rep.bSendCacheValid && !bPoseChanged && !bScaleChanged)
                {
                    return false; // unchanged since last send
                }
                const float Interval = (Net.NetUpdateFrequency > 0.0f) ? (1.0f / Net.NetUpdateFrequency) : 0.0f;
                if (Rep.TimeSinceLastSend < Interval)
                {
                    return false; // throttled
                }
            }

            // Send scale only when it actually changed, or on a baseline/keyframe (first send included).
            bOutScale = bScaleChanged || bForceResend || !Rep.bSendCacheValid;

            Rep.LastSentPos       = QPos;
            Rep.LastSentRot       = QRot;
            Rep.LastSentScale     = QScale;
            Rep.bSendCacheValid   = true;
            Rep.TimeSinceLastSend = 0.0f;
            return true;
        }

        // Runs on the exclusive network system, so structural changes are safe here.
        void EnsureRepTransforms(ECS::FRegistry& Registry)
        {
            LUMINA_PROFILE_SCOPE();
            Registry.View<ECS::Read<SNetworkComponent>>().ForEach([&](ECS::FEntity Entity, const SNetworkComponent& Net)
            {
                const bool bQualifies = Net.bReplicates && Net.bReplicatesMovement && Net.bNetLoadOnClient
                    && Registry.HasAll<STransformComponent>(Entity);
                const bool bHas = Registry.HasAll<FRepTransform>(Entity);

                if (bQualifies && !bHas)
                {
                    Registry.Emplace<FRepTransform>(Entity).NetGUID = Net.NetGUID.Value;
                }
                else if (!bQualifies && bHas)
                {
                    Registry.Remove<FRepTransform>(Entity);
                }
                else if (bQualifies)
                {
                    Registry.Get<ECS::Write<FRepTransform>>(Entity).NetGUID = Net.NetGUID.Value;
                }
            });
        }
        
        void ServerReplicateRelevant(ECS::FRegistry& Registry, FNetWorldState& State,
            const SDefaultWorldSettings& Settings, float DeltaTime,
            float ServerTime, TVector<uint8>& ReliableBroadcast)
        {
            LUMINA_PROFILE_SCOPE();
            double PhaseStart = PlatformTime::Seconds();
            auto PhaseMark = [&PhaseStart, &State](ENetPhase Phase)
            {
                const double Now = PlatformTime::Seconds();
                const double Ms  = (Now - PhaseStart) * 1000.0;
                State.PhaseMsSum[static_cast<uint32>(Phase)] += Ms;
                State.PhaseMsMax[static_cast<uint32>(Phase)] = Math::Max(State.PhaseMsMax[static_cast<uint32>(Phase)], Ms);
                PhaseStart = Now;
            };
            ++State.RelevancyTick; // relevancy generation stamp for this tick
            NetGraph::BuildExtract(Registry, State.Extract, State.OwnerToRecord);
            NetGraph::BuildGrid(State.Extract, Settings, State.Grid);

            State.AlwaysRelevantRecords.Reset();
            for (uint32 Rec = 0; Rec < State.Extract.Num(); ++Rec)
            {
                if (State.Extract.Flags[Rec] & NETREC_AlwaysRelevant)
                {
                    State.AlwaysRelevantRecords.Add(Rec);
                }
            }

            PhaseMark(ENetPhase::Extract);
            const FNetExtract& Ex   = State.Extract;
            const FNetGrid&    Grid = State.Grid;
            const float Grace      = Settings.RelevancyGraceSeconds;
            const float EnterR2    = Settings.AOIEnterRadius * Settings.AOIEnterRadius;
            const int32 CellRadius = static_cast<int32>(Settings.AOILeaveRadius / Grid.CellSize) + 1;

            const uint32 NumClients = static_cast<uint32>(State.ConnectedClientIds.Num());
            TVector<TVector<uint8>>                ClientReliable; ClientReliable.SetNum(NumClients);
            TVector<TVector<FTransformSendRecord>> ClientRecords;  ClientRecords.SetNum(NumClients);

            uint32 RelevantSum = 0, RelevantMax = 0, SpawnsSum = 0, DespawnsSum = 0;

            // Message building mints shared export indices, so the parallel pass only records what each client is owed.
            enum class EClientOp : uint8 { Spawn, MapBaseline, Despawn };
            struct FClientOp { EClientOp Op; uint32 Guid; uint32 Rec; };
            TVector<TVector<FClientOp>> ClientOps;      ClientOps.SetNum(NumClients);
            TVector<uint32>             ClientRelevant; ClientRelevant.SetNum(NumClients);

            TVector<FNetClientView*> Views;
            Views.Reserve(NumClients);
            for (uint32 ConnId : State.ConnectedClientIds)
            {
                Views.Add(&State.ClientViews[ConnId]);
            }

            const CNetworkSettings* NetSettings = GetDefault<CNetworkSettings>();
            const float  SendRate     = NetSettings != nullptr ? NetSettings->ServerSendRate : 30.0f;
            static const TOptional<int> MaxRelevantArg = Global::GCommandLine->GetInt("maxrelevant");
            static const TOptional<int> SnapshotBytesArg = Global::GCommandLine->GetInt("snapshotbudget");
            const int32 SnapshotBytesPerSecond = SnapshotBytesArg.Get(NetSettings != nullptr ? NetSettings->MaxSnapshotBytesPerClient : 0);

            // A record runs about ten bytes once anchored and yaw-packed, which turns a byte budget into a record count.
            constexpr float ApproxRecordBytes = 10.0f;
            constexpr float MinResidencySeconds = 2.0f;
            const size_t MaxRecordsPerSend = (SnapshotBytesPerSecond > 0 && SendRate > 0.0f)
                ? static_cast<size_t>(Math::Max(1.0f, SnapshotBytesPerSecond / SendRate / ApproxRecordBytes)) : 0;
            const int32 MaxRelevant  = static_cast<size_t>(Math::Max(MaxRelevantArg.Get(NetSettings != nullptr ? NetSettings->MaxRelevantPerClient : 0), 0));
            const double SendInterval = SendRate > 0.0f ? 1.0 / SendRate : 0.0;
            TVector<uint8>    Serviced;     Serviced.SetNum(NumClients);
            TVector<FVector3> ClientAnchor; ClientAnchor.SetNum(NumClients, FVector3(0.0f));
            TVector<float> ClientDelta; ClientDelta.SetNum(NumClients);
            for (uint32 ci = 0; ci < NumClients; ++ci)
            {
                FNetClientView& View = *Views[ci];
                if (View.NextServiceTime < 0.0)
                {
                    // A golden-ratio offset per connection spreads joiners evenly over the interval.
                    const double Phase = std::fmod(State.ConnectedClientIds[ci] * 0.6180339887, 1.0);
                    View.NextServiceTime = ServerTime + SendInterval * Phase;
                }
                const bool bDue = SendInterval <= 0.0 || View.bForceBaseline || ServerTime >= View.NextServiceTime;
                Serviced[ci] = bDue ? 1 : 0;
                if (!bDue)
                {
                    continue;
                }
                ClientDelta[ci] = View.LastServiceTime < 0.0 ? DeltaTime : static_cast<float>(ServerTime - View.LastServiceTime);
                View.LastServiceTime = ServerTime;
                View.NextServiceTime = Math::Max(View.NextServiceTime + SendInterval, static_cast<double>(ServerTime));
            }

            PhaseMark(ENetPhase::Schedule);
            // One scratch slot per worker, sized before the parallel pass so no two clients ever share a buffer.
            const uint32 NumThreadSlots = Jobs::GetNumThreadSlots();
            if (static_cast<uint32>(State.GatheredScratch.Num()) < NumThreadSlots) { State.GatheredScratch.SetNum(NumThreadSlots); }
            if (static_cast<uint32>(State.DueScratch.Num())      < NumThreadSlots) { State.DueScratch.SetNum(NumThreadSlots); }

            // Phase A, the per-client gather, relevancy diff and transform records, run across workers.
            Jobs::ParallelFor(NumClients, [&](uint32 ci, uint32 Thread)
            {
                const uint32 ConnId = State.ConnectedClientIds[ci];
                FNetClientView& CV  = *Views[ci];
                ClientRelevant[ci] = static_cast<uint32>(CV.Relevant.Num());
                if (!Serviced[ci])
                {
                    return;
                }
                const float SinceServiced = ClientDelta[ci];
                TVector<FClientOp>& Ops = ClientOps[ci];
                TVector<FNetGatheredRecord>& Gathered = State.GatheredScratch[Thread];

                // A client without a pawn yet still receives everything that is relevant everywhere.
                const auto VpIt = State.OwnerToRecord.Find(ConnId);
                const bool bHasViewpoint = VpIt != nullptr;
                const FVector3 VP = bHasViewpoint ? Ex.WorldPos[(*VpIt)] : FVector3(0.0f);
                ClientAnchor[ci] = VP;

                Gathered.Reset();
                int32 Vcx = 0, Vcz = 0;
                if (bHasViewpoint)
                {
                    Grid.CellCoords(VP, Vcx, Vcz);
                    for (int32 cz = Vcz - CellRadius; cz <= Vcz + CellRadius; ++cz)
                    {
                        if (cz < 0 || cz >= Grid.DimZ) { continue; }
                        for (int32 cx = Vcx - CellRadius; cx <= Vcx + CellRadius; ++cx)
                        {
                            if (cx < 0 || cx >= Grid.DimX) { continue; }
                            const int32 Cell = Grid.CellIndex(cx, cz);
                            for (int32 s = Grid.CellStart[Cell]; s < Grid.CellStart[Cell + 1]; ++s)
                            {
                                const FVector3& P = Grid.SortedWorldPos[s];
                                const float DX = P.x - VP.x;
                                const float DZ = P.z - VP.z;
                                ENetLODTier Tier  = NetGraph::TierForDistanceSq(DX * DX + DZ * DZ, Settings);
                                const uint8 Flags = Grid.SortedFlags[s];
                                if (Tier == ENetLODTier::Cull)
                                {
                                    if (!(Flags & NETREC_AlwaysRelevant)) { continue; }
                                    Tier = ENetLODTier::Far;
                                }
                                Gathered.Emplace(Grid.SortedRecords[s], Grid.SortedGuid[s], Flags, Tier);
                            }
                        }
                    }
                }

                // The cell window above only reaches nearby cells, so distant always-relevant records join here.
                for (uint32 Rec : State.AlwaysRelevantRecords)
                {
                    ENetLODTier Tier = ENetLODTier::Near;
                    if (bHasViewpoint)
                    {
                        int32 Rcx, Rcz;
                        Grid.CellCoords(Ex.WorldPos[Rec], Rcx, Rcz);
                        const int32 OffX = Rcx > Vcx ? Rcx - Vcx : Vcx - Rcx;
                        const int32 OffZ = Rcz > Vcz ? Rcz - Vcz : Vcz - Rcz;
                        if (OffX <= CellRadius && OffZ <= CellRadius)
                        {
                            continue;
                        }
                        const float DX = Ex.WorldPos[Rec].x - VP.x;
                        const float DZ = Ex.WorldPos[Rec].z - VP.z;
                        Tier = NetGraph::TierForDistanceSq(DX * DX + DZ * DZ, Settings);
                        if (Tier == ENetLODTier::Cull)
                        {
                            Tier = ENetLODTier::Far;
                        }
                    }
                    Gathered.Emplace(Rec, Ex.Guid[Rec], Ex.Flags[Rec], Tier);
                }

                // A crowd keeps only the nearest entities, so a full town costs a client a bounded stream.
                if (MaxRelevant > 0 && Gathered.Num() > MaxRelevant)
                {
                    const uint32 OwnRecord = bHasViewpoint ? (*VpIt) : Constants::kIndexNoneU32;
                    auto ScoreOf = [&](const FNetGatheredRecord& G)
                    {
                        if ((G.Flags & NETREC_AlwaysRelevant) || G.Rec == OwnRecord)
                        {
                            return -1.0f;
                        }
                        const FVector3& P = Ex.WorldPos[G.Rec];
                        const float DistanceSq = (P.x - VP.x) * (P.x - VP.x) + (P.z - VP.z) * (P.z - VP.z);
                        // A held entity leaves only once it is well past a newcomer, and never in its first seconds, so a crowd does not churn spawns.
                        const auto Held = CV.Relevant.Find(G.Guid);
                        if (Held == nullptr)
                        {
                            return DistanceSq;
                        }
                        return Held->TimeHeld < MinResidencySeconds ? 0.0f : DistanceSq * 0.36f;
                    };
                    for (FNetGatheredRecord& G : Gathered)
                    {
                        G.Score = ScoreOf(G);
                    }
                    Algo::NthElement(Gathered.begin(), Gathered.begin() + MaxRelevant, Gathered.end(),
                        [](const FNetGatheredRecord& A, const FNetGatheredRecord& B) { return A.Score < B.Score; });
                    Gathered.SetNum(MaxRelevant);
                }

                // An entry is "relevant this tick" iff its RelevantTick == State.RelevancyTick.
                TVector<FTransformSendRecord>& Records  = ClientRecords[ci];

                TVector<FNetDueRecord>& Due = State.DueScratch[Thread];
                Due.Reset();

                // A single pass doing the relevancy diff on enter and the transform-record build.
                for (const FNetGatheredRecord& G : Gathered)
                {
                    const uint32 Rec   = G.Rec;
                    const uint32 Guid  = G.Guid;
                    const uint8  Flags = G.Flags;
                    const bool   bDyn  = (Flags & NETREC_Dynamic) != 0;

                    FRelevantEntry* EntryPtr;
                    auto It = CV.Relevant.Find(Guid);
                    if (It == nullptr)
                    {
                        // A new entity enters at the enter radius, an existing one stays to the leave radius.
                        if (!(Flags & NETREC_AlwaysRelevant))
                        {
                            const FVector3& P = Ex.WorldPos[Rec];
                            const float DDX = P.x - VP.x;
                            const float DDZ = P.z - VP.z;
                            if (DDX * DDX + DDZ * DDZ > EnterR2) { continue; }
                        }
                        FRelevantEntry E;
                        E.Tier         = G.Tier;
                        E.RelevantTick = State.RelevancyTick;
                        E.bDynamic     = bDyn;
                        E.TimeOutOfAOI = 0.0f;
                        if (bDyn)
                        {
                            if (State.GuidTable.Find(SNetGUID{ Guid }) != ECS::NullEntity)
                            {
                                Ops.Emplace(EClientOp::Spawn, Guid, Rec);
                                E.bBaselinePending = true; // spawn carried the pose; hold the transform one tick
                                E.SentSeq          = Ex.Seq[Rec];
                            }
                        }
                        else
                        {
                            E.bNeedsBaseline = true; // the client has it from the map, so send the current pose once
                            Ops.Emplace(EClientOp::MapBaseline, Guid, Rec);
                        }
                        EntryPtr = &CV.Relevant.FindOrAdd(Guid, E); // used immediately, before any rehash
                    }
                    else
                    {
                        It->RelevantTick = State.RelevancyTick;
                        It->Tier         = G.Tier;
                        It->TimeOutOfAOI = 0.0f;
                        It->TimeHeld    += SinceServiced;
                        EntryPtr = &(*It);
                    }

                    // Transform stream (movement entities only; a non-movement pose rode the spawn baseline).
                    if (Flags & NETREC_Movement)
                    {
                        FRelevantEntry& E = *EntryPtr;
                        E.TimeSinceSent += SinceServiced;
                        if (E.bBaselinePending)
                        {
                            E.bBaselinePending = false; // spawn carried the pose; hold the transform one tick
                        }
                        else
                        {
                            bool bScale = false;
                            if (NetGraph::IsTransformDue(E, Ex.Seq[Rec], CV.bForceBaseline, Settings, bScale))
                            {
                                const bool bBaseline = CV.bForceBaseline || E.bNeedsBaseline;
                                FTransformSendRecord R;
                                R.Guid   = Guid;
                                R.Tier   = E.Tier;
                                R.Pos    = Ex.Pos[Rec];
                                R.Rot    = Ex.Rot[Rec].ToQuat();
                                R.bScale = bScale;
                                R.Scale  = Ex.Scale[Rec].ToVector(NetQuantize::ScaleQuantum);
                                R.bPitch = (Ex.Flags[Rec] & NETREC_HasPitch) != 0;
                                R.Pitch  = Ex.Pitch[Rec];

                                // Staleness weighted by nearness, so a capped stream rotates through far entities instead of starving them.
                                const FVector3& P = Ex.WorldPos[Rec];
                                const float Distance = std::sqrt((P.x - VP.x) * (P.x - VP.x) + (P.z - VP.z) * (P.z - VP.z));
                                const float Priority = bBaseline ? 1.0e9f : E.TimeSinceSent / (1.0f + Distance / 15.0f);
                                Due.Emplace(R, Priority, Ex.Seq[Rec]);
                            }
                        }
                    }
                }

                if (MaxRecordsPerSend > 0 && static_cast<size_t>(Due.Num()) > MaxRecordsPerSend)
                {
                    Algo::NthElement(Due.begin(), Due.begin() + MaxRecordsPerSend, Due.end(),
                        [](const FNetDueRecord& A, const FNetDueRecord& B) { return A.Priority > B.Priority; });
                    Due.SetNum(IntCastChecked<int32>(MaxRecordsPerSend));
                }
                for (const FNetDueRecord& D : Due)
                {
                    const auto It = CV.Relevant.Find(D.Record.Guid);
                    if (It != nullptr)
                    {
                        It->bNeedsBaseline = false;
                        It->TimeSinceSent  = 0.0f;
                        It->SentSeq        = D.Seq;
                    }
                    Records.Add(D.Record);
                }

                // Expire entries not seen relevant this tick (left the AOI / destroyed) -> grace then despawn.
                for (auto It = CV.Relevant.begin(); It != CV.Relevant.end(); )
                {
                    FRelevantEntry& E = It->Value;
                    if (E.RelevantTick != State.RelevancyTick)
                    {
                        const uint32 Guid = It->Key;
                        const bool bDestroyed = (State.GuidTable.Find(SNetGUID{ Guid }) == ECS::NullEntity);
                        E.TimeOutOfAOI += SinceServiced;
                        if (bDestroyed || E.TimeOutOfAOI >= Grace)
                        {
                            if (E.bDynamic)
                            {
                                Ops.Emplace(EClientOp::Despawn, Guid, 0);
                            }
                            It = CV.Relevant.Remove(It);
                            continue;
                        }
                    }
                    ++It;
                }

                CV.bForceBaseline = false;
                ClientRelevant[ci] = static_cast<uint32>(CV.Relevant.Num());
            });

            PhaseMark(ENetPhase::Relevancy);
            // Serial, since writing a message mints shared export indices. Each one is identical for every recipient.
            THashMap<uint32, TVector<uint8>> SpawnCache;
            THashMap<uint32, TVector<uint8>> BaselineCache;
            THashMap<uint32, TVector<uint8>> ResyncCache;
            const double PropertyNow     = PlatformTime::Seconds();
            const double PropertyTimeout = GetDefault<CNetworkSettings>()->PropertyAckTimeout;
            for (uint32 ci = 0; ci < NumClients; ++ci)
            {
                LUMINA_PROFILE_SECTION("Net/ClientOps");
                RelevantSum += ClientRelevant[ci];
                RelevantMax  = Math::Max(RelevantMax, ClientRelevant[ci]);
                TVector<uint8>& Reliable = ClientReliable[ci];
                const uint32 ConnId = State.ConnectedClientIds[ci];
                FNetPropertyDelivery& Properties = Views[ci]->Properties;
                State.Stats.PropertyPacketsLost += NetProperties::Expire(Properties, PropertyNow, PropertyTimeout);

                // A lost packet's entities go again as they are now, written once however many clients lost them.
                if (Serviced[ci] && !Properties.Resends.IsEmpty())
                {
                    Algo::Sort(Properties.Resends);
                    for (int32 Index = 0; Index < Properties.Resends.Num(); ++Index)
                    {
                        const uint32 Guid = Properties.Resends[Index];
                        const ECS::FEntity Ent = State.GuidTable.Find(SNetGUID{ Guid });
                        if ((Index > 0 && Properties.Resends[Index - 1] == Guid) || !State.HoldsEntity(ConnId, Guid) || !Registry.IsValid(Ent))
                        {
                            continue;
                        }
                        TVector<uint8>* CachedPtr = ResyncCache.Find(Guid);
                        TVector<uint8>& Cached = CachedPtr != nullptr ? *CachedPtr : ResyncCache.Add(Guid);
                        if (CachedPtr == nullptr)
                        {
                            TVector<uint8> Msg;
                            WritePropertyResync(Registry, State, Ent, Guid, Msg);
                            Net::AppendFramedMessage(Cached, Msg.GetData(), static_cast<SIZE_T>(Msg.Num()));
                        }
                        NetProperties::Queue(Properties, Guid, Cached.GetData(), static_cast<SIZE_T>(Cached.Num()));
                    }
                    Properties.Resends.Reset();
                }

                bool bSpawned = false;
                for (const FClientOp& Op : ClientOps[ci])
                {
                    if (Op.Op == EClientOp::Despawn)
                    {
                        TVector<uint8> Buf;
                        WriteDespawnMessage(Op.Guid, Buf);
                        Net::AppendFramedMessage(Reliable, Buf.GetData(), static_cast<SIZE_T>(Buf.Num()));
                        ++DespawnsSum;
                        continue;
                    }

                    const ECS::FEntity Ent = State.GuidTable.Find(SNetGUID{ Op.Guid });
                    if (Ent == ECS::NullEntity || !Registry.IsValid(Ent))
                    {
                        continue;
                    }

                    if (Op.Op == EClientOp::Spawn)
                    {
                        TVector<uint8>* CachedPtr = SpawnCache.Find(Op.Guid);
                        const bool bNew = CachedPtr == nullptr;
                        TVector<uint8>& Cached = bNew ? SpawnCache.Add(Op.Guid) : *CachedPtr;
                        if (bNew)
                        {
                            WriteSpawnMessage(Registry, State, Ent, Op.Guid, Ex.OwnerConn[Op.Rec], Ex.Pos[Op.Rec], Ex.Rot[Op.Rec].ToQuat(),
                                Ex.Scale[Op.Rec].ToVector(NetQuantize::ScaleQuantum), Cached);
                        }
                        Net::AppendFramedMessage(Reliable, Cached.GetData(), static_cast<SIZE_T>(Cached.Num()));
                        ++SpawnsSum;
                        bSpawned = true;
                        continue;
                    }

                    // Refs minted here are flushed by the export step before this reliable batch.
                    if (!Registry.HasAny<FComponentRepState>(Ent))
                    {
                        continue;
                    }
                    TVector<uint8>* CachedPtr = BaselineCache.Find(Op.Guid);
                    const bool bNew = CachedPtr == nullptr;
                    TVector<uint8>& Cached = bNew ? BaselineCache.Add(Op.Guid) : *CachedPtr;
                    if (bNew)
                    {
                        CollectBaseline(Registry, State, Ent, Op.Guid, &Registry.GetOrEmplace<FComponentRepState>(Ent), &Cached);
                    }
                    Net::AppendFramedMessage(Reliable, Cached.GetData(), static_cast<SIZE_T>(Cached.Num()));
                }

                // An older property packet still in flight could land after these spawns and undo them, so the client ignores it and gets it again.
                if (bSpawned)
                {
                    TVector<uint8> Fence;
                    FNetArchive Writer(Fence);
                    uint8 Type = static_cast<uint8>(ENetMessage::PropertyFence);
                    Writer << Type;
                    Writer << Properties.NextSeq;
                    Net::AppendFramedMessage(Reliable, Fence.GetData(), static_cast<SIZE_T>(Fence.Num()));
                }
            }

            LUMINA_PROFILE_VALUE("Net/RelevantMax", RelevantMax);
            LUMINA_PROFILE_VALUE("Net/Spawns",      SpawnsSum);
            LUMINA_PROFILE_VALUE("Net/Despawns",    DespawnsSum);

            PhaseMark(ENetPhase::Messages);
            // Exports first, so every client learns an index before any spawn that references it.
            TVector<uint8> Exports;
            Net::BuildObjectExport(State.OutObjects, State.OutObjects.PendingExports, Exports);
            Net::BuildAssetExport(State.OutAssets, State.OutAssets.PendingExports, Exports);
            Net::BuildNameExport(State.OutNames, State.OutNames.PendingExports, Exports);
            State.OutObjects.PendingExports.Reset();
            State.OutAssets.PendingExports.Reset();
            State.OutNames.PendingExports.Reset();
            if (!Exports.IsEmpty())
            {
                State.Transport->Broadcast(Exports.GetData(), static_cast<SIZE_T>(Exports.Num()), 0, ESendMode::Reliable);
            }
            PhaseMark(ENetPhase::Exports);
            if (!ReliableBroadcast.IsEmpty())
            {
                State.Stats.ReliableBatchBytes = static_cast<uint32>(ReliableBroadcast.Num());
                State.Transport->Broadcast(ReliableBroadcast.GetData(), static_cast<SIZE_T>(ReliableBroadcast.Num()), 0, ESendMode::Reliable);
            }

            PhaseMark(ENetPhase::Exports);
            // Ownership changes resolve to GUIDs here, after this tick's spawns have minted theirs.
            TVector<FNetOwnerRecord> OwnerChanges;
            for (ECS::FEntity Entity : State.OwnershipChanged)
            {
                const SNetworkComponent* Net = Registry.IsValid(Entity) ? Registry.TryGet<ECS::Read<SNetworkComponent>>(Entity) : nullptr;
                if (Net != nullptr && Net->bNetLoadOnClient && Net->NetGUID.Value != 0)
                {
                    OwnerChanges.Emplace(Net->NetGUID.Value, Net->OwningConnectionId);
                }
            }
            State.OwnershipChanged.Reset();

            TVector<FNetOwnerRecord> JoinerTable;
            if (!State.OwnershipJoiners.IsEmpty())
            {
                for (auto [Entity, Net] : Registry.View<ECS::Read<SNetworkComponent>>().Each())
                {
                    if (Net.bNetLoadOnClient && Net.NetGUID.Value != 0 && Net.NetGUID.Value < NetGUID_DynamicStart && Net.OwningConnectionId != 0)
                    {
                        JoinerTable.Emplace(Net.NetGUID.Value, Net.OwningConnectionId);
                    }
                }
            }

            PhaseMark(ENetPhase::Exports);
            // Phase B builds every client's payload across workers, since only the transport calls must stay serial.
            TVector<TVector<uint8>> ClientUnreliable; ClientUnreliable.SetNum(NumClients);
            TVector<TVector<TVector<uint8>>> ClientPropertyPackets; ClientPropertyPackets.SetNum(NumClients);
            TVector<int32>          ClientPropertyCount; ClientPropertyCount.SetNum(NumClients);
            TVector<uint32>         ClientLargest;    ClientLargest.SetNum(NumClients);
            TVector<uint32>         ClientBytes;      ClientBytes.SetNum(NumClients);
            TVector<uint32>         ClientChunks;     ClientChunks.SetNum(NumClients);
            if (static_cast<uint32>(State.OwnershipPickedScratch.Num()) < NumThreadSlots) { State.OwnershipPickedScratch.SetNum(NumThreadSlots); }
            Jobs::ParallelFor(NumClients, [&](uint32 ci, uint32 Thread)
            {
                const uint32 ConnId = State.ConnectedClientIds[ci];
                TVector<uint8>& Pending = Views[ci]->PendingReliable;

                const bool bJoiner = Algo::Find(State.OwnershipJoiners, ConnId) != State.OwnershipJoiners.end();
                if (!OwnerChanges.IsEmpty() || bJoiner)
                {
                    AppendOwnership(State, Thread, ConnId, OwnerChanges, bJoiner ? &JoinerTable : nullptr, Pending);
                }
                FNetPropertyDelivery& Properties = Views[ci]->Properties;
                for (const FNetWorldState::FPendingProperty& Property : State.PendingProperties)
                {
                    if (State.HoldsEntity(ConnId, Property.Guid))
                    {
                        NetProperties::Queue(Properties, Property.Guid, State.PendingPropertyBytes.GetData() + Property.Offset, Property.Size);
                    }
                }
                if (!Serviced[ci])
                {
                    return;
                }
                NetProperties::Pack(Properties, PropertyNow, ClientPropertyPackets[ci], ClientPropertyCount[ci]);

                // Spawns lead, so an update never reaches a client before the entity it changes.
                TVector<uint8>& Reliable = ClientReliable[ci];
                Reliable.Append(Pending);
                Pending.Reset();

                const TVector<FTransformSendRecord>& Records = ClientRecords[ci];
                TVector<uint8>& Unreliable = ClientUnreliable[ci];
                const NetQuantize::FQuantizedVector QAnchor = NetQuantize::FQuantizedVector::FromVector(ClientAnchor[ci], SnapshotAnchorQuantum);
                TVector<uint8> Scratch;
                ClientLargest[ci] = AppendTransformFrames(Unreliable, Scratch, Records, QAnchor.ToVector(SnapshotAnchorQuantum), [&](FNetArchive& Writer, uint16 Count)
                {
                    uint8 Type = static_cast<uint8>(ENetMessage::TransformSnapshot);
                    Writer << Type;
                    Writer << ServerTime;
                    Writer << Count;
                    QAnchor.Write(Writer);
                });
                ClientChunks[ci] = static_cast<uint32>((Records.Num() + MaxRecordsPerFrame - 1) / MaxRecordsPerFrame);
                ClientBytes[ci]  = static_cast<uint32>(Unreliable.Num()) - ClientChunks[ci] * sizeof(uint16);
            });

            PhaseMark(ENetPhase::Payloads);
            uint32 LargestFrame = 0, TotalBytes = 0, Chunks = 0;
            {
                LUMINA_PROFILE_SECTION("Net/ClientSend");
                for (uint32 ci = 0; ci < NumClients; ++ci)
                {
                    const FConnectionHandle Conn{ State.ConnectedClientIds[ci] };
                    if (!ClientReliable[ci].IsEmpty())
                    {
                        State.Transport->Send(Conn, ClientReliable[ci].GetData(), static_cast<SIZE_T>(ClientReliable[ci].Num()), 0, ESendMode::Reliable);
                    }
                    if (!ClientUnreliable[ci].IsEmpty())
                    {
                        State.Transport->Send(Conn, ClientUnreliable[ci].GetData(), static_cast<SIZE_T>(ClientUnreliable[ci].Num()), 0, ESendMode::UnreliableSequenced);
                    }
                    for (int32 Packet = 0; Packet < ClientPropertyCount[ci]; ++Packet)
                    {
                        const TVector<uint8>& Bytes = ClientPropertyPackets[ci][Packet];
                        State.Transport->Send(Conn, Bytes.GetData(), static_cast<SIZE_T>(Bytes.Num()), NetPropertyChannel, ESendMode::UnreliableSequenced);
                    }
                    LargestFrame = Math::Max(LargestFrame, ClientLargest[ci]);
                    TotalBytes  += ClientBytes[ci];
                    Chunks      += ClientChunks[ci];
                }
            }

            PhaseMark(ENetPhase::Send);
            State.OwnershipJoiners.Reset();
            State.Stats.MovementEntityCount    = Ex.Num();
            State.Stats.MovementFrameBytes     = LargestFrame;
            State.Stats.MovementTotalBytes     = TotalBytes;
            State.Stats.UnreliableBatchBytes   = TotalBytes;
            State.Stats.MovementChunks         = static_cast<uint16>(Chunks > 0xFFFF ? 0xFFFF : Chunks);
            State.Stats.PeakMovementFrameBytes = Math::Max(State.Stats.PeakMovementFrameBytes, LargestFrame);
            State.Stats.SpawnsSent             = static_cast<uint16>(SpawnsSum > 0xFFFF ? 0xFFFF : SpawnsSum);
            State.Stats.DespawnsSent           = static_cast<uint16>(DespawnsSum > 0xFFFF ? 0xFFFF : DespawnsSum);
            State.Stats.RelevantAvg            = NumClients ? (RelevantSum / NumClients) : 0;
            State.Stats.RelevantMax            = RelevantMax;
        }

        // Client -> server. Frames the pose of entities this client owns (AutonomousProxy) for the upstream send.
        void WriteOwnedTransforms(ECS::FRegistry& Registry, float DeltaTime, TVector<uint8>& OutBatch)
        {
            auto TransformStorage = Registry.GetStorage<STransformComponent>();

            TVector<FTransformSendRecord> Records;
            Registry.View<ECS::Write<SNetworkComponent>, ECS::Write<FRepTransform>>().ForEach([&](ECS::FEntity Entity, ECS::TMut<SNetworkComponent> NetMut, ECS::TMut<FRepTransform> RepMut)
            {
                if (NetMut.Get().LocalRole != ENetRole::AutonomousProxy || !NetMut.Get().bReplicatesMovement
                    || NetPrediction::IsPredictedCharacter(Registry, Entity) || !TransformStorage.Contains(Entity))
                {
                    return;
                }
                FRepTransform& Rep = RepMut.GetMut();
                SNetworkComponent& Net = NetMut.GetMut();
                STransformComponent& T = TransformStorage.Get(Entity);

                bool bScale = false;
                if (!PrepareTransformSend(Net, Rep, T.GetLocalLocation(), T.GetLocalRotation(), T.GetLocalScale(), DeltaTime, false, bScale))
                {
                    return;
                }
                FTransformSendRecord R;
                R.Guid   = Net.NetGUID.Value;
                R.Tier   = ENetLODTier::Near; // the owner's own pawn -> full precision upstream
                R.Pos    = T.GetLocalLocation();
                R.Rot    = T.GetLocalRotation();
                R.bScale = bScale;
                R.Scale  = T.GetLocalScale();
                Records.Add(R);
            });

            TVector<uint8> Scratch;
            AppendTransformFrames(OutBatch, Scratch, Records, FVector3(0.0f), [](FNetArchive& Writer, uint16 Count)
            {
                uint8 Type = static_cast<uint8>(ENetMessage::ClientTransform);
                Writer << Type;
                Writer << Count;
            });
        }

        void SendOwnedTransforms(ECS::FRegistry& Registry, FNetWorldState& State, float DeltaTime)
        {
            TVector<uint8> Batch;
            WriteOwnedTransforms(Registry, DeltaTime, Batch);
            if (!Batch.IsEmpty())
            {
                State.Transport->Send(State.ServerConnection, Batch.GetData(), static_cast<SIZE_T>(Batch.Num()), 0, ESendMode::UnreliableSequenced);
            }
        }

        // Returns false on a decode error so the caller breaks the batch.
        bool ReadTransformIntoRing(ECS::FRegistry& Registry, FNetWorldState& State, FNetArchive& Reader, double SampleTime, bool bSkipAutonomous,
                                   uint32 OwnerGate, const FVector3& Anchor)
        {
            // Decode mirrors WriteTransformRecord, where the tier byte selects quantum and precision.
            uint8 TierByte = 0;
            Reader.SerializeBits(&TierByte, 2);
            const ENetLODTier Tier = static_cast<ENetLODTier>(TierByte);
            const uint32 Guid = Net::ReadNetGuid(Reader);

            bool bAnchored = false;
            Reader.SerializeBit(bAnchored);
            NetQuantize::FQuantizedVector QPos;
            QPos.Read(Reader);
            const FVector3 Pos = QPos.ToVector(TierPosQuantum(Tier)) + (bAnchored ? Anchor : FVector3(0.0f));

            FQuat Rot;
            if (Tier == ENetLODTier::Far)
            {
                Rot = ReadYaw(Reader, FarYawBits);
            }
            else
            {
                bool bYawOnly = false;
                Reader.SerializeBit(bYawOnly);
                if (bYawOnly)
                {
                    Rot = ReadYaw(Reader, YawBits);
                }
                else
                {
                    NetQuantize::FQuantizedQuat QRot;
                    QRot.Read(Reader);
                    Rot = QRot.ToQuat();
                }
            }

            bool bScale = false;
            Reader.SerializeBit(bScale);
            NetQuantize::FQuantizedVector QScale;
            if (bScale)
            {
                QScale.Read(Reader);
            }
            bool bPitch = false;
            Reader.SerializeBit(bPitch);
            uint8 PackedPitch = 0;
            if (bPitch)
            {
                Reader.SerializeBits(&PackedPitch, 8);
            }
            if (Reader.HasError())
            {
                return false;
            }

            const ECS::FEntity Entity = State.GuidTable.Find(SNetGUID{ Guid });
            if (Entity == ECS::NullEntity || !Registry.IsValid(Entity) || !Registry.HasAll<STransformComponent>(Entity))
            {
                return true; // unknown/late entity; skip but keep parsing the batch
            }

            const SNetworkComponent* Net = Registry.TryGet<ECS::Read<SNetworkComponent>>(Entity);
            // The client skips entities it controls, since the server pose would be stale.
            if (bSkipAutonomous && Net != nullptr && Net->LocalRole == ENetRole::AutonomousProxy)
            {
                return true;
            }
            // The server only accepts poses for entities the sender actually owns, and simulates its characters itself.
            if (OwnerGate != 0 && (Net == nullptr || Net->OwningConnectionId != OwnerGate || NetPrediction::IsPredictedCharacter(Registry, Entity)))
            {
                return true;
            }

            // Only another player's character takes the pitch, since the owner's own view is its input.
            if (bPitch && OwnerGate == 0)
            {
                if (SCharacterControllerComponent* Controller = Registry.TryGet<ECS::Write<SCharacterControllerComponent>>(Entity))
                {
                    Controller->LookInput.y = ViewPitchDegrees(static_cast<int8>(PackedPitch));
                }
            }

            FRepTransform& Rep = Registry.GetOrEmplace<FRepTransform>(Entity);
            Rep.NetGUID = Guid;
            Rep.Ring.Push(SampleTime, Pos, Rot);
            if (bScale)
            {
                Rep.CurrentScaleQ = QScale;
                Rep.bHasScale     = true;
            }
            return true;
        }

        // Client, buffer each received pose into the entity's ring; the interp system writes the smoothed pose.
        void ApplyTransformSnapshot(ECS::FRegistry& Registry, FNetWorldState& State, const uint8* Data, SIZE_T Size)
        {
            LUMINA_PROFILE_SCOPE();
            FNetArchive Reader(Data, Size);
            uint8 Type = 0;
            Reader << Type; // ENetMessage::TransformSnapshot (already routed by the caller)
            float ServerTime = 0.0f;
            Reader << ServerTime;
            uint16 Count = 0;
            Reader << Count;
            NetQuantize::FQuantizedVector QAnchor;
            QAnchor.Read(Reader);
            const FVector3 Anchor = QAnchor.ToVector(SnapshotAnchorQuantum);

            const double SampleTime = static_cast<double>(ServerTime);
            State.LatestServerTime = Math::Max(SampleTime, State.LatestServerTime);

            for (uint16 Index = 0; Index < Count; ++Index)
            {
                if (!ReadTransformIntoRing(Registry, State, Reader, SampleTime, /*bSkipAutonomous*/ true, /*OwnerGate*/ 0, Anchor))
                {
                    break;
                }
            }
        }

        // Server, buffer each client-owned pose into its ring (gated on ownership). Relayed raw next snapshot.
        void ApplyClientTransform(ECS::FRegistry& Registry, FNetWorldState& State, uint32 SenderConn, double ServerNow, const uint8* Data, SIZE_T Size)
        {
            FNetArchive Reader(Data, Size);
            uint8  Type  = 0;
            uint16 Count = 0;
            Reader << Type;
            Reader << Count;

            State.LatestServerTime = Math::Max(ServerNow, State.LatestServerTime);

            for (uint16 i = 0; i < Count; ++i)
            {
                if (!ReadTransformIntoRing(Registry, State, Reader, ServerNow, /*bSkipAutonomous*/ false, /*OwnerGate*/ SenderConn, FVector3(0.0f)))
                {
                    break;
                }
            }
        }
        // The server always trusts its own idea of the sender, never the caller id a client wrote.
        void ReceiveScriptRpc(ECS::FRegistry& Registry, FNetWorldState& State, bool bServer, uint32 Sender, const uint8* Data, SIZE_T Size)
        {
            FNetArchive Reader(Data, Size);
            uint8 Type = 0, TargetByte = 0, Flags = 0;
            Reader << Type;
            const uint32 Guid        = Net::ReadNetGuid(Reader);
            const uint32 ScriptIndex = ReadVarUInt(Reader);
            const uint32 RpcId       = ReadVarUInt(Reader);
            Reader << TargetByte;
            Reader << Flags;
            const uint32 Caller      = ReadVarUInt(Reader);
            const uint32 PayloadSize = ReadVarUInt(Reader);
            if (Reader.HasError() || PayloadSize > Net::MaxFramedMessageSize)
            {
                return;
            }

            TVector<uint8> Payload(PayloadSize);
            if (PayloadSize > 0)
            {
                Reader.Serialize(Payload.GetData(), static_cast<int64>(PayloadSize));
                if (Reader.HasError())
                {
                    return;
                }
            }

            const ECS::FEntity Entity = State.GuidTable.Find(SNetGUID{ Guid });
            if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
            {
                return;
            }
            CEntityScript* Script = Net::GetScriptAt(Registry, Entity, ScriptIndex);
            if (Script == nullptr)
            {
                return;
            }

            // Queued, never run here, so a script's RPC body always runs at the sync point after this system.
            auto Queue = [&](uint32 From)
            {
                if (CWorld* World = Script->GetWorld())
                {
                    ScriptNet::QueueRpc(*World, Entity, ScriptIndex, RpcId, From, Move(Payload));
                }
            };

            if (!bServer)
            {
                Queue(Caller);
                return;
            }

            ERpcTarget Target = ERpcTarget::Broadcast;
            if (!ScriptNet::FindRpcTarget(Script, RpcId, Target) || Target != static_cast<ERpcTarget>(TargetByte))
            {
                return;
            }
            const bool bReliable = (Flags & static_cast<uint8>(ENetFlags::Unreliable)) == 0;
            auto Relay = [&](uint32 Connection)
            {
                TVector<uint8>& Batch = bReliable ? State.PendingRpcReliable[Connection] : State.PendingRpcUnreliable[Connection];
                Net::AppendScriptRpc(Batch, Guid, ScriptIndex, RpcId, Target, Flags, Sender, Payload.GetData(), PayloadSize);
            };

            switch (Target)
            {
            case ERpcTarget::Host:
                Queue(Sender);
                break;
            case ERpcTarget::Broadcast:
                for (uint32 Connection : State.ReadyClientIds)
                {
                    if (Connection != Sender && State.HoldsEntity(Connection, Guid))
                    {
                        Relay(Connection);
                    }
                }
                Queue(Sender);
                break;
            case ERpcTarget::Owner:
            {
                const SNetworkComponent* Net = Registry.TryGet<ECS::Read<SNetworkComponent>>(Entity);
                const uint32 Owner = Net != nullptr ? Net->OwningConnectionId : 0u;
                if (Owner == 0)
                {
                    Queue(Sender);
                }
                else if (Owner != Sender)
                {
                    Relay(Owner);
                }
                break;
            }
            }
        }

        const char* MessageName(uint32 Type)
        {
            switch (static_cast<ENetMessage>(Type))
            {
                case ENetMessage::TransformSnapshot: return "transform";
                case ENetMessage::ScriptRpc:         return "rpc";
                case ENetMessage::OwnershipUpdate:   return "ownership";
                case ENetMessage::SpawnEntity:       return "spawn";
                case ENetMessage::DespawnEntity:     return "despawn";
                case ENetMessage::TearOffEntity:     return "tearoff";
                case ENetMessage::EventMessage:      return "message";
                case ENetMessage::PropertyUpdate:
                case ENetMessage::PropertyPacket:    return "property";
                case ENetMessage::MoveAck:           return "moveack";
                case ENetMessage::ObjectExport:      return "objects";
                case ENetMessage::AssetExport:       return "assets";
                case ENetMessage::NameExport:        return "names";
                default:                             return "other";
            }
        }

        void LogStats(FNetWorldState& State, float DeltaTime)
        {
            static const float Interval = (float)Global::GCommandLine->GetInt("netstats").Get(0);
            if (Interval <= 0.0f || State.Transport == nullptr)
            {
                return;
            }

            // From this world's first stage to its send, which leaves out the frame cap's sleep.
            const float FrameMs = static_cast<float>((PlatformTime::Seconds() - State.StatsTickStart) * 1000.0);
            State.StatsFrameMsMax = Math::Max(State.StatsFrameMsMax, FrameMs);
            State.StatsFrameMsSum += FrameMs;
            ++State.StatsFrames;
            State.StatsTimer += DeltaTime;
            if (State.StatsTimer < Interval)
            {
                return;
            }

            const FNetworkStats Totals = State.Transport->GetStats();
            TVector<FConnectionStats> Peers;
            State.Transport->GetConnectionStats(Peers);
            uint32 RttSum = 0, RttMax = 0, BacklogMax = 0;
            float LossMax = 0.0f;
            for (const FConnectionStats& Peer : Peers)
            {
                RttSum += Peer.RoundTripTimeMs;
                RttMax = Math::Max(RttMax, Peer.RoundTripTimeMs);
                LossMax = Math::Max(LossMax, Peer.PacketLoss);
                BacklogMax = Math::Max(BacklogMax, State.Transport->GetReliableBacklogBytes(FConnectionHandle{ Peer.ConnectionId }));
            }
            const float Seconds = State.StatsTimer;
            LOG_DISPLAY("[Net][Stats] clients {}  tick {:.0f} Hz  out {:.1f} KB/s  in {:.1f} KB/s  rtt avg {} max {} ms  relevant avg {} max {}  work avg {:.2f} max {:.2f} ms  backlog max {:.1f} KB  loss max {:.1f}%  memory {} MB  net receive {:.2f} send avg {:.2f} max {:.2f} ms",
                State.ConnectedClients, (double)State.StatsFrames / State.StatsTimer,
                (double)(Totals.TotalSentBytes - State.StatsLastSent) / 1024.0 / Seconds,
                (double)(Totals.TotalReceivedBytes - State.StatsLastReceived) / 1024.0 / Seconds,
                Peers.IsEmpty() ? 0u : RttSum / (uint32)Peers.Num(), RttMax,
                State.Stats.RelevantAvg, State.Stats.RelevantMax,
                State.StatsFrames ? State.StatsFrameMsSum / State.StatsFrames : 0.0, State.StatsFrameMsMax,
                BacklogMax / 1024.0, LossMax * 100.0f, Platform::GetProcessMemoryUsageMegaBytes(),
                State.StatsFrames ? State.StatsReceiveMsSum / State.StatsFrames : 0.0,
                State.StatsFrames ? State.StatsSendMsSum / State.StatsFrames : 0.0, State.StatsSendMsMax);

            if (FNetCountingTransport* Counting = State.CountingTransport)
            {
                const TArray<uint64, FNetCountingTransport::TypeCount> Bytes = Counting->TakeBytesByType();
                FString Breakdown;
                for (uint32 Type = 0; Type < FNetCountingTransport::TypeCount; ++Type)
                {
                    if (Bytes[Type] > 0)
                    {
                        Breakdown += *Format("  {} {:.1f}", MessageName(Type), (double)Bytes[Type] / 1024.0 / Seconds);
                    }
                }
                LOG_DISPLAY("[Net][Stats] out KB/s by type{}", *Breakdown);
            }

            static const char* const PhaseNames[] = { "extract", "schedule", "relevancy", "messages", "exports", "payloads", "send", "flush" };
            FString Phases;
            for (uint32 Phase = 0; Phase < static_cast<uint32>(ENetPhase::Count); ++Phase)
            {
                Phases += *Format("  {} {:.2f}/{:.1f}", PhaseNames[Phase], State.StatsFrames ? State.PhaseMsSum[Phase] / State.StatsFrames : 0.0, State.PhaseMsMax[Phase]);
                State.PhaseMsSum[Phase] = 0.0;
                State.PhaseMsMax[Phase] = 0.0;
            }
            LOG_DISPLAY("[Net][Stats] ms per tick avg/max{}", *Phases);

            State.StatsLastSent     = Totals.TotalSentBytes;
            State.StatsLastReceived = Totals.TotalReceivedBytes;
            State.StatsTimer        = 0.0f;
            State.StatsFrameMsMax   = 0.0f;
            State.StatsFrameMsSum   = 0.0;
            State.StatsReceiveMsSum = 0.0;
            State.StatsSendMsSum    = 0.0;
            State.StatsSendMsMax    = 0.0f;
            State.StatsFrames       = 0;
        }

        void FlushRpcs(FNetWorldState& State)
        {
            {
                TScopeLock Scope(State.MessageLock);
                for (auto& [Connection, Batch] : State.PendingMessages)
                {
                    if (!Batch.IsEmpty())
                    {
                        State.Transport->Send(FConnectionHandle{ Connection }, Batch.GetData(), static_cast<SIZE_T>(Batch.Num()), 0, ESendMode::Reliable);
                        Batch.Reset();
                    }
                }
            }
            for (auto& [Connection, Batch] : State.PendingRpcReliable)
            {
                if (!Batch.IsEmpty())
                {
                    State.Transport->Send(FConnectionHandle{ Connection }, Batch.GetData(), static_cast<SIZE_T>(Batch.Num()), 0, ESendMode::Reliable);
                    Batch.Reset();
                }
            }
            for (auto& [Connection, Batch] : State.PendingRpcUnreliable)
            {
                if (!Batch.IsEmpty())
                {
                    State.Transport->Send(FConnectionHandle{ Connection }, Batch.GetData(), static_cast<SIZE_T>(Batch.Num()), 1, ESendMode::UnreliableSequenced);
                    Batch.Reset();
                }
            }
        }

        void SendSessionEvent(CWorld* World, ENetSessionEventKind Kind, uint32 ConnectionId, ENetLeaveReason Reason)
        {
            SNetSessionEvent Event;
            Event.Kind         = Kind;
            Event.ConnectionId = ConnectionId;
            Event.Reason       = Reason;
            ECS::SendEvent(World->GetEvents(), Event);
        }

        // Runs the tick after the leave event, so its readers and the scripts it fans out to still see what the player held.
        void ReleaseDepartedConnections(CWorld* World, ECS::FRegistry& Registry, FNetWorldState& State)
        {
            for (const uint32 Connection : State.DepartedConnections)
            {
                // A player's own entities leave with them instead of lingering as host-owned leftovers.
                TVector<ECS::FEntity> Leaving;
                Registry.View<ECS::Read<SNetworkComponent>>().ForEach([&](ECS::FEntity Entity, const SNetworkComponent& Net)
                {
                    if (Net.OwningConnectionId == Connection && Net.bDestroyWithOwner)
                    {
                        Leaving.Add(Entity);
                    }
                });
                for (ECS::FEntity Entity : Leaving)
                {
                    if (Registry.IsValid(Entity))
                    {
                        World->DestroyEntity(Entity);
                    }
                }

                // Release anything this connection owned so it doesn't stay stuck as an orphan proxy.
                Registry.View<ECS::Write<SNetworkComponent>>().ForEach([&](ECS::FEntity Entity, ECS::TMut<SNetworkComponent> NetMut)
                {
                    if (NetMut.Get().OwningConnectionId == Connection)
                    {
                        NetMut->OwningConnectionId = 0;
                        State.OwnershipChanged.Add(Entity);

                        SNetOwnershipChangedEvent Changed;
                        Changed.Entity        = Entity;
                        Changed.PreviousOwner = Connection;
                        ECS::SendEventIfRead(State.Events, Changed);
                    }
                });
            }
            State.DepartedConnections.Reset();
        }

        void LeaveHost(CWorld* World, FNetWorldState& State, ENetLeaveReason Reason)
        {
            if (State.bLeaveDispatched)
            {
                return;
            }
            State.bLeaveDispatched = true;
            LOG_DISPLAY("[Net][Client] Left the host ({})", static_cast<uint32>(Reason));
            SendSessionEvent(World, ENetSessionEventKind::LeftHost, 0, Reason);
        }

        void SeedDirtyBaselines(ECS::FRegistry& Registry, FNetWorldState& State)
        {
            TVector<ECS::FEntity> Dirty;
            for (ECS::FEntity Entity : Registry.View<FNetDirty>())
            {
                Dirty.Add(Entity);
            }
            TVector<Net::FComponentRepOut>& Comps   = State.ReplicationScratch.Comps;
            TVector<Net::FScriptRepOut>&    Scripts = State.ReplicationScratch.Scripts;
            for (ECS::FEntity Entity : Dirty)
            {
                if (Registry.HasAll<SNetworkComponent>(Entity))
                {
                    FComponentRepState& Diff = Registry.GetOrEmplace<FComponentRepState>(Entity);
                    Net::CollectComponentFieldsInto(Registry, Entity, State, /*bBaseline*/false, &Diff, Comps);
                    if (Net::CollectScriptFieldsInto(Registry, Entity, State, /*bBaseline*/false, &Diff, Scripts))
                    {
                        continue;
                    }
                }
                Registry.Remove<FNetDirty>(Entity);
            }
        }

        // CScriptSyncSystem compares Sync fields only where this marks the entity networked, so a standalone script costs it nothing.
        void TrackScriptSync(ECS::FRegistry& Registry)
        {
            TVector<ECS::FEntity> Untracked;
            Registry.View<ECS::Read<SEntityScriptComponent>, ECS::Read<SNetworkComponent>>(ECS::TExclude<FScriptSyncState>{}).ForEachEntity([&](ECS::FEntity Entity) { Untracked.Add(Entity); });
            for (ECS::FEntity Entity : Untracked)
            {
                Registry.Emplace<FScriptSyncState>(Entity);
            }
        }

        void SendFrame(CWorld* World, ECS::FRegistry& Registry, FNetWorldState& State, const SSystemContext& Context, bool bServer)
        {
            LUMINA_PROFILE_SCOPE();
            NetSimulation::Pump(World, State, bServer);
            if (bServer)
            {
                ProcessTearOffs(World, Registry, State);
                MarkReplicatedChanges(Registry, State);
                if (State.ConnectedClients > 0)
                {
                    State.Stats.PropertyUpdatesSent = 0;
                    State.Stats.bKeyframeThisTick   = false;

                    // A periodic keyframe re-arms every client's baseline so a dropped delta heals.
                    const SDefaultWorldSettings& WorldSettings = World->GetDefaultWorldSettings();
                    const float KeyframeInterval = WorldSettings.TransformKeyframeInterval;
                    if (KeyframeInterval > 0.0f)
                    {
                        State.TimeSinceKeyframe += Context.GetDeltaTime();
                        if (State.TimeSinceKeyframe >= KeyframeInterval)
                        {
                            State.TimeSinceKeyframe       = 0.0f;
                            State.Stats.bKeyframeThisTick = true;
                        }
                    }

                    {
                        LUMINA_PROFILE_SECTION("Net/DynamicLifetime");
                        MaintainDynamicLifetime(Registry, State);
                    }

                    // Broadcast inside ServerReplicateRelevant, after the spawns mint and export their indices.
                    TVector<uint8> ReliableBatch;
                    ReplicateStableDespawns(Registry, State, ReliableBatch);
                    State.PendingProperties.Reset();
                    State.PendingPropertyBytes.Reset();
                    ReplicateDirtyProperties(Registry, State, Context.GetTime());

                    ServerReplicateRelevant(Registry, State, WorldSettings,
                        Context.GetDeltaTime(), static_cast<float>(Context.GetTime()), ReliableBatch);
                    LUMINA_PROFILE_SECTION("Net/Acks");
                    NetPrediction::SendAcks(Registry, State, Context.GetDeltaTime());
                }
                else
                {
                    SeedDirtyBaselines(Registry, State);
                }
            }
            else
            {
                const CNetworkSettings* Settings = GetDefault<CNetworkSettings>();
                const double Timeout = Settings != nullptr ? Settings->ConnectTimeout : 10.0;
                if (!State.bClientConnected && State.ConnectStartTime >= 0.0 && PlatformTime::Seconds() - State.ConnectStartTime > Timeout)
                {
                    State.ConnectStartTime = -1.0;
                    State.Transport->Disconnect(State.ServerConnection, 0, true);
                    LeaveHost(World, State, ENetLeaveReason::TimedOut);
                }

                if (State.bClientConnected && State.bWelcomed && !State.bJoinDispatched)
                {
                    State.bJoinDispatched = true;
                    SendSessionEvent(World, ENetSessionEventKind::JoinedHost, 0, ENetLeaveReason::None);
                }

                if (State.bClientConnected && !State.bClientReadySent)
                {
                    State.bClientReadySent = true;
                    TVector<uint8> Buffer;
                    FNetArchive Writer(Buffer);
                    uint8  Type  = static_cast<uint8>(ENetMessage::ClientReady);
                    uint32 Proto = Net::GetProtocolHash(State.ReplTypes);
                    Writer << Type;
                    Writer << Proto;
                    Net::SendFramed(*State.Transport, State.ServerConnection, Buffer.GetData(), static_cast<SIZE_T>(Buffer.Num()), 0, ESendMode::Reliable);
                }

                if (State.bClientConnected)
                {
                    SendOwnedTransforms(Registry, State, Context.GetDeltaTime());
                    NetPrediction::SendCommands(Registry, State);

                    TVector<uint8> Ack;
                    if (NetProperties::WriteAck(State.PropertyAcks, Ack))
                    {
                        State.Transport->Send(State.ServerConnection, Ack.GetData(), static_cast<SIZE_T>(Ack.Num()), NetPropertyChannel, ESendMode::UnreliableSequenced);
                    }
                }
            }

            {
                LUMINA_PROFILE_SECTION("Net/FlushRpcs");
                FlushRpcs(State);
            }
            {
                LUMINA_PROFILE_SECTION("Net/TransportFlush");
                const double FlushStart = PlatformTime::Seconds();
                State.Transport->Flush();
                const double FlushMs = (PlatformTime::Seconds() - FlushStart) * 1000.0;
                State.PhaseMsSum[static_cast<uint32>(ENetPhase::Flush)] += FlushMs;
                State.PhaseMsMax[static_cast<uint32>(ENetPhase::Flush)] = Math::Max(State.PhaseMsMax[static_cast<uint32>(ENetPhase::Flush)], FlushMs);
            }
            if (bServer)
            {
                LogStats(State, Context.GetDeltaTime());
            }
        }
    }

    void CNetworkSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart);
        RequireUpdate(EUpdateStage::FrameEnd);
        // The frame end snapshot leaves out what expired this frame.
        RunsAfter<CLifetimeSystem>();
        // Sync fields a script wrote this frame have to be compared before the send reads them.
        RunsAfter<CScriptSyncSystem>();
    }

    void CNetworkSystem::OnExclusiveUpdate(const SExclusiveContext& Context)
    {
        using namespace NetworkSystemInternal;

        ECS::FRegistry& Registry = Context.GetRegistry();

        CWorld* World = Context.GetWorld();
        if (World == nullptr)
        {
            return;
        }

        const ENetMode NetMode = World->GetNetMode();
        if (NetMode == ENetMode::Standalone)
        {
            return;
        }

        // Lazily create the per-world net state on the first networked tick; log the role once.
        FNetWorldState* State = Registry.Ctx().Find<FNetWorldState>();
        if (State == nullptr)
        {
            State = &Registry.Ctx().Emplace<FNetWorldState>();
            State->Registry = &Registry;
            State->Events   = &Context.GetEvents();
            LOG_DISPLAY("[Net] World '{}' net mode = {}", *World->GetName(), NetModeToString(NetMode));
        }
        Clock->bNetworked = true;

        const bool bServer = IsServerMode(NetMode);

        // Adopt pre-placed entities into stable NetGUIDs once, before any transport activity.
        if (!State->bStableEntitiesAdopted)
        {
            State->bStableEntitiesAdopted = true;
            AdoptStableNetworkEntities(Registry, *State, bServer);
        }

        if (!State->bInitialized)
        {
            State->bInitialized = true;

            if (Physics::IPhysicsScene* Scene = World->GetPhysicsScene())
            {
                Scene->SetCharacterStepHook([World](float, bool bAfter)
                {
                    Physics::IPhysicsScene* StepScene = World->GetPhysicsScene();
                    if (World->GetNetMode() == ENetMode::Client && StepScene != nullptr)
                    {
                        NetPrediction::OnCharacterStep(World->GetEntityRegistry(), *StepScene, bAfter);
                    }
                });
            }

            // Data-driven from FWorldContext, defaulting to loopback on port 7777.
            FWorldContext* WorldCtx = Global::GWorldManager ? Global::GWorldManager->FindContext(World) : nullptr;
            const FString  Host = WorldCtx ? WorldCtx->NetHost : FString("127.0.0.1");
            const uint16   Port = WorldCtx ? WorldCtx->NetPort : NetDefaultPort;

            Registry.GetSignals<SNetworkComponent>().OnDestroy.Connect<&OnNetworkComponentDestroyed>();

            if (bServer)
            {
                State->Transport.reset(Network::CreateTransport());
                if (Global::GCommandLine->GetInt("netstats").Get(0) > 0 && State->Transport != nullptr)
                {
                    TUniquePtr<FNetCountingTransport> Counting = MakeUnique<FNetCountingTransport>(Move(State->Transport));
                    State->CountingTransport = Counting.get();
                    State->Transport = Move(Counting);
                }

                FListenParams Params;
                Params.Port           = Port;
                const CNetworkSettings* NetSettings = GetDefault<CNetworkSettings>();
                Params.MaxConnections = NetSettings ? (uint32)Math::Max(NetSettings->MaxClients, 1) : 64u;
                if (TOptional<int> MaxClients = Global::GCommandLine->GetInt("maxclients"))
                {
                    Params.MaxConnections = (uint32)Math::Clamp(MaxClients.GetValue(), 1, 4095);
                }
                Params.ChannelCount   = NetChannelCount;

                if (State->Transport->StartServer(Params))
                {
                    LOG_DISPLAY("[Net] Listen server started on port {}", Port);
                }
                else
                {
                    LOG_ERROR("[Net] Failed to start listen server on port {}", Port);
                }
            }
            else if (Global::GEngine != nullptr && Global::GEngine->HasCarriedConnection())
            {
                // Welcome-driven travel handed us the already-connected transport from the previous world.
                FConnectionHandle Conn;
                uint32 PeerId = ServerPeerId;
                State->Transport        = Global::GEngine->TakeCarriedConnection(Conn, PeerId);
                State->ServerConnection = Conn;
                State->LocalPeerId      = PeerId;
                State->bClientConnected = true;
                State->bWelcomed        = true;
                LOG_DISPLAY("[Net] Client adopted carried connection after travel (peer id {})", PeerId);
            }
            else // Client, open a fresh connection.
            {
                State->Transport.reset(Network::CreateTransport());

                FConnectParams Params;
                Params.Address.Host = Host;
                Params.Address.Port = Port;
                Params.ChannelCount = NetChannelCount;

                State->ServerConnection = State->Transport->ConnectToServer(Params);
                State->ConnectStartTime = PlatformTime::Seconds();
                LOG_DISPLAY("[Net] Client connecting to {}:{}...", *Host, Port);
                if (State->ServerConnection == FConnectionHandle::Invalid())
                {
                    LeaveHost(World, *State, ENetLeaveReason::ConnectFailed);
                }
            }
        }

        if (State->Transport == nullptr)
        {
            return;
        }

        if (Context.GetUpdateStage() == EUpdateStage::FrameEnd)
        {
            TrackScriptSync(Registry);
            const double SendStart = PlatformTime::Seconds();
            SendFrame(World, Registry, *State, Context, bServer);
            const float SendMs = static_cast<float>((PlatformTime::Seconds() - SendStart) * 1000.0);
            State->StatsSendMsSum += SendMs;
            State->StatsSendMsMax  = Math::Max(State->StatsSendMsMax, SendMs);
            return;
        }

        State->StatsTickStart = PlatformTime::Seconds();
        if (bServer)
        {
            NetPrediction::TickBudgets(Registry, NetPrediction::FixedStepOf(World), Context.GetDeltaTime());
        }

        if (bServer)
        {
            ReleaseDepartedConnections(World, Registry, *State);
        }

        // Reuse the events buffer across ticks; Service appends, so clear it first.
        TVector<FNetworkEvent>& Events = State->ServiceEvents;
        Events.Reset();
        {
            LUMINA_PROFILE_SECTION("Net/Service");
            State->Transport->Service(Events);
        }

        for (const FNetworkEvent& Event : Events)
        {
            LUMINA_PROFILE_SECTION("Net/Event");
            switch (Event.Type)
            {
            case ENetworkEventType::Connected:
                if (bServer)
                {
                    ++State->ConnectedClients;
                    State->ConnectedClientIds.Add(Event.Connection.Value);
                    State->ClientViews[Event.Connection.Value] = FNetClientView{}; // bForceBaseline defaults true

                    // Tell the new client its unique peer id (= its connection handle here).
                    TVector<uint8> Buffer;
                    FNetArchive Writer(Buffer);
                    uint8  Type   = static_cast<uint8>(ENetMessage::AssignPeerId);
                    uint32 PeerId = Event.Connection.Value;
                    Writer << Type;
                    Writer << PeerId;
                    Net::SendFramed(*State->Transport, Event.Connection, Buffer.GetData(), static_cast<SIZE_T>(Buffer.Num()), 0, ESendMode::Reliable);

                    // Welcome, tell the client which map we're running so it can load the same level.
                    if (FWorldContext* SrvCtx = Global::GWorldManager ? Global::GWorldManager->FindContext(World) : nullptr)
                    {
                        const FString& MapPath = SrvCtx->MapPath;
                        TVector<uint8> WelcomeBuf;
                        FNetArchive WWriter(WelcomeBuf);
                        uint8  WType  = static_cast<uint8>(ENetMessage::Welcome);
                        uint32 WProto = Net::GetProtocolHash(State->ReplTypes);
                        uint16 Len    = static_cast<uint16>(MapPath.Len());
                        WWriter << WType;
                        WWriter << WProto; // client refuses the connection on a protocol/build mismatch
                        WWriter << Len;
                        if (Len > 0)
                        {
                            WWriter.Serialize(const_cast<char*>(MapPath.GetData()), Len);
                        }
                        Net::SendFramed(*State->Transport, Event.Connection, WelcomeBuf.GetData(), static_cast<SIZE_T>(WelcomeBuf.Num()), 0, ESendMode::Reliable);
                    }

                    LOG_DISPLAY("[Net][Server] Client {} connected ({} total)", Event.Connection.Value, State->ConnectedClients);
                }
                else
                {
                    State->bClientConnected = true;
                    LOG_DISPLAY("[Net][Client] Connected to server (handle {})", Event.Connection.Value);
                }
                break;

            case ENetworkEventType::Disconnected:
                if (bServer)
                {
                    State->ConnectedClients = (State->ConnectedClients > 0) ? State->ConnectedClients - 1 : 0;
                    auto& Ids = State->ConnectedClientIds;
                    Ids.Remove(Event.Connection.Value);
                    auto& Joiners = State->OwnershipJoiners;
                    Joiners.Remove(Event.Connection.Value);
                    State->PendingRpcReliable.Remove(Event.Connection.Value);
                    State->PendingRpcUnreliable.Remove(Event.Connection.Value);
                    State->InObjects.Remove(Event.Connection.Value);
                    State->InAssets.Remove(Event.Connection.Value);
                    State->InNames.Remove(Event.Connection.Value);

                    auto& Ready = State->ReadyClientIds;
                    if (Algo::Find(Ready, Event.Connection.Value) != Ready.end())
                    {
                        Ready.Remove(Event.Connection.Value);
                        SendSessionEvent(World, ENetSessionEventKind::ClientLeft, Event.Connection.Value, ENetLeaveReason::None);
                    }
                    State->ClientViews.Remove(Event.Connection.Value); // drop its per-client relevancy state
                    NetSimulation::OnConnectionLeft(*State, Event.Connection.Value);
                    State->DepartedConnections.Add(Event.Connection.Value);
                    LOG_DISPLAY("[Net][Server] Client {} disconnected ({} total)", Event.Connection.Value, State->ConnectedClients);
                }
                else
                {
                    const bool bWasConnected = State->bClientConnected;
                    State->bClientConnected = false;
                    State->LocalPeerId      = ServerPeerId;
                    LOG_DISPLAY("[Net][Client] Disconnected from server");
                    const ENetLeaveReason Reason = State->PendingLeaveReason != ENetLeaveReason::None ? State->PendingLeaveReason
                        : (bWasConnected ? ENetLeaveReason::HostClosed : ENetLeaveReason::ConnectFailed);
                    LeaveHost(World, *State, Reason);
                }
                break;

            case ENetworkEventType::Data:
            {
                // Every packet is a batch of length-framed messages; split + dispatch each by its type byte.
                Net::ForEachFramedMessage(Event.Data.GetData(), Event.Data.Num(), [&](const uint8* Msg, SIZE_T MsgSize)
                {
                    if (MsgSize == 0)
                    {
                        return;
                    }
                    if (NetSimulation::HandleMessage(World, *State, bServer, Event.Connection.Value, static_cast<ENetMessage>(Msg[0]), Msg, MsgSize))
                    {
                        return;
                    }
                    switch (static_cast<ENetMessage>(Msg[0]))
                    {
                    case ENetMessage::TransformSnapshot:
                        if (!bServer)
                        {
                            ApplyTransformSnapshot(Registry, *State, Msg, MsgSize);
                        }
                        break;
                    case ENetMessage::AssignPeerId:
                        if (!bServer)
                        {
                            FNetArchive Reader(Msg, MsgSize);
                            uint8  Type   = 0;
                            uint32 PeerId = 0;
                            Reader << Type;
                            Reader << PeerId;
                            if (!Reader.HasError())
                            {
                                State->LocalPeerId  = PeerId;
                                State->PropertyAcks = FNetPropertyAcks{};
                                LOG_DISPLAY("[Net][Client] Assigned Peer ID: {}", PeerId);
                            }
                        }
                        break;
                    case ENetMessage::OwnershipUpdate:
                        if (!bServer)
                        {
                            ApplyOwnershipUpdate(Registry, *State, Msg, MsgSize);
                        }
                        break;
                    case ENetMessage::SpawnEntity:
                        if (!bServer)
                        {
                            ApplySpawnEntity(Registry, *State, Event.Connection.Value, Msg, MsgSize);
                        }
                        break;
                    case ENetMessage::DespawnEntity:
                        if (!bServer)
                        {
                            ApplyDespawnEntity(Registry, *State, Msg, MsgSize);
                        }
                        break;
                    case ENetMessage::TearOffEntity:
                        if (!bServer)
                        {
                            ApplyTearOff(Registry, World, *State, Msg, MsgSize);
                        }
                        break;
                    case ENetMessage::PropertyUpdate:
                        if (!bServer)
                        {
                            ApplyPropertyUpdate(Registry, *State, Event.Connection.Value, Msg, MsgSize);
                        }
                        break;
                    case ENetMessage::PropertyPacket:
                        if (!bServer)
                        {
                            ApplyPropertyPacket(Registry, *State, Event.Connection.Value, Msg, MsgSize);
                        }
                        break;
                    case ENetMessage::PropertyFence:
                        if (!bServer)
                        {
                            FNetArchive Reader(Msg, MsgSize);
                            uint8  Type = 0;
                            uint32 Seq  = 0;
                            Reader << Type;
                            Reader << Seq;
                            if (!Reader.HasError())
                            {
                                State->PropertyAcks.Floor = Math::Max(State->PropertyAcks.Floor, Seq);
                            }
                        }
                        break;
                    case ENetMessage::PropertyAck:
                        if (bServer)
                        {
                            uint32 Latest = 0;
                            uint32 Bits   = 0;
                            FNetClientView* View = State->ClientViews.Find(Event.Connection.Value);
                            if (View != nullptr && NetProperties::ReadAck(Msg, MsgSize, Latest, Bits))
                            {
                                State->Stats.PropertyPacketsLost += NetProperties::ApplyAck(View->Properties, Latest, Bits);
                            }
                        }
                        break;
                    case ENetMessage::ClientTransform:
                        if (bServer)
                        {
                            ApplyClientTransform(Registry, *State, Event.Connection.Value, Context.GetTime(), Msg, MsgSize);
                        }
                        break;
                    case ENetMessage::Welcome:
                        if (!bServer)
                        {
                            FNetArchive Reader(Msg, MsgSize);
                            uint8  Type  = 0;
                            uint32 Proto = 0;
                            uint16 Len   = 0;
                            Reader << Type;
                            Reader << Proto;

                            // The type table, RPC ids and field order all assume same-build peers.
                            if (Proto != Net::GetProtocolHash(State->ReplTypes))
                            {
                                LOG_ERROR("[Net][Client] Protocol mismatch (server {:#x}, client {:#x}) -- disconnecting. Client and server builds differ.",
                                    Proto, Net::GetProtocolHash(State->ReplTypes));
                                State->Transport->Disconnect(Event.Connection, /*Reason*/1, /*bForce*/false);
                                State->PendingLeaveReason = ENetLeaveReason::ProtocolMismatch;
                                break;
                            }

                            Reader << Len;
                            FString MapPath;
                            if (!Reader.HasError() && Len > 0 && Len < 4096)
                            {
                                MapPath.SetLen(Len);
                                Reader.Serialize(MapPath.GetData(), Len);
                            }
                            // The live connection is carried across the travel, and PIE clients share the server's map.
                            if (!MapPath.IsEmpty())
                            {
                                FWorldContext* CliCtx = Global::GWorldManager ? Global::GWorldManager->FindContext(World) : nullptr;
                                const FString Current = CliCtx ? CliCtx->MapPath : FString();
                                if (Current != MapPath && Global::GEngine != nullptr)
                                {
                                    LOG_DISPLAY("[Net][Client] Welcome -> loading server map '{}'", *MapPath);
                                    Global::GEngine->Travel(FStringView(*MapPath, MapPath.Len()), World);
                                }
                                else
                                {
                                    State->bWelcomed = true;
                                }
                            }
                            else
                            {
                                State->bWelcomed = true;
                            }
                        }
                        break;
                    case ENetMessage::ClientReady:
                        if (bServer)
                        {
                            // A mismatched client would corrupt the server's view, so kick it before accepting anything.
                            {
                                FNetArchive RReader(Msg, MsgSize);
                                uint8  RType  = 0;
                                uint32 RProto = 0;
                                RReader << RType;
                                RReader << RProto;
                                if (RReader.HasError() || RProto != Net::GetProtocolHash(State->ReplTypes))
                                {
                                    LOG_WARN("[Net][Server] Kicking client {}: protocol mismatch (client {:#x}, server {:#x}).",
                                        Event.Connection.Value, RProto, Net::GetProtocolHash(State->ReplTypes));
                                    State->Transport->Disconnect(Event.Connection, /*Reason*/1, /*bForce*/false);
                                    break;
                                }
                            }

                            // Catches this connection up to the current world, with ownership re-sent below.
                            State->ClientViews[Event.Connection.Value].bForceBaseline = true;
                            State->OwnershipJoiners.Add(Event.Connection.Value);

                            // Every changed stable entity's state, built before the index tables so any reference it mints goes out with them.
                            TVector<uint8> StableBaselines;
                            TVector<uint8> Message;
                            for (ECS::FEntity Ent : Registry.View<ECS::Read<SNetworkComponent>, ECS::Read<FComponentRepState>>())
                            {
                                const SNetworkComponent& StableNet = Registry.Get<ECS::Read<SNetworkComponent>>(Ent);
                                if (!StableNet.bReplicates || StableNet.NetGUID.Value == 0 || StableNet.NetGUID.Value >= NetGUID_DynamicStart)
                                {
                                    continue;
                                }
                                Message.Reset();
                                CollectBaseline(Registry, *State, Ent, StableNet.NetGUID.Value, nullptr, &Message);
                                Net::AppendFramedMessage(StableBaselines, Message.GetData(), static_cast<SIZE_T>(Message.Num()));
                            }

                            // Dynamic entities arrive through AOI, while stable ones come from the map the joiner loads.
                            auto AllIndices = [](const auto& Table)
                            {
                                TVector<uint32> Indices;
                                Indices.Reserve(Table.Num());
                                for (const auto& [Index, Value] : Table)
                                {
                                    Indices.Add(Index);
                                }
                                return Indices;
                            };
                            TVector<uint8> Exports;
                            Net::BuildObjectExport(State->OutObjects, AllIndices(State->OutObjects.IndexToGuid), Exports);
                            Net::BuildAssetExport(State->OutAssets, AllIndices(State->OutAssets.IndexToRef), Exports);
                            Net::BuildNameExport(State->OutNames, AllIndices(State->OutNames.IndexToName), Exports);
                            if (!Exports.IsEmpty())
                            {
                                State->Transport->Send(Event.Connection, Exports.GetData(), static_cast<SIZE_T>(Exports.Num()), 0, ESendMode::Reliable);
                            }

                            // The joiner loaded the level fresh, so tell it to remove copies it would have created.
                            if (!State->DestroyedStableGuids.IsEmpty())
                            {
                                TVector<uint8> DespawnBatch;
                                for (uint32 Guid : State->DestroyedStableGuids)
                                {
                                    TVector<uint8> Buffer;
                                    WriteDespawnMessage(Guid, Buffer);
                                    Net::AppendFramedMessage(DespawnBatch, Buffer.GetData(), static_cast<SIZE_T>(Buffer.Num()));
                                }
                                State->Transport->Send(Event.Connection, DespawnBatch.GetData(), static_cast<SIZE_T>(DespawnBatch.Num()), 0, ESendMode::Reliable);
                            }

                            // A joiner far from a stable entity would otherwise only catch up once it comes into view.
                            if (!StableBaselines.IsEmpty())
                            {
                                State->Transport->Send(Event.Connection, StableBaselines.GetData(), static_cast<SIZE_T>(StableBaselines.Num()), 0, ESendMode::Reliable);
                            }

                            LOG_DISPLAY("[Net][Server] Client {} ready; sent index tables", Event.Connection.Value);

                            auto& Ready = State->ReadyClientIds;
                            if (Algo::Find(Ready, Event.Connection.Value) == Ready.end())
                            {
                                Ready.Add(Event.Connection.Value);
                                SendSessionEvent(World, ENetSessionEventKind::ClientJoined, Event.Connection.Value, ENetLeaveReason::None);
                            }
                        }
                        break;
                    case ENetMessage::ObjectExport:
                        // The index space is sender-owned, and the server needs it for client-to-server object args.
                        Net::ApplyObjectExport(State->InObjects[Event.Connection.Value], Msg, MsgSize);
                        break;
                    case ENetMessage::AssetExport:
                        Net::ApplyAssetExport(State->InAssets[Event.Connection.Value], Msg, MsgSize);
                        break;
                    case ENetMessage::NameExport:
                        Net::ApplyNameExport(State->InNames[Event.Connection.Value], Msg, MsgSize);
                        break;

                    case ENetMessage::ScriptRpc:
                        ReceiveScriptRpc(Registry, *State, bServer, Event.Connection.Value, Msg, MsgSize);
                        break;
                    case ENetMessage::EventMessage:
                        NetMessages::Apply(*State, FLuminaNetworkRuntime::Get().GetScriptNetMessages(), bServer ? Event.Connection.Value : 0u, Msg, MsgSize);
                        break;
                    case ENetMessage::MoveCommands:
                        if (Physics::IPhysicsScene* Scene = World->GetPhysicsScene(); bServer && Scene != nullptr)
                        {
                            NetPrediction::ReceiveCommands(Registry, *Scene, NetPrediction::FixedStepOf(World), Event.Connection.Value, Msg, MsgSize);
                        }
                        break;
                    case ENetMessage::MoveAck:
                        if (Physics::IPhysicsScene* Scene = World->GetPhysicsScene(); !bServer && Scene != nullptr)
                        {
                            NetPrediction::ReceiveAck(Registry, *State, *Scene, NetPrediction::FixedStepOf(World), Msg, MsgSize);
                        }
                        break;

                    // The id came off the wire, so it may be anything.
                    default:
                        break;
                    }
                });
                break;
            }
            }
        }

        Clock->LatestServerTime = State->LatestServerTime;
        State->StatsReceiveMsSum += (PlatformTime::Seconds() - State->StatsTickStart) * 1000.0;

        // Roles fall out of net mode + ownership; recompute every tick (cheap, few networked entities).
        RefreshNetRoles(Registry, *State, bServer);
        NetPrediction::RefreshDrives(World, Registry, bServer);

        // Smoothing back onto the transform is done by CNetMovementInterpSystem, not here.
        EnsureRepTransforms(Registry);

        if (!bServer)
        {
            // Keep proxy physics from fighting replication; each proxy is configured once.
            ConfigureProxyPhysics(Registry, World);

            if (!State->PredictedSpawns.IsEmpty())
            {
                ExpirePredictedSpawns(Registry, *State, PlatformTime::Seconds());
            }
        }
    }
}
