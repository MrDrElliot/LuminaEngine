#include "RuntimePCH.h"
#include "NetworkSystem.h"

#include <algorithm>
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
#include "Components/RepTransformComponent.h"
#include "Net/NetReplicationGraph.h"
#include "Config/NetworkSettings.h"
#include "World/Subsystems/WorldSettings.h"
#include "World/Entity/EntityHandle.h"
#include "NetworkGlobals.h"
#include "Networking/INetworkTransport.h"
#include "Physics/PhysicsScene.h"
#include "Physics/PhysicsTypes.h"
#include "Core/Serialization/NetArchive.h"
#include "Core/Serialization/NetQuantize.h"
#include "Log/Log.h"

namespace Lumina
{
    namespace
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

        // Sorted by entity id, which deserialization recreates identically, so references match with no spawn.
        void AdoptStableNetworkEntities(entt::registry& Registry, FNetWorldState& State, bool bAuthority)
        {
            TVector<entt::entity> Networked;
            for (entt::entity Entity : Registry.view<SNetworkComponent>())
            {
                Networked.push_back(Entity);
            }

            Algo::Sort(Networked.begin(), Networked.end(), [](entt::entity A, entt::entity B)
            {
                return entt::to_integral(A) < entt::to_integral(B);
            });

            uint32 StableId = 1; // stable range is [1, NetGUID_DynamicStart)
            for (entt::entity Entity : Networked)
            {
                SNetworkComponent& Net = Registry.get<SNetworkComponent>(Entity);
                if (!Net.bNetLoadOnClient)
                {
                    continue;
                }
                Net.NetGUID            = FNetGUID{ StableId++ };
                Net.OwningConnectionId = 0;
                State.GuidTable.Register(Net.NetGUID, Entity);

                // The server tracks live stable ids so a runtime destroy can be despawned to clients.
                if (bAuthority)
                {
                    State.KnownStableGuids.push_back(Net.NetGUID.Value);
                }
            }

            LOG_DISPLAY("[Net] Adopted {} stable networked entities ({})", Networked.size(), bAuthority ? "authority" : "proxy");
        }

        // A client is AutonomousProxy for what it owns and SimulatedProxy for the rest, every tick.
        void RefreshNetRoles(entt::registry& Registry, const FNetWorldState& State, bool bServer)
        {
            LUMINA_PROFILE_SCOPE();
            for (entt::entity Entity : Registry.view<SNetworkComponent>())
            {
                SNetworkComponent& Net = Registry.get<SNetworkComponent>(Entity);
                if (bServer)
                {
                    Net.LocalRole  = ENetRole::Authority;
                    Net.RemoteRole = (Net.OwningConnectionId != 0) ? ENetRole::AutonomousProxy : ENetRole::SimulatedProxy;
                }
                else
                {
                    const bool bOwnedHere = (Net.OwningConnectionId != 0) && (Net.OwningConnectionId == State.LocalPeerId);
                    Net.LocalRole  = bOwnedHere ? ENetRole::AutonomousProxy : ENetRole::SimulatedProxy;
                    Net.RemoteRole = ENetRole::Authority;
                }
            }
        }

        // Reliable, since ownership is state-defining, and sent whenever it changes or a client joins.
        void BroadcastOwnership(entt::registry& Registry, TVector<uint8>& Batch)
        {
            LUMINA_PROFILE_SCOPE();
            auto View = Registry.view<SNetworkComponent>();
            uint16 Count = 0;
            for (entt::entity Entity : View)
            {
                if (View.get<SNetworkComponent>(Entity).bNetLoadOnClient)
                {
                    ++Count;
                }
            }
            if (Count == 0)
            {
                return;
            }

            TVector<uint8> Buffer;
            FNetArchive Writer(Buffer);
            uint8 Type = static_cast<uint8>(ENetMessage::OwnershipUpdate);
            Writer << Type;
            Writer << Count;
            for (entt::entity Entity : View)
            {
                SNetworkComponent& Net = View.get<SNetworkComponent>(Entity);
                if (!Net.bNetLoadOnClient) { continue; }
                Net::WriteNetGuid(Writer, Net.NetGUID.Value);
                WriteVarUInt(Writer, Net.OwningConnectionId);
            }
            Net::AppendFramedMessage(Batch, Buffer.data(), static_cast<SIZE_T>(Buffer.size()));
        }

        // Client, apply an ownership table. Roles refresh from it on the same tick.
        void ApplyOwnershipUpdate(entt::registry& Registry, FNetWorldState& State, const uint8* Data, SIZE_T Size)
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
                const entt::entity Entity = State.GuidTable.Find(FNetGUID{ Guid });
                if (Entity != entt::null && Registry.valid(Entity))
                {
                    if (SNetworkComponent* Net = Registry.try_get<SNetworkComponent>(Entity))
                    {
                        Net->OwningConnectionId = Owner;
                    }
                }
            }
        }


        // Carries the baseline so later updates send only changes, and is recipient-independent.
        void WriteSpawnMessage(entt::registry& Registry, FNetWorldState& State, entt::entity Entity, uint32 Guid, uint32 Owner,
            const FVector3& Pos, const FQuat& Rot, const FVector3& Scale, TVector<uint8>& OutMsg)
        {
            FNetArchive Writer(OutMsg);
            Net::BindWriters(Writer, State);
            uint8 Type = static_cast<uint8>(ENetMessage::SpawnEntity);
            Writer << Type;
            Net::WriteNetGuid(Writer, Guid);
            WriteVarUInt(Writer, Owner);

            FComponentRepState& CompDiff = Registry.get_or_emplace<FComponentRepState>(Entity);
            TVector<Net::FComponentRepOut> Comps = Net::CollectComponentFields(Registry, Entity, State, /*bBaseline*/true, &CompDiff);
            Net::WriteEntityComponents(Writer, Registry, Entity, &Comps);

            NetQuantize::FQuantizedVector::FromVector(Pos).Write(Writer);
            NetQuantize::FQuantizedQuat::FromQuat(Rot).Write(Writer);
            NetQuantize::FQuantizedVector::FromVector(Scale, NetQuantize::ScaleQuantum).Write(Writer);
        }
        
        // Runs on the game thread with synchronous signals, so the GuidTable mutation is safe.
        void OnNetworkComponentDestroyed(entt::registry& Registry, entt::entity Entity)
        {
            FNetWorldState* State = Registry.ctx().find<FNetWorldState>();
            if (State == nullptr)
            {
                return;
            }
            CWorld** WorldPtr = Registry.ctx().find<CWorld*>(); // find, since ctx may be gone during teardown
            CWorld*  World    = WorldPtr ? *WorldPtr : nullptr;
            if (World == nullptr || !IsServerMode(World->GetNetMode()))
            {
                return;
            }
            const SNetworkComponent& Net = Registry.get<SNetworkComponent>(Entity);
            if (Net.NetGUID.Value >= NetGUID_DynamicStart) // stable GUIDs handled by ReplicateStableDespawns
            {
                State->GuidTable.Unregister(Net.NetGUID);
            }
        }

        // Assigns NetGUIDs to newly-spawned dynamic entities, with destruction handled by the destroy hook.
        void MaintainDynamicLifetime(entt::registry& Registry, FNetWorldState& State)
        {
            LUMINA_PROFILE_SCOPE();
            for (auto [Entity, Net] : Registry.view<SNetworkComponent>().each())
            {
                if (Net.bNetLoadOnClient && Net.NetGUID.Value == 0)
                {
                    Net.NetGUID = State.GuidTable.AllocateDynamic();
                    State.GuidTable.Register(Net.NetGUID, Entity);
                }
            }
        }
        
        void ReplicateStableDespawns(entt::registry& Registry, FNetWorldState& State, TVector<uint8>& Batch)
        {
            LUMINA_PROFILE_SCOPE();
            for (size_t i = 0; i < State.KnownStableGuids.size(); )
            {
                const uint32 Guid = State.KnownStableGuids[i];
                const entt::entity Entity = State.GuidTable.Find(FNetGUID{ Guid });
                if (Entity == entt::null || !Registry.valid(Entity))
                {
                    TVector<uint8> Buffer;
                    FNetArchive Writer(Buffer);
                    uint8 Type = static_cast<uint8>(ENetMessage::DespawnEntity);
                    Writer << Type;
                    Net::WriteNetGuid(Writer, Guid);
                    Net::AppendFramedMessage(Batch, Buffer.data(), static_cast<SIZE_T>(Buffer.size()));
                    ++State.Stats.DespawnsSent;

                    State.GuidTable.Unregister(FNetGUID{ Guid });
                    State.DestroyedStableGuids.push_back(Guid);
                    State.KnownStableGuids.erase(State.KnownStableGuids.begin() + i);
                }
                else
                {
                    ++i;
                }
            }
        }

        // Sends the field-granular diff as one reliable broadcast, then clears the tag.
        void ReplicateDirtyProperties(entt::registry& Registry, FNetWorldState& State, double NowTime, TVector<uint8>& Batch)
        {
            LUMINA_PROFILE_SCOPE();
            const CNetworkSettings* Settings = GetDefault<CNetworkSettings>();
            const int32 BudgetBytes = Settings ? Settings->MaxReliablePropertyBytesPerTick : 0;
            const int32 PauseBytes  = Settings ? Settings->ReliableBacklogPauseBytes : 0;

            // Every dirty entity stays flagged, so nothing is lost when a saturated link pauses updates.
            if (PauseBytes > 0 && State.Transport != nullptr)
            {
                uint32 MaxBacklog = 0;
                for (uint32 ConnId : State.ConnectedClientIds)
                {
                    MaxBacklog = Math::Max(MaxBacklog, State.Transport->GetReliableBacklogBytes(FConnectionHandle{ ConnId }));
                }
                if (MaxBacklog >= static_cast<uint32>(PauseBytes))
                {
                    return;
                }
            }

            // Clearing the flag from entities that cannot replicate stops them re-evaluating every tick.
            struct FDirtyEntry { entt::entity Entity; double LastTime; uint32 Guid; };
            TVector<FDirtyEntry> Dirty;
            {
                auto View = Registry.view<SNetworkComponent, FNetDirty>();
                for (entt::entity Entity : View)
                {
                    const SNetworkComponent& Net = View.get<SNetworkComponent>(Entity);
                    if (!Net.bReplicates || Net.NetGUID.Value == 0)
                    {
                        Registry.remove<FNetDirty>(Entity);
                        continue;
                    }
                    const FComponentRepState* Cs = Registry.try_get<FComponentRepState>(Entity);
                    Dirty.push_back({ Entity, Cs ? Cs->LastReplicatedTime : 0.0, Net.NetGUID.Value });
                }
            }

            // Oldest first with a GUID tie-break, so a backlog spreads across ticks without starving one.
            Algo::Sort(Dirty.begin(), Dirty.end(), [](const FDirtyEntry& A, const FDirtyEntry& B)
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

                const entt::entity Entity = D.Entity;
                SNetworkComponent& Net = Registry.get<SNetworkComponent>(Entity);

                // Diff the native replicated component fields once (recipient-independent); updates the snapshot.
                FComponentRepState& CompDiff = Registry.get_or_emplace<FComponentRepState>(Entity);
                TVector<Net::FComponentRepOut> Comps = Net::CollectComponentFields(Registry, Entity, State, /*bBaseline*/false, &CompDiff);

                // Native component blocks are recipient-independent, so one reliable broadcast serves all clients.
                TVector<uint8> Buffer;
                FNetArchive Writer(Buffer);
                Net::BindWriters(Writer, State);
                uint8 Type = static_cast<uint8>(ENetMessage::PropertyUpdate);
                Writer << Type;
                Net::WriteNetGuid(Writer, Net.NetGUID.Value);
                Net::WriteEntityComponents(Writer, Registry, Entity, &Comps);
                const size_t Before = Batch.size();
                Net::AppendFramedMessage(Batch, Buffer.data(), static_cast<SIZE_T>(Buffer.size()));
                BytesThisTick += static_cast<int32>(Batch.size() - Before);

                CompDiff.LastReplicatedTime = NowTime; // mark sent -> drives the oldest-first fairness above
                Registry.remove<FNetDirty>(Entity);    // sent this tick; deferred entities keep the flag
                ++State.Stats.PropertyUpdatesSent;
            }

            LUMINA_PROFILE_VALUE("Net/DirtyEntities",  static_cast<int64>(Dirty.size()));
            LUMINA_PROFILE_VALUE("Net/DirtyPropBytes", static_cast<int64>(BytesThisTick));
        }
        
        // Client, create + link a server-spawned entity, then apply its components.
        void ApplySpawnEntity(entt::registry& Registry, FNetWorldState& State, uint32 SenderConn, const uint8* Data, SIZE_T Size)
        {
            LUMINA_PROFILE_SCOPE();
            FNetArchive Reader(Data, Size);
            uint8 Type = 0;
            Reader << Type;
            const uint32 Guid  = Net::ReadNetGuid(Reader);
            const uint32 Owner = ReadVarUInt(Reader);
            if (Reader.HasError() || State.GuidTable.Find(FNetGUID{ Guid }) != entt::null)
            {
                return; // malformed, or we already have this entity
            }

            Net::BindReaders(Reader, State, SenderConn);
            const entt::entity Entity = Registry.create();
            Net::ReadEntityComponents(Reader, Registry, Entity);

            // Spawn pose
            NetQuantize::FQuantizedVector QPos;   
            QPos.Read(Reader);
            NetQuantize::FQuantizedQuat   QRot;   
            QRot.Read(Reader);
            NetQuantize::FQuantizedVector QScale; 
            QScale.Read(Reader);
            
            if (!Reader.HasError())
            {
                if (STransformComponent* T = Registry.try_get<STransformComponent>(Entity))
                {
                    T->SetRaw(QPos.ToVector(), QRot.ToQuat(), QScale.ToVector(NetQuantize::ScaleQuantum));
                }
            }

            SNetworkComponent& Net = Registry.get_or_emplace<SNetworkComponent>(Entity);
            Net.NetGUID            = FNetGUID{ Guid };
            Net.OwningConnectionId = Owner;
            State.GuidTable.Register(Net.NetGUID, Entity);
            Registry.emplace_or_replace<FNeedsTransformUpdate>(Entity);
            // Now that this entity's NetGUID is registered, reparent any children that were waiting on it.
            Net::DrainPendingAttach(Registry, State, Guid, Entity);
        }

        // Client, apply a reliable property delta to an existing entity.
        void ApplyPropertyUpdate(entt::registry& Registry, FNetWorldState& State, uint32 SenderConn, const uint8* Data, SIZE_T Size)
        {
            LUMINA_PROFILE_SCOPE();
            FNetArchive Reader(Data, Size);
            uint8 Type = 0;
            Reader << Type;
            const uint32 Guid = Net::ReadNetGuid(Reader);
            if (Reader.HasError())
            {
                return;
            }
            const entt::entity Entity = State.GuidTable.Find(FNetGUID{ Guid });
            if (Entity != entt::null && Registry.valid(Entity))
            {
                Net::BindReaders(Reader, State, SenderConn);
                Net::ReadEntityComponents(Reader, Registry, Entity);
                Registry.emplace_or_replace<FNeedsTransformUpdate>(Entity);
            }
        }

        // Client, destroy a despawned entity.
        void ApplyDespawnEntity(CWorld* World, FNetWorldState& State, const uint8* Data, SIZE_T Size)
        {
            FNetArchive Reader(Data, Size);
            uint8 Type = 0;
            Reader << Type;
            const uint32 Guid = Net::ReadNetGuid(Reader);
            if (Reader.HasError())
            {
                return;
            }
            entt::registry& Registry = ECS::GetWorldRegistry(*World);
            const entt::entity Entity = State.GuidTable.Find(FNetGUID{ Guid });
            if (Entity != entt::null && Registry.valid(Entity))
            {
                State.GuidTable.Unregister(FNetGUID{ Guid });
                ECS::Utils::DestroyEntity(Registry, Entity);
            }
        }
        
        void ConfigureProxyPhysics(entt::registry& Registry, CWorld* World)
        {
            Physics::IPhysicsScene* PhysScene = World->GetPhysicsScene();
            if (PhysScene == nullptr)
            {
                return;
            }

            for (entt::entity Entity : Registry.view<SNetworkComponent>())
            {
                SNetworkComponent& Net = Registry.get<SNetworkComponent>(Entity);
                const bool bIsProxy = Net.LocalRole == ENetRole::SimulatedProxy || Net.LocalRole == ENetRole::AutonomousProxy;
                if (Net.bProxyPhysicsConfigured || !bIsProxy)
                {
                    continue;
                }

                const uint32 BodyID = PhysScene->GetEntityBodyID(Entity);
                if (BodyID != ~0u) // body created yet?
                {
                    PhysScene->ChangeBodyMotionType(BodyID, EBodyType::Kinematic);
                    Net.bProxyPhysicsConfigured = true;
                }
            }
        }

        // Coarser the farther away, and both peers derive the quantum from the wire tier.
        double TierPosQuantum(ENetLODTier Tier)
        {
            switch (Tier)
            {
            case ENetLODTier::Near: return 0.001; // 1 mm
            case ENetLODTier::Mid:  return 0.01;  // 1 cm
            default:                return 0.1;   // 10 cm (Far)
            }
        }

        // The wire encoding is chosen by tier at write time and mirrored by tier at read time.
        struct FTransformSendRecord
        {
            uint32      Guid = 0;
            ENetLODTier Tier = ENetLODTier::Near;
            FVector3    Pos;
            FQuat       Rot;
            bool        bScale = false;
            FVector3    Scale  = FVector3(1.0f);
        };

        void WriteTransformRecord(FNetArchive& Writer, const FTransformSendRecord& R)
        {
            uint8 Tier = static_cast<uint8>(R.Tier);
            Writer.SerializeBits(&Tier, 2);
            Net::WriteNetGuid(Writer, R.Guid);

            NetQuantize::FQuantizedVector::FromVector(R.Pos, TierPosQuantum(R.Tier)).Write(Writer);

            if (R.Tier == ENetLODTier::Far)
            {
                // Yaw-only rotation in one byte, since far entities need no full orientation fidelity.
                const float Yaw = Math::EulerAngles(R.Rot).y; // radians, around up (Y)
                float Norm = Yaw / 6.2831853f + 0.5f;
                Norm = Norm < 0.0f ? 0.0f : (Norm > 1.0f ? 1.0f : Norm);
                uint8 YawByte = static_cast<uint8>(Norm * 255.0f + 0.5f);
                Writer << YawByte;
            }
            else
            {
                NetQuantize::FQuantizedQuat::FromQuat(R.Rot).Write(Writer);
            }

            bool bScale = R.bScale;
            Writer.SerializeBit(bScale); // 1-bit has-scale flag
            if (bScale)
            {
                NetQuantize::FQuantizedVector::FromVector(R.Scale, NetQuantize::ScaleQuantum).Write(Writer);
            }
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
        void EnsureRepTransforms(entt::registry& Registry)
        {
            LUMINA_PROFILE_SCOPE();
            for (entt::entity Entity : Registry.view<SNetworkComponent>())
            {
                SNetworkComponent& Net = Registry.get<SNetworkComponent>(Entity);
                const bool bQualifies = Net.bReplicates && Net.bReplicatesMovement && Net.bNetLoadOnClient
                    && Registry.all_of<STransformComponent>(Entity);
                const bool bHas = Registry.all_of<FRepTransform>(Entity);

                if (bQualifies && !bHas)
                {
                    Registry.emplace<FRepTransform>(Entity).NetGUID = Net.NetGUID.Value;
                }
                else if (!bQualifies && bHas)
                {
                    Registry.remove<FRepTransform>(Entity);
                }
                else if (bQualifies)
                {
                    Registry.get<FRepTransform>(Entity).NetGUID = Net.NetGUID.Value;
                }
            }
        }
        
        void ServerReplicateRelevant(entt::registry& Registry, FNetWorldState& State,
            const SDefaultWorldSettings& Settings, float DeltaTime,
            float ServerTime, TVector<uint8>& ReliableBroadcast)
        {
            LUMINA_PROFILE_SCOPE();
            ++State.RelevancyTick; // relevancy generation stamp for this tick
            NetGraph::BuildExtract(Registry, State.Extract, State.OwnerToRecord);
            NetGraph::BuildGrid(State.Extract, Settings, State.Grid);

            const FNetExtract& Ex   = State.Extract;
            const FNetGrid&    Grid = State.Grid;
            const float Grace      = Settings.RelevancyGraceSeconds;
            const float EnterR2    = Settings.AOIEnterRadius * Settings.AOIEnterRadius;
            const int32 CellRadius = static_cast<int32>(Settings.AOILeaveRadius / Grid.CellSize) + 1;

            constexpr SIZE_T HeaderBytes        = 7;
            constexpr SIZE_T MaxRecordBytes     = 5 + 30 + 6 + 1 + 30;
            constexpr SIZE_T MaxRecordsPerFrame = (Net::MaxFramedMessageSize - HeaderBytes) / MaxRecordBytes;
            static_assert(MaxRecordsPerFrame >= 1 && MaxRecordsPerFrame <= 0xFFFF, "frame chunk size invalid");

            const uint32 NumClients = static_cast<uint32>(State.ConnectedClientIds.size());
            TVector<TVector<uint8>>                ClientReliable; ClientReliable.resize(NumClients);
            TVector<TVector<FTransformSendRecord>> ClientRecords;  ClientRecords.resize(NumClients);

            uint32 RelevantSum = 0, RelevantMax = 0, SpawnsSum = 0, DespawnsSum = 0;

            // Copied from the grid's cell-ordered arrays so the diff reads hot fields contiguously.
            struct FGathered { uint32 Rec; uint32 Guid; uint8 Flags; ENetLODTier Tier; };
            TVector<FGathered> Gathered;

            // Phase A, the per-client gather, relevancy diff and spawn, despawn and transform records.
            for (uint32 ci = 0; ci < NumClients; ++ci)
            {
                LUMINA_PROFILE_SECTION("Relevancy/Client");
                const uint32 ConnId = State.ConnectedClientIds[ci];
                FNetClientView& CV  = State.ClientViews[ConnId];

                const auto VpIt = State.OwnerToRecord.find(ConnId);
                if (VpIt == State.OwnerToRecord.end()) { continue; } // no owned pawn yet -> nothing relevant
                const FVector3 VP = Ex.WorldPos[VpIt->second]; // viewpoint in world space

                Gathered.clear();
                int32 Vcx, Vcz;
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
                            Gathered.push_back({ Grid.SortedRecords[s], Grid.SortedGuid[s], Flags, Tier });
                        }
                    }
                }

                // An entry is "relevant this tick" iff its RelevantTick == State.RelevancyTick.
                TVector<uint8>&                Reliable = ClientReliable[ci];
                TVector<FTransformSendRecord>& Records  = ClientRecords[ci];

                // A single pass doing the relevancy diff on enter and the transform-record build.
                for (const FGathered& G : Gathered)
                {
                    const uint32 Rec   = G.Rec;
                    const uint32 Guid  = G.Guid;
                    const uint8  Flags = G.Flags;
                    const bool   bDyn  = (Flags & NETREC_Dynamic) != 0;

                    FRelevantEntry* EntryPtr;
                    auto It = CV.Relevant.find(Guid);
                    if (It == CV.Relevant.end())
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
                            const entt::entity Ent = State.GuidTable.Find(FNetGUID{ Guid });
                            if (Ent != entt::null)
                            {
                                TVector<uint8> Buf;
                                const FVector3 SpawnPos   = Ex.Pos[Rec];
                                const FQuat    SpawnRot   = Ex.Rot[Rec].ToQuat();
                                const FVector3 SpawnScale = Ex.Scale[Rec].ToVector(NetQuantize::ScaleQuantum);
                                WriteSpawnMessage(Registry, State, Ent, Guid, Ex.OwnerConn[Rec], SpawnPos, SpawnRot, SpawnScale, Buf);
                                Net::AppendFramedMessage(Reliable, Buf.data(), static_cast<SIZE_T>(Buf.size()));
                                ++SpawnsSum;
                                E.bBaselinePending = true; // spawn carried the pose; hold the transform one tick
                            }
                        }
                        else
                        {
                            E.bNeedsBaseline = true; // the client has it from the map, so send the current pose once

                            // Refs minted here are flushed by the export step before this reliable batch.
                            const entt::entity Ent = State.GuidTable.Find(FNetGUID{ Guid });
                            if (Ent != entt::null && Registry.valid(Ent) && Registry.any_of<FComponentRepState>(Ent))
                            {
                                FComponentRepState& CDiff = Registry.get_or_emplace<FComponentRepState>(Ent);
                                TVector<Net::FComponentRepOut> Comps = Net::CollectComponentFields(Registry, Ent, State, /*bBaseline*/true, &CDiff);

                                TVector<uint8> Buf;
                                FNetArchive W(Buf);
                                Net::BindWriters(W, State);
                                uint8 PType = static_cast<uint8>(ENetMessage::PropertyUpdate);
                                W << PType;
                                Net::WriteNetGuid(W, Guid);
                                Net::WriteEntityComponents(W, Registry, Ent, &Comps);
                                Net::AppendFramedMessage(Reliable, Buf.data(), static_cast<SIZE_T>(Buf.size()));
                            }
                        }
                        EntryPtr = &CV.Relevant.emplace(Guid, E).first->second; // used immediately, before any rehash
                    }
                    else
                    {
                        It->second.RelevantTick = State.RelevancyTick;
                        It->second.Tier         = G.Tier;
                        It->second.TimeOutOfAOI = 0.0f;
                        EntryPtr = &It->second;
                    }

                    // Transform stream (movement entities only; a non-movement pose rode the spawn baseline).
                    if (Flags & NETREC_Movement)
                    {
                        FRelevantEntry& E = *EntryPtr;
                        E.TimeSinceSent += DeltaTime;
                        if (E.bBaselinePending)
                        {
                            E.bBaselinePending = false; // spawn carried the pose; hold the transform one tick
                        }
                        else
                        {
                            const bool bChanged  = (Flags & NETREC_Changed) != 0;
                            const bool bScaleChg = (Flags & NETREC_ScaleChanged) != 0;
                            const bool bBaseline = CV.bForceBaseline || E.bNeedsBaseline;

                            // Near sends every changed tick, Mid and Far throttle to tier rate, and a baseline bypasses.
                            float Period = 0.0f;
                            if (E.Tier == ENetLODTier::Mid)      { Period = (Settings.TierMidRate > 0.0f) ? 1.0f / Settings.TierMidRate : 0.0f; }
                            else if (E.Tier == ENetLODTier::Far) { Period = (Settings.TierFarRate > 0.0f) ? 1.0f / Settings.TierFarRate : 0.0f; }
                            const bool bCadence = (Period <= 0.0f) || (E.TimeSinceSent >= Period);

                            if (bBaseline || (bChanged && bCadence))
                            {
                                E.bNeedsBaseline = false;
                                E.TimeSinceSent  = 0.0f;
                                FTransformSendRecord R;
                                R.Guid   = Guid;
                                R.Tier   = E.Tier;
                                R.Pos    = Ex.Pos[Rec];
                                R.Rot    = Ex.Rot[Rec].ToQuat();
                                R.bScale = bScaleChg || bBaseline;
                                R.Scale  = Ex.Scale[Rec].ToVector(NetQuantize::ScaleQuantum);
                                Records.push_back(R);
                            }
                        }
                    }
                }

                // Expire entries not seen relevant this tick (left the AOI / destroyed) -> grace then despawn.
                for (auto It = CV.Relevant.begin(); It != CV.Relevant.end(); )
                {
                    FRelevantEntry& E = It->second;
                    if (E.RelevantTick != State.RelevancyTick)
                    {
                        const uint32 Guid = It->first;
                        const bool bDestroyed = (State.GuidTable.Find(FNetGUID{ Guid }) == entt::null);
                        E.TimeOutOfAOI += DeltaTime;
                        if (bDestroyed || E.TimeOutOfAOI >= Grace)
                        {
                            if (E.bDynamic)
                            {
                                TVector<uint8> Buf;
                                FNetArchive W(Buf);
                                uint8 Type = static_cast<uint8>(ENetMessage::DespawnEntity);
                                W << Type;
                                Net::WriteNetGuid(W, Guid);
                                Net::AppendFramedMessage(Reliable, Buf.data(), static_cast<SIZE_T>(Buf.size()));
                                ++DespawnsSum;
                            }
                            It = CV.Relevant.erase(It);
                            continue;
                        }
                    }
                    ++It;
                }

                CV.bForceBaseline = false;
                RelevantSum += static_cast<uint32>(CV.Relevant.size());
                RelevantMax  = Math::Max(RelevantMax, static_cast<uint32>(CV.Relevant.size()));
            }

            LUMINA_PROFILE_VALUE("Net/RelevantMax", static_cast<int64>(RelevantMax));
            LUMINA_PROFILE_VALUE("Net/Spawns",      static_cast<int64>(SpawnsSum));
            LUMINA_PROFILE_VALUE("Net/Despawns",    static_cast<int64>(DespawnsSum));

            // Exports first, so every client learns an index before any spawn that references it.
            if (!State.OutObjects.PendingExports.empty())
            {
                TVector<uint8> ExportMsg;
                Net::BuildObjectExport(State.OutObjects, State.OutObjects.PendingExports, ExportMsg);
                Net::BroadcastFramed(*State.Transport, ExportMsg.data(), static_cast<SIZE_T>(ExportMsg.size()), 0, ESendMode::Reliable);
                State.OutObjects.PendingExports.clear();
            }
            if (!State.OutAssets.PendingExports.empty())
            {
                TVector<uint8> ExportMsg;
                Net::BuildAssetExport(State.OutAssets, State.OutAssets.PendingExports, ExportMsg);
                Net::BroadcastFramed(*State.Transport, ExportMsg.data(), static_cast<SIZE_T>(ExportMsg.size()), 0, ESendMode::Reliable);
                State.OutAssets.PendingExports.clear();
            }
            if (!State.OutNames.PendingExports.empty())
            {
                TVector<uint8> ExportMsg;
                Net::BuildNameExport(State.OutNames, State.OutNames.PendingExports, ExportMsg);
                Net::BroadcastFramed(*State.Transport, ExportMsg.data(), static_cast<SIZE_T>(ExportMsg.size()), 0, ESendMode::Reliable);
                State.OutNames.PendingExports.clear();
            }
            if (!ReliableBroadcast.empty())
            {
                State.Stats.ReliableBatchBytes = static_cast<uint32>(ReliableBroadcast.size());
                State.Transport->Broadcast(ReliableBroadcast.data(), static_cast<SIZE_T>(ReliableBroadcast.size()), 0, ESendMode::Reliable);
            }

            // Phase B, flushing per-client reliable spawns and despawns, then chunked unreliable transforms.
            uint32 LargestFrame = 0, TotalBytes = 0, Chunks = 0;
            for (uint32 ci = 0; ci < NumClients; ++ci)
            {
                const FConnectionHandle Conn{ State.ConnectedClientIds[ci] };
                TVector<uint8>& Reliable = ClientReliable[ci];
                if (!Reliable.empty())
                {
                    State.Transport->Send(Conn, Reliable.data(), static_cast<SIZE_T>(Reliable.size()), 0, ESendMode::Reliable);
                }

                // Owner-conditioned PropertyUpdates deferred from ReplicateDirtyProperties, now that exports went out.
                const auto PcrIt = State.PendingClientReliable.find(State.ConnectedClientIds[ci]);
                if (PcrIt != State.PendingClientReliable.end() && !PcrIt->second.empty())
                {
                    State.Transport->Send(Conn, PcrIt->second.data(), static_cast<SIZE_T>(PcrIt->second.size()), 0, ESendMode::Reliable);
                }

                TVector<FTransformSendRecord>& Records = ClientRecords[ci];
                if (Records.empty()) { continue; }

                TVector<uint8> Unreliable;
                for (size_t Begin = 0; Begin < Records.size(); Begin += MaxRecordsPerFrame)
                {
                    const size_t End = Math::Min(Records.size(), Begin + MaxRecordsPerFrame);
                    TVector<uint8> Buffer;
                    FNetArchive Writer(Buffer);
                    uint8  Type  = static_cast<uint8>(ENetMessage::TransformSnapshot);
                    uint16 Count = static_cast<uint16>(End - Begin);
                    Writer << Type;
                    Writer << ServerTime;
                    Writer << Count;
                    for (size_t i = Begin; i < End; ++i) { WriteTransformRecord(Writer, Records[i]); }
                    const uint32 FrameBytes = static_cast<uint32>(Buffer.size());
                    LargestFrame = Math::Max(LargestFrame, FrameBytes);
                    TotalBytes  += FrameBytes;
                    ++Chunks;
                    Net::AppendFramedMessage(Unreliable, Buffer.data(), static_cast<SIZE_T>(Buffer.size()));
                }
                State.Transport->Send(Conn, Unreliable.data(), static_cast<SIZE_T>(Unreliable.size()), 0, ESendMode::UnreliableSequenced);
            }

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

            State.PendingClientReliable.clear(); // consumed this tick
        }

        // Client -> server. Push the pose of entities this client owns (AutonomousProxy) upstream.
        void SendOwnedTransforms(entt::registry& Registry, FNetWorldState& State, float DeltaTime)
        {
            auto&& TransformStorage = Registry.storage<STransformComponent>();
            auto View = Registry.view<SNetworkComponent, FRepTransform>();

            TVector<FTransformSendRecord> Records;
            for (entt::entity Entity : View)
            {
                SNetworkComponent& Net = View.get<SNetworkComponent>(Entity);
                FRepTransform&     Rep = View.get<FRepTransform>(Entity);
                if (Net.LocalRole != ENetRole::AutonomousProxy || !Net.bReplicatesMovement)
                {
                    continue;
                }
                if (!TransformStorage.contains(Entity))
                {
                    continue;
                }
                STransformComponent& T = TransformStorage.get(Entity);

                bool bScale = false;
                if (!PrepareTransformSend(Net, Rep, T.GetLocalLocation(), T.GetLocalRotation(), T.GetLocalScale(), DeltaTime, false, bScale))
                {
                    continue;
                }
                FTransformSendRecord R;
                R.Guid   = Net.NetGUID.Value;
                R.Tier   = ENetLODTier::Near; // the owner's own pawn -> full precision upstream
                R.Pos    = T.GetLocalLocation();
                R.Rot    = T.GetLocalRotation();
                R.bScale = bScale;
                R.Scale  = T.GetLocalScale();
                Records.push_back(R);
            }

            if (Records.empty())
            {
                return;
            }

            TVector<uint8> Buffer;
            FNetArchive Writer(Buffer);
            uint8  Type  = static_cast<uint8>(ENetMessage::ClientTransform);
            uint16 Count = static_cast<uint16>(Records.size());
            Writer << Type;
            Writer << Count;
            for (const FTransformSendRecord& R : Records)
            {
                WriteTransformRecord(Writer, R);
            }
            Net::SendFramed(*State.Transport, State.ServerConnection, Buffer.data(), static_cast<SIZE_T>(Buffer.size()), 0, ESendMode::UnreliableSequenced);
        }

        // Returns false on a decode error so the caller breaks the batch.
        bool ReadTransformIntoRing(entt::registry& Registry, FNetWorldState& State, FNetArchive& Reader, double SampleTime, bool bSkipAutonomous, uint32 OwnerGate)
        {
            // Decode mirrors WriteTransformRecord, where the tier byte selects quantum and precision.
            uint8 TierByte = 0;
            Reader.SerializeBits(&TierByte, 2);
            const ENetLODTier Tier = static_cast<ENetLODTier>(TierByte);
            const uint32 Guid = Net::ReadNetGuid(Reader);

            NetQuantize::FQuantizedVector QPos;
            QPos.Read(Reader);
            const FVector3 Pos = QPos.ToVector(TierPosQuantum(Tier));

            FQuat Rot;
            if (Tier == ENetLODTier::Far)
            {
                uint8 YawByte = 0;
                Reader << YawByte;
                const float Yaw = (static_cast<float>(YawByte) / 255.0f - 0.5f) * 6.2831853f;
                Rot = FQuat(FVector3(0.0f, Yaw, 0.0f));
            }
            else
            {
                NetQuantize::FQuantizedQuat QRot;
                QRot.Read(Reader);
                Rot = QRot.ToQuat();
            }

            bool bScale = false;
            Reader.SerializeBit(bScale);
            NetQuantize::FQuantizedVector QScale;
            if (bScale)
            {
                QScale.Read(Reader);
            }
            if (Reader.HasError())
            {
                return false;
            }

            const entt::entity Entity = State.GuidTable.Find(FNetGUID{ Guid });
            if (Entity == entt::null || !Registry.valid(Entity) || !Registry.all_of<STransformComponent>(Entity))
            {
                return true; // unknown/late entity; skip but keep parsing the batch
            }

            SNetworkComponent* Net = Registry.try_get<SNetworkComponent>(Entity);
            // The client skips entities it controls, since the server pose would be stale.
            if (bSkipAutonomous && Net != nullptr && Net->LocalRole == ENetRole::AutonomousProxy)
            {
                return true;
            }
            // The server only accepts poses for entities the sender actually owns.
            if (OwnerGate != 0 && (Net == nullptr || Net->OwningConnectionId != OwnerGate))
            {
                return true;
            }

            FRepTransform& Rep = Registry.get_or_emplace<FRepTransform>(Entity);
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
        void ApplyTransformSnapshot(entt::registry& Registry, FNetWorldState& State, const uint8* Data, SIZE_T Size)
        {
            LUMINA_PROFILE_SCOPE();
            FNetArchive Reader(Data, Size);
            uint8 Type = 0;
            Reader << Type; // ENetMessage::TransformSnapshot (already routed by the caller)
            float ServerTime = 0.0f;
            Reader << ServerTime;
            uint16 Count = 0;
            Reader << Count;

            const double SampleTime = static_cast<double>(ServerTime);
            State.LatestServerTime = Math::Max(SampleTime, State.LatestServerTime);

            for (uint16 Index = 0; Index < Count; ++Index)
            {
                if (!ReadTransformIntoRing(Registry, State, Reader, SampleTime, /*bSkipAutonomous*/ true, /*OwnerGate*/ 0))
                {
                    break;
                }
            }
        }

        // Server, buffer each client-owned pose into its ring (gated on ownership). Relayed raw next snapshot.
        void ApplyClientTransform(entt::registry& Registry, FNetWorldState& State, FConnectionHandle Sender, double ServerNow, const uint8* Data, SIZE_T Size)
        {
            FNetArchive Reader(Data, Size);
            uint8  Type  = 0;
            uint16 Count = 0;
            Reader << Type;
            Reader << Count;

            State.LatestServerTime = Math::Max(ServerNow, State.LatestServerTime);

            for (uint16 i = 0; i < Count; ++i)
            {
                if (!ReadTransformIntoRing(Registry, State, Reader, ServerNow, /*bSkipAutonomous*/ false, /*OwnerGate*/ Sender.Value))
                {
                    break;
                }
            }
        }
    }

    void SNetworkSystem::Update(const FSystemContext& Context) noexcept
    {
        LUMINA_PROFILE_SCOPE();
        
        entt::registry& Registry = Context.GetRegistry();

        CWorld* World = Registry.ctx().get<CWorld*>();
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
        FNetWorldState* State = Registry.ctx().find<FNetWorldState>();
        if (State == nullptr)
        {
            State = &Registry.ctx().emplace<FNetWorldState>();
            LOG_DISPLAY("[Net] World '{}' net mode = {}", World->GetName().c_str(), NetModeToString(NetMode));
        }

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

            // Data-driven from FWorldContext, defaulting to loopback on port 7777.
            FWorldContext* WorldCtx = GWorldManager ? GWorldManager->FindContext(World) : nullptr;
            const FString  Host = WorldCtx ? WorldCtx->NetHost : FString("127.0.0.1");
            const uint16   Port = WorldCtx ? WorldCtx->NetPort : NetDefaultPort;

            if (bServer)
            {
                // Unregister a destroyed entity's dynamic GUID the instant it dies (server only, connected once).
                Registry.on_destroy<SNetworkComponent>().connect<&OnNetworkComponentDestroyed>();

                State->Transport.reset(Network::CreateTransport());

                FListenParams Params;
                Params.Port           = Port;
                Params.MaxConnections = 8;
                Params.ChannelCount   = 2;

                if (State->Transport->StartServer(Params))
                {
                    LOG_DISPLAY("[Net] Listen server started on port {}", Port);
                }
                else
                {
                    LOG_ERROR("[Net] Failed to start listen server on port {}", Port);
                }
            }
            else if (GEngine != nullptr && GEngine->HasCarriedConnection())
            {
                // Welcome-driven travel handed us the already-connected transport from the previous world.
                FConnectionHandle Conn;
                uint32 PeerId = ServerPeerId;
                State->Transport        = GEngine->TakeCarriedConnection(Conn, PeerId);
                State->ServerConnection = Conn;
                State->LocalPeerId      = PeerId;
                State->bClientConnected = true;
                LOG_DISPLAY("[Net] Client adopted carried connection after travel (peer id {})", PeerId);
            }
            else // Client, open a fresh connection.
            {
                State->Transport.reset(Network::CreateTransport());

                FConnectParams Params;
                Params.Address.Host = Host;
                Params.Address.Port = Port;
                Params.ChannelCount = 2;

                State->ServerConnection = State->Transport->ConnectToServer(Params);
                LOG_DISPLAY("[Net] Client connecting to {}:{}...", Host.c_str(), Port);
            }
        }

        if (State->Transport == nullptr)
        {
            return;
        }

        // Reuse the events buffer across ticks; Service appends, so clear it first.
        TVector<FNetworkEvent>& Events = State->ServiceEvents;
        Events.clear();
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
                    State->ConnectedClientIds.push_back(Event.Connection.Value);
                    State->ClientViews[Event.Connection.Value] = FNetClientView{}; // bForceBaseline defaults true
                    State->bOwnershipDirty = true; // (re)send the current ownership table

                    // Tell the new client its unique peer id (= its connection handle here).
                    TVector<uint8> Buffer;
                    FNetArchive Writer(Buffer);
                    uint8  Type   = static_cast<uint8>(ENetMessage::AssignPeerId);
                    uint32 PeerId = Event.Connection.Value;
                    Writer << Type;
                    Writer << PeerId;
                    Net::SendFramed(*State->Transport, Event.Connection, Buffer.data(), static_cast<SIZE_T>(Buffer.size()), 0, ESendMode::Reliable);

                    // Welcome, tell the client which map we're running so it can load the same level.
                    if (FWorldContext* SrvCtx = GWorldManager ? GWorldManager->FindContext(World) : nullptr)
                    {
                        const FString& MapPath = SrvCtx->MapPath;
                        TVector<uint8> WelcomeBuf;
                        FNetArchive WWriter(WelcomeBuf);
                        uint8  WType  = static_cast<uint8>(ENetMessage::Welcome);
                        uint32 WProto = Net::GetProtocolHash();
                        uint16 Len    = static_cast<uint16>(MapPath.size());
                        WWriter << WType;
                        WWriter << WProto; // client refuses the connection on a protocol/build mismatch
                        WWriter << Len;
                        if (Len > 0)
                        {
                            WWriter.Serialize(const_cast<char*>(MapPath.data()), Len);
                        }
                        Net::SendFramed(*State->Transport, Event.Connection, WelcomeBuf.data(), static_cast<SIZE_T>(WelcomeBuf.size()), 0, ESendMode::Reliable);
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
                    Ids.erase(Algo::Remove(Ids.begin(), Ids.end(), Event.Connection.Value), Ids.end());
                    State->ClientViews.erase(Event.Connection.Value); // drop its per-client relevancy state

                    // Release anything this connection owned so it doesn't stay stuck as an orphan proxy.
                    for (entt::entity Entity : Registry.view<SNetworkComponent>())
                    {
                        SNetworkComponent& Net = Registry.get<SNetworkComponent>(Entity);
                        if (Net.OwningConnectionId == Event.Connection.Value)
                        {
                            Net.OwningConnectionId = 0;
                            State->bOwnershipDirty  = true;
                        }
                    }
                    LOG_DISPLAY("[Net][Server] Client {} disconnected ({} total)", Event.Connection.Value, State->ConnectedClients);
                }
                else
                {
                    State->bClientConnected = false;
                    State->LocalPeerId      = ServerPeerId;
                    LOG_DISPLAY("[Net][Client] Disconnected from server");
                }
                break;

            case ENetworkEventType::Data:
            {
                // Every packet is a batch of length-framed messages; split + dispatch each by its type byte.
                Net::ForEachFramedMessage(Event.Data.data(), Event.Data.size(), [&](const uint8* Msg, SIZE_T MsgSize)
                {
                    if (MsgSize == 0)
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
                                State->LocalPeerId = PeerId;
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
                            ApplyDespawnEntity(World, *State, Msg, MsgSize);
                        }
                        break;
                    case ENetMessage::PropertyUpdate:
                        if (!bServer)
                        {
                            ApplyPropertyUpdate(Registry, *State, Event.Connection.Value, Msg, MsgSize);
                        }
                        break;
                    case ENetMessage::ClientTransform:
                        if (bServer)
                        {
                            ApplyClientTransform(Registry, *State, Event.Connection, Context.GetTime(), Msg, MsgSize);
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
                            if (Proto != Net::GetProtocolHash())
                            {
                                LOG_ERROR("[Net][Client] Protocol mismatch (server {:#x}, client {:#x}) -- disconnecting. Client and server builds differ.",
                                    Proto, Net::GetProtocolHash());
                                State->Transport->Disconnect(Event.Connection, /*Reason*/1, /*bForce*/false);
                                break;
                            }

                            Reader << Len;
                            FString MapPath;
                            if (!Reader.HasError() && Len > 0 && Len < 4096)
                            {
                                MapPath.resize(Len);
                                Reader.Serialize(MapPath.data(), Len);
                            }
                            // The live connection is carried across the travel, and PIE clients share the server's map.
                            if (!MapPath.empty())
                            {
                                FWorldContext* CliCtx = GWorldManager ? GWorldManager->FindContext(World) : nullptr;
                                const FString Current = CliCtx ? CliCtx->MapPath : FString();
                                if (Current != MapPath && GEngine != nullptr)
                                {
                                    LOG_DISPLAY("[Net][Client] Welcome -> loading server map '{}'", MapPath.c_str());
                                    GEngine->Travel(FStringView(MapPath.c_str(), MapPath.size()));
                                }
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
                                if (RReader.HasError() || RProto != Net::GetProtocolHash())
                                {
                                    LOG_WARN("[Net][Server] Kicking client {}: protocol mismatch (client {:#x}, server {:#x}).",
                                        Event.Connection.Value, RProto, Net::GetProtocolHash());
                                    State->Transport->Disconnect(Event.Connection, /*Reason*/1, /*bForce*/false);
                                    break;
                                }
                            }

                            // Catches this connection up to the current world, with ownership re-sent below.
                            State->ClientViews[Event.Connection.Value].bForceBaseline = true;
                            State->bOwnershipDirty = true;

                            // Dynamic entities arrive through AOI, while stable ones come from the map the joiner loads.
                            if (!State->OutObjects.IndexToGuid.empty())
                            {
                                TVector<uint32> AllIndices;
                                AllIndices.reserve(State->OutObjects.IndexToGuid.size());
                                for (const auto& Pair : State->OutObjects.IndexToGuid)
                                {
                                    AllIndices.push_back(Pair.first);
                                }
                                TVector<uint8> ExportMsg;
                                Net::BuildObjectExport(State->OutObjects, AllIndices, ExportMsg);
                                Net::SendFramed(*State->Transport, Event.Connection, ExportMsg.data(), static_cast<SIZE_T>(ExportMsg.size()), 0, ESendMode::Reliable);
                            }

                            if (!State->OutAssets.IndexToRef.empty()) // asset index table for the joiner
                            {
                                TVector<uint32> AllIndices;
                                AllIndices.reserve(State->OutAssets.IndexToRef.size());
                                for (const auto& Pair : State->OutAssets.IndexToRef)
                                {
                                    AllIndices.push_back(Pair.first);
                                }
                                TVector<uint8> ExportMsg;
                                Net::BuildAssetExport(State->OutAssets, AllIndices, ExportMsg);
                                Net::SendFramed(*State->Transport, Event.Connection, ExportMsg.data(), static_cast<SIZE_T>(ExportMsg.size()), 0, ESendMode::Reliable);
                            }

                            if (!State->OutNames.IndexToName.empty()) // name index table for the joiner
                            {
                                TVector<uint32> AllIndices;
                                AllIndices.reserve(State->OutNames.IndexToName.size());
                                for (const auto& Pair : State->OutNames.IndexToName)
                                {
                                    AllIndices.push_back(Pair.first);
                                }
                                TVector<uint8> ExportMsg;
                                Net::BuildNameExport(State->OutNames, AllIndices, ExportMsg);
                                Net::SendFramed(*State->Transport, Event.Connection, ExportMsg.data(), static_cast<SIZE_T>(ExportMsg.size()), 0, ESendMode::Reliable);
                            }

                            // The joiner loaded the level fresh, so tell it to remove copies it would have created.
                            if (!State->DestroyedStableGuids.empty())
                            {
                                TVector<uint8> DespawnBatch;
                                for (uint32 Guid : State->DestroyedStableGuids)
                                {
                                    TVector<uint8> Buffer;
                                    FNetArchive Writer(Buffer);
                                    uint8 Type = static_cast<uint8>(ENetMessage::DespawnEntity);
                                    Writer << Type;
                                    Net::WriteNetGuid(Writer, Guid);
                                    Net::AppendFramedMessage(DespawnBatch, Buffer.data(), static_cast<SIZE_T>(Buffer.size()));
                                }
                                State->Transport->Send(Event.Connection, DespawnBatch.data(), static_cast<SIZE_T>(DespawnBatch.size()), 0, ESendMode::Reliable);
                            }

                            LOG_DISPLAY("[Net][Server] Client {} ready; sent index tables", Event.Connection.Value);
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

                    // ScriptRpc is not implemented yet, and the id came off the wire, so it may be anything.
                    default:
                        break;
                    }
                });
                break;
            }
            }
        }

        // Roles fall out of net mode + ownership; recompute every tick (cheap, few networked entities).
        RefreshNetRoles(Registry, *State, bServer);

        // Smoothing back onto the transform is done by SNetMovementInterpSystem, not here.
        EnsureRepTransforms(Registry);

        if (bServer)
        {
            if (State->ConnectedClients > 0)
            {
                State->Stats.PropertyUpdatesSent = 0;
                State->Stats.bKeyframeThisTick   = false;

                // Periodic keyframe -> re-arm every client's baseline so a dropped delta self-heals.
                const SDefaultWorldSettings& WorldSettings = World->GetDefaultWorldSettings();
                const float KeyframeInterval = WorldSettings.TransformKeyframeInterval;
                if (KeyframeInterval > 0.0f)
                {
                    State->TimeSinceKeyframe += static_cast<float>(Context.GetDeltaTime());
                    if (State->TimeSinceKeyframe >= KeyframeInterval)
                    {
                        State->TimeSinceKeyframe       = 0.0f;
                        State->Stats.bKeyframeThisTick = true;
                        for (auto& KV : State->ClientViews) { KV.second.bForceBaseline = true; }
                    }
                }

                // Dynamic-entity lifetime, GUID assign and unregister on destroy, NOT spawn emission.
                MaintainDynamicLifetime(Registry, *State);

                // Broadcast INSIDE ServerReplicateRelevant, after the spawns mint and export their indices.
                TVector<uint8> ReliableBatch;
                ReplicateStableDespawns(Registry, *State, ReliableBatch);
                ReplicateDirtyProperties(Registry, *State, Context.GetTime(), ReliableBatch);
                if (State->bOwnershipDirty)
                {
                    BroadcastOwnership(Registry, ReliableBatch);
                    State->bOwnershipDirty = false;
                }

                // Also flushes the index exports and the reliable broadcast batch, in the correct order.
                ServerReplicateRelevant(Registry, *State, WorldSettings,
                    static_cast<float>(Context.GetDeltaTime()), static_cast<float>(Context.GetTime()), ReliableBatch);
            }
        }
        else
        {
            // Keep proxy physics from fighting replication (idempotent; acts once per body).
            ConfigureProxyPhysics(Registry, World);

            // One-shot, and the server re-emits spawns, ownership and poses on receipt.
            if (State->bClientConnected && !State->bClientReadySent)
            {
                State->bClientReadySent = true;
                TVector<uint8> Buffer;
                FNetArchive Writer(Buffer);
                uint8  Type  = static_cast<uint8>(ENetMessage::ClientReady);
                uint32 Proto = Net::GetProtocolHash();
                Writer << Type;
                Writer << Proto; // server kicks the connection on a protocol/build mismatch
                Net::SendFramed(*State->Transport, State->ServerConnection, Buffer.data(), static_cast<SIZE_T>(Buffer.size()), 0, ESendMode::Reliable);
            }

            // Client-authoritative movement, push the pose of entities we control up to the server.
            if (State->bClientConnected)
            {
                SendOwnedTransforms(Registry, *State, static_cast<float>(Context.GetDeltaTime()));
            }

            // Smoothing runs in SNetMovementInterpSystem at PostPhysics, every frame.
        }
    }
}
