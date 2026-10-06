#include "RuntimePCH.h"
#include "NetworkSystem.h"
#include "World/ECS/Registry.h"

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
#include "Scripting/DotNet/NetScriptBridge.h"
#include "Core/Object/ScriptClass.h"

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
        void AdoptStableNetworkEntities(ECS::FRegistry& Registry, FNetWorldState& State, bool bAuthority)
        {
            TVector<ECS::FEntity> Networked;
            for (ECS::FEntity Entity : Registry.View<SNetworkComponent>())
            {
                Networked.push_back(Entity);
            }

            Algo::Sort(Networked, [](ECS::FEntity A, ECS::FEntity B)
            {
                return (A).Value < (B).Value;
            });

            uint32 StableId = 1; // stable range is [1, NetGUID_DynamicStart)
            for (ECS::FEntity Entity : Networked)
            {
                SNetworkComponent& Net = Registry.Get<SNetworkComponent>(Entity);
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
        void RefreshNetRoles(ECS::FRegistry& Registry, const FNetWorldState& State, bool bServer)
        {
            LUMINA_PROFILE_SCOPE();
            for (ECS::FEntity Entity : Registry.View<SNetworkComponent>())
            {
                SNetworkComponent& Net = Registry.Get<SNetworkComponent>(Entity);
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

        struct FOwnerRecord
        {
            uint32 Guid  = 0;
            uint32 Owner = 0;
        };

        // One message per client, since each only learns owners of entities it holds plus the ones it owns.
        void AppendOwnership(const FNetWorldState& State, uint32 Connection, const TVector<FOwnerRecord>& Changes,
                             const TVector<FOwnerRecord>* JoinerTable, TVector<uint8>& Reliable)
        {
            static thread_local TVector<FOwnerRecord> Picked;
            Picked.clear();
            for (const FOwnerRecord& Record : Changes)
            {
                if (Record.Owner == Connection || State.HoldsEntity(Connection, Record.Guid))
                {
                    Picked.push_back(Record);
                }
            }
            if (JoinerTable != nullptr)
            {
                Picked.insert(Picked.end(), JoinerTable->begin(), JoinerTable->end());
            }

            for (size_t Begin = 0; Begin < Picked.size(); Begin += 0xFFFF)
            {
                const size_t End = Math::Min(Picked.size(), Begin + 0xFFFF);
                TVector<uint8> Buffer;
                FNetArchive Writer(Buffer);
                uint8  Type  = static_cast<uint8>(ENetMessage::OwnershipUpdate);
                uint16 Count = static_cast<uint16>(End - Begin);
                Writer << Type;
                Writer << Count;
                for (size_t Index = Begin; Index < End; ++Index)
                {
                    Net::WriteNetGuid(Writer, Picked[Index].Guid);
                    WriteVarUInt(Writer, Picked[Index].Owner);
                }
                Net::AppendFramedMessage(Reliable, Buffer.data(), static_cast<SIZE_T>(Buffer.size()));
            }
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
                const ECS::FEntity Entity = State.GuidTable.Find(FNetGUID{ Guid });
                if (Entity != ECS::NullEntity && Registry.IsValid(Entity))
                {
                    if (SNetworkComponent* Net = Registry.TryGet<SNetworkComponent>(Entity))
                    {
                        Net->OwningConnectionId = Owner;
                    }
                }
            }
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
            FComponentRepState& CompDiff = Registry.GetOrEmplace<FComponentRepState>(Entity);
            Net::CollectComponentFields(Registry, Entity, State, /*bBaseline*/true, &CompDiff);
            TVector<Net::FScriptRepOut> Scripts;
            Net::CollectScriptFieldsInto(Registry, Entity, State, /*bBaseline*/true, &CompDiff, Scripts);

            Net::WriteSpawnComponents(Writer, Registry, Entity);
            Net::WriteScriptManifest(Writer, Registry, Entity);
            Net::WriteScriptStates(Writer, Registry, Entity);

            NetQuantize::FQuantizedVector::FromVector(Pos).Write(Writer);
            NetQuantize::FQuantizedQuat::FromQuat(Rot).Write(Writer);
            NetQuantize::FQuantizedVector::FromVector(Scale, NetQuantize::ScaleQuantum).Write(Writer);
        }
        
        // Runs on the game thread with synchronous signals, so the GuidTable mutation is safe.
        void OnNetworkComponentDestroyed(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            FNetWorldState* State = Registry.Ctx().Find<FNetWorldState>();
            if (State == nullptr)
            {
                return;
            }
            CWorld** WorldPtr = Registry.Ctx().Find<CWorld*>(); // find, since ctx may be gone during teardown
            CWorld*  World    = WorldPtr ? *WorldPtr : nullptr;
            if (World == nullptr || !IsServerMode(World->GetNetMode()))
            {
                return;
            }
            const SNetworkComponent& Net = Registry.Get<SNetworkComponent>(Entity);
            if (Net.NetGUID.Value >= NetGUID_DynamicStart) // stable GUIDs handled by ReplicateStableDespawns
            {
                State->GuidTable.Unregister(Net.NetGUID);
            }
        }

        // Assigns NetGUIDs to newly-spawned dynamic entities, with destruction handled by the destroy hook.
        void MaintainDynamicLifetime(ECS::FRegistry& Registry, FNetWorldState& State)
        {
            LUMINA_PROFILE_SCOPE();
            for (auto [Entity, Net] : Registry.View<SNetworkComponent>().Each())
            {
                if (Net.bNetLoadOnClient && Net.NetGUID.Value == 0)
                {
                    Net.NetGUID = State.GuidTable.AllocateDynamic();
                    State.GuidTable.Register(Net.NetGUID, Entity);
                }
            }
        }
        
        void ReplicateStableDespawns(ECS::FRegistry& Registry, FNetWorldState& State, TVector<uint8>& Batch)
        {
            LUMINA_PROFILE_SCOPE();
            for (size_t i = 0; i < State.KnownStableGuids.size(); )
            {
                const uint32 Guid = State.KnownStableGuids[i];
                const ECS::FEntity Entity = State.GuidTable.Find(FNetGUID{ Guid });
                if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
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
        void ReplicateDirtyProperties(ECS::FRegistry& Registry, FNetWorldState& State, double NowTime)
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
            struct FDirtyEntry { ECS::FEntity Entity; double LastTime; uint32 Guid; };
            TVector<FDirtyEntry> Dirty;
            {
                auto View = Registry.View<SNetworkComponent, FNetDirty>();
                for (ECS::FEntity Entity : View)
                {
                    const SNetworkComponent& Net = View.Get<SNetworkComponent>(Entity);
                    if (!Net.bReplicates || Net.NetGUID.Value == 0)
                    {
                        Registry.Remove<FNetDirty>(Entity);
                        continue;
                    }
                    const FComponentRepState* Cs = Registry.TryGet<FComponentRepState>(Entity);
                    Dirty.push_back({ Entity, Cs ? Cs->LastReplicatedTime : 0.0, Net.NetGUID.Value });
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
                SNetworkComponent& Net = Registry.Get<SNetworkComponent>(Entity);

                // Diff the native replicated component fields once (recipient-independent); updates the snapshot.
                FComponentRepState& CompDiff = Registry.GetOrEmplace<FComponentRepState>(Entity);

                // Parked across entities so a tick's worth of updates reuses one set of buffers.
                static thread_local TVector<Net::FComponentRepOut> Comps;
                static thread_local TVector<Net::FScriptRepOut>    Scripts;
                static thread_local TVector<uint8>                 Buffer;
                Net::CollectComponentFieldsInto(Registry, Entity, State, /*bBaseline*/false, &CompDiff, Comps);
                const bool bHeldBack = Net::CollectScriptFieldsInto(Registry, Entity, State, /*bBaseline*/false, &CompDiff, Scripts);
                // Waiting on a rate-limited field with nothing else to say, so this tick sends nothing at all.
                if (bHeldBack && Comps.empty() && Scripts.empty())
                {
                    continue;
                }

                // Written once and copied only to clients that hold the entity, which happens after relevancy runs.
                Buffer.clear();
                FNetArchive Writer(Buffer);
                Net::BindWriters(Writer, State);
                uint8 Type = static_cast<uint8>(ENetMessage::PropertyUpdate);
                Writer << Type;
                Net::WriteNetGuid(Writer, Net.NetGUID.Value);
                Net::WriteEntityComponents(Writer, Registry, Entity, &Comps);
                Net::WriteEntityScripts(Writer, Scripts);
                const size_t Before = State.PendingPropertyBytes.size();
                Net::AppendFramedMessage(State.PendingPropertyBytes, Buffer.data(), static_cast<SIZE_T>(Buffer.size()));
                const uint32 Framed = static_cast<uint32>(State.PendingPropertyBytes.size() - Before);
                State.PendingProperties.push_back({ Net.NetGUID.Value, static_cast<uint32>(Before), Framed });
                BytesThisTick += static_cast<int32>(Framed);

                CompDiff.LastReplicatedTime = NowTime; // mark sent -> drives the oldest-first fairness above
                if (!bHeldBack)
                {
                    Registry.Remove<FNetDirty>(Entity); // sent this tick; deferred entities keep the flag
                }
                ++State.Stats.PropertyUpdatesSent;
            }

            LUMINA_PROFILE_VALUE("Net/DirtyEntities",  static_cast<int64>(Dirty.size()));
            LUMINA_PROFILE_VALUE("Net/DirtyPropBytes", static_cast<int64>(BytesThisTick));
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
            if (Reader.HasError() || State.GuidTable.Find(FNetGUID{ Guid }) != ECS::NullEntity)
            {
                return; // malformed, or we already have this entity
            }

            Net::BindReaders(Reader, State, SenderConn);
            const ECS::FEntity Entity = Registry.Create();
            Net::ReadSpawnComponents(Reader, Registry, Entity);
            const uint32 ScriptCount = Net::ReadScriptManifest(Reader, Registry, Entity);
            Net::ReadScriptStates(Reader, Registry, Entity, ScriptCount);

            // Spawn pose
            NetQuantize::FQuantizedVector QPos;   
            QPos.Read(Reader);
            NetQuantize::FQuantizedQuat   QRot;   
            QRot.Read(Reader);
            NetQuantize::FQuantizedVector QScale; 
            QScale.Read(Reader);
            
            if (!Reader.HasError())
            {
                if (STransformComponent* T = Registry.TryGet<STransformComponent>(Entity))
                {
                    T->SetRaw(QPos.ToVector(), QRot.ToQuat(), QScale.ToVector(NetQuantize::ScaleQuantum));
                }
            }

            // A spawned tag arrives as the component alone, and lookups read the per-tag storage it belongs in.
            if (const STagComponent* Tag = Registry.TryGet<STagComponent>(Entity))
            {
                const FName TagName = Tag->Tag;
                ECS::Utils::SetEntityTag(Registry, Entity, TagName);
            }

            SNetworkComponent& Net = Registry.GetOrEmplace<SNetworkComponent>(Entity);
            Net.NetGUID            = FNetGUID{ Guid };
            Net.OwningConnectionId = Owner;
            State.GuidTable.Register(Net.NetGUID, Entity);
            Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
            // Now that this entity's NetGUID is registered, reparent any children that were waiting on it.
            Net::DrainPendingAttach(Registry, State, Guid, Entity);
        }

        // Client, apply a reliable property delta to an existing entity.
        void ApplyPropertyUpdate(ECS::FRegistry& Registry, FNetWorldState& State, uint32 SenderConn, const uint8* Data, SIZE_T Size)
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
            const ECS::FEntity Entity = State.GuidTable.Find(FNetGUID{ Guid });
            if (Entity != ECS::NullEntity && Registry.IsValid(Entity))
            {
                Net::BindReaders(Reader, State, SenderConn);
                Net::ReadEntityComponents(Reader, Registry, Entity);
                if (!Reader.HasError() && Registry.IsValid(Entity))
                {
                    Net::ReadEntityScripts(Reader, Registry, Entity);
                }
                if (Registry.IsValid(Entity))
                {
                    Registry.EmplaceOrReplace<FNeedsTransformUpdate>(Entity);
                }
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
            ECS::FRegistry& Registry = ECS::GetWorldRegistry(*World);
            const ECS::FEntity Entity = State.GuidTable.Find(FNetGUID{ Guid });
            if (Entity != ECS::NullEntity && Registry.IsValid(Entity))
            {
                State.GuidTable.Unregister(FNetGUID{ Guid });
                ECS::Utils::DestroyEntity(Registry, Entity);
            }
        }
        
        void ConfigureProxyPhysics(ECS::FRegistry& Registry, CWorld* World)
        {
            Physics::IPhysicsScene* PhysScene = World->GetPhysicsScene();
            if (PhysScene == nullptr)
            {
                return;
            }

            for (ECS::FEntity Entity : Registry.View<SNetworkComponent>())
            {
                SNetworkComponent& Net = Registry.Get<SNetworkComponent>(Entity);
                const bool bIsProxy = Net.LocalRole == ENetRole::SimulatedProxy || Net.LocalRole == ENetRole::AutonomousProxy;
                if (Net.bProxyPhysicsConfigured || !bIsProxy)
                {
                    continue;
                }

                // A character has no solver body to freeze, since the network drives its capsule directly.
                if (Registry.HasAny<SCharacterPhysicsComponent>(Entity))
                {
                    Net.bProxyPhysicsConfigured = true;
                    continue;
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
            }
        }

        // Coarser the farther away, and both peers derive the quantum from the wire tier.
        // The anchor rides once per snapshot, so a centimeter keeps it cheap while records carry the fine detail.
        constexpr double SnapshotAnchorQuantum = 0.01;

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
            bool        bPitch = false;
            int8        Pitch  = 0;
        };

        constexpr uint32 YawBits = 12;
        constexpr float  TwoPi   = 6.2831853f;

        // An upright pose turns about Y alone, which covers characters and most props on the ground.
        bool IsYawOnly(const FQuat& Rot)
        {
            return Math::Abs(Rot.x) < 1.0e-3f && Math::Abs(Rot.z) < 1.0e-3f;
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
                // Yaw-only rotation in one byte, since far entities need no full orientation fidelity.
                const float Yaw = Math::EulerAngles(R.Rot).y; // radians, around up (Y)
                float Norm = Yaw / 6.2831853f + 0.5f;
                Norm = Norm < 0.0f ? 0.0f : (Norm > 1.0f ? 1.0f : Norm);
                uint8 YawByte = static_cast<uint8>(Norm * 255.0f + 0.5f);
                Writer << YawByte;
            }
            else
            {
                bool bYawOnly = IsYawOnly(R.Rot);
                Writer.SerializeBit(bYawOnly);
                if (bYawOnly)
                {
                    const float Yaw = 2.0f * std::atan2(R.Rot.y, R.Rot.w);
                    float Norm = Yaw / TwoPi + 0.5f;
                    Norm -= std::floor(Norm);
                    uint32 Packed = static_cast<uint32>(Norm * static_cast<float>(1u << YawBits) + 0.5f) & ((1u << YawBits) - 1u);
                    Writer.SerializeBits(&Packed, YawBits);
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
            for (ECS::FEntity Entity : Registry.View<SNetworkComponent>())
            {
                SNetworkComponent& Net = Registry.Get<SNetworkComponent>(Entity);
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
                    Registry.Get<FRepTransform>(Entity).NetGUID = Net.NetGUID.Value;
                }
            }
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

            State.AlwaysRelevantRecords.clear();
            for (uint32 Rec = 0; Rec < State.Extract.Num(); ++Rec)
            {
                if (State.Extract.Flags[Rec] & NETREC_AlwaysRelevant)
                {
                    State.AlwaysRelevantRecords.push_back(Rec);
                }
            }

            PhaseMark(ENetPhase::Extract);
            const FNetExtract& Ex   = State.Extract;
            const FNetGrid&    Grid = State.Grid;
            const float Grace      = Settings.RelevancyGraceSeconds;
            const float EnterR2    = Settings.AOIEnterRadius * Settings.AOIEnterRadius;
            const int32 CellRadius = static_cast<int32>(Settings.AOILeaveRadius / Grid.CellSize) + 1;

            constexpr SIZE_T HeaderBytes        = 7 + 30;
            constexpr SIZE_T MaxRecordBytes     = 5 + 30 + 6 + 1 + 30;
            constexpr SIZE_T MaxRecordsPerFrame = (Net::MaxFramedMessageSize - HeaderBytes) / MaxRecordBytes;
            static_assert(MaxRecordsPerFrame >= 1 && MaxRecordsPerFrame <= 0xFFFF, "frame chunk size invalid");

            const uint32 NumClients = static_cast<uint32>(State.ConnectedClientIds.size());
            TVector<TVector<uint8>>                ClientReliable; ClientReliable.resize(NumClients);
            TVector<TVector<FTransformSendRecord>> ClientRecords;  ClientRecords.resize(NumClients);

            uint32 RelevantSum = 0, RelevantMax = 0, SpawnsSum = 0, DespawnsSum = 0;

            // Copied from the grid's cell-ordered arrays so the diff reads hot fields contiguously.
            struct FGathered { uint32 Rec; uint32 Guid; uint8 Flags; ENetLODTier Tier; };

            // Message building mints shared export indices, so the parallel pass only records what each client is owed.
            enum class EClientOp : uint8 { Spawn, MapBaseline, Despawn };
            struct FClientOp { EClientOp Op; uint32 Guid; uint32 Rec; };
            TVector<TVector<FClientOp>> ClientOps;      ClientOps.resize(NumClients);
            TVector<uint32>             ClientRelevant; ClientRelevant.resize(NumClients);

            TVector<FNetClientView*> Views;
            Views.reserve(NumClients);
            for (uint32 ConnId : State.ConnectedClientIds)
            {
                Views.push_back(&State.ClientViews[ConnId]);
            }

            const CNetworkSettings* NetSettings = GetDefault<CNetworkSettings>();
            const float  SendRate     = NetSettings != nullptr ? NetSettings->ServerSendRate : 30.0f;
            static const TOptional<int> MaxRelevantArg = GCommandLine->GetInt("maxrelevant");
            static const TOptional<int> SnapshotBytesArg = GCommandLine->GetInt("snapshotbudget");
            const int32 SnapshotBytesPerSecond = SnapshotBytesArg.value_or(NetSettings != nullptr ? NetSettings->MaxSnapshotBytesPerClient : 0);

            // A record runs about ten bytes once anchored and yaw-packed, which turns a byte budget into a record count.
            constexpr float ApproxRecordBytes = 10.0f;
            constexpr float MinResidencySeconds = 2.0f;
            const size_t MaxRecordsPerSend = (SnapshotBytesPerSecond > 0 && SendRate > 0.0f)
                ? static_cast<size_t>(Math::Max(1.0f, SnapshotBytesPerSecond / SendRate / ApproxRecordBytes)) : 0;
            const size_t MaxRelevant  = static_cast<size_t>(Math::Max(MaxRelevantArg.value_or(NetSettings != nullptr ? NetSettings->MaxRelevantPerClient : 0), 0));
            const double SendInterval = SendRate > 0.0f ? 1.0 / SendRate : 0.0;
            TVector<uint8>    Serviced;     Serviced.resize(NumClients);
            TVector<FVector3> ClientAnchor; ClientAnchor.resize(NumClients, FVector3(0.0f));
            TVector<float> ClientDelta; ClientDelta.resize(NumClients);
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
            // Phase A, the per-client gather, relevancy diff and transform records, run across workers.
            Task::ParallelFor(NumClients, [&](uint32 ci)
            {
                const uint32 ConnId = State.ConnectedClientIds[ci];
                FNetClientView& CV  = *Views[ci];
                ClientRelevant[ci] = static_cast<uint32>(CV.Relevant.size());
                if (!Serviced[ci])
                {
                    return;
                }
                const float SinceServiced = ClientDelta[ci];
                TVector<FClientOp>& Ops = ClientOps[ci];
                static thread_local TVector<FGathered> Gathered;

                // A client without a pawn yet still receives everything that is relevant everywhere.
                const auto VpIt = State.OwnerToRecord.find(ConnId);
                const bool bHasViewpoint = VpIt != State.OwnerToRecord.end();
                const FVector3 VP = bHasViewpoint ? Ex.WorldPos[VpIt->second] : FVector3(0.0f);
                ClientAnchor[ci] = VP;

                Gathered.clear();
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
                                Gathered.push_back({ Grid.SortedRecords[s], Grid.SortedGuid[s], Flags, Tier });
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
                    Gathered.push_back({ Rec, Ex.Guid[Rec], Ex.Flags[Rec], Tier });
                }

                // A crowd keeps only the nearest entities, so a full town costs a client a bounded stream.
                if (MaxRelevant > 0 && Gathered.size() > MaxRelevant)
                {
                    const uint32 OwnRecord = bHasViewpoint ? VpIt->second : UINT32_MAX;
                    auto Score = [&](const FGathered& G)
                    {
                        if ((G.Flags & NETREC_AlwaysRelevant) || G.Rec == OwnRecord)
                        {
                            return -1.0f;
                        }
                        const FVector3& P = Ex.WorldPos[G.Rec];
                        const float DistanceSq = (P.x - VP.x) * (P.x - VP.x) + (P.z - VP.z) * (P.z - VP.z);
                        // A held entity leaves only once it is well past a newcomer, and never in its first seconds, so a crowd does not churn spawns.
                        const auto Held = CV.Relevant.find(G.Guid);
                        if (Held == CV.Relevant.end())
                        {
                            return DistanceSq;
                        }
                        return Held->second.TimeHeld < MinResidencySeconds ? 0.0f : DistanceSq * 0.36f;
                    };
                    std::nth_element(Gathered.begin(), Gathered.begin() + MaxRelevant, Gathered.end(),
                        [&](const FGathered& A, const FGathered& B) { return Score(A) < Score(B); });
                    Gathered.resize(MaxRelevant);
                }

                // An entry is "relevant this tick" iff its RelevantTick == State.RelevancyTick.
                TVector<FTransformSendRecord>& Records  = ClientRecords[ci];

                struct FDueRecord { FTransformSendRecord Record; float Priority; };
                static thread_local TVector<FDueRecord> Due;
                Due.clear();

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
                            if (State.GuidTable.Find(FNetGUID{ Guid }) != ECS::NullEntity)
                            {
                                Ops.push_back({ EClientOp::Spawn, Guid, Rec });
                                E.bBaselinePending = true; // spawn carried the pose; hold the transform one tick
                            }
                        }
                        else
                        {
                            E.bNeedsBaseline = true; // the client has it from the map, so send the current pose once
                            Ops.push_back({ EClientOp::MapBaseline, Guid, Rec });
                        }
                        EntryPtr = &CV.Relevant.emplace(Guid, E).first->second; // used immediately, before any rehash
                    }
                    else
                    {
                        It->second.RelevantTick = State.RelevancyTick;
                        It->second.Tier         = G.Tier;
                        It->second.TimeOutOfAOI = 0.0f;
                        It->second.TimeHeld    += SinceServiced;
                        EntryPtr = &It->second;
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
                            const bool bChanged  = (Flags & NETREC_Changed) != 0;
                            const bool bScaleChg = (Flags & NETREC_ScaleChanged) != 0;
                            const bool bBaseline = CV.bForceBaseline || E.bNeedsBaseline;

                            // A pose not sent for a whole keyframe interval goes again, which heals a lost final update.
                            const bool bRefresh = Settings.TransformKeyframeInterval > 0.0f && E.TimeSinceSent >= Settings.TransformKeyframeInterval;

                            // Near sends every changed tick, Mid and Far throttle to tier rate, and a baseline bypasses.
                            float Period = 0.0f;
                            if (E.Tier == ENetLODTier::Mid)      { Period = (Settings.TierMidRate > 0.0f) ? 1.0f / Settings.TierMidRate : 0.0f; }
                            else if (E.Tier == ENetLODTier::Far) { Period = (Settings.TierFarRate > 0.0f) ? 1.0f / Settings.TierFarRate : 0.0f; }
                            const bool bCadence = (Period <= 0.0f) || (E.TimeSinceSent >= Period);

                            if (bBaseline || bRefresh || (bChanged && bCadence))
                            {
                                FTransformSendRecord R;
                                R.Guid   = Guid;
                                R.Tier   = E.Tier;
                                R.Pos    = Ex.Pos[Rec];
                                R.Rot    = Ex.Rot[Rec].ToQuat();
                                R.bScale = bScaleChg || bBaseline || bRefresh;
                                R.Scale  = Ex.Scale[Rec].ToVector(NetQuantize::ScaleQuantum);
                                R.bPitch = (Ex.Flags[Rec] & NETREC_HasPitch) != 0;
                                R.Pitch  = Ex.Pitch[Rec];

                                // Staleness weighted by nearness, so a capped stream rotates through far entities instead of starving them.
                                const FVector3& P = Ex.WorldPos[Rec];
                                const float Distance = std::sqrt((P.x - VP.x) * (P.x - VP.x) + (P.z - VP.z) * (P.z - VP.z));
                                const float Priority = bBaseline ? 1.0e9f : E.TimeSinceSent / (1.0f + Distance / 15.0f);
                                Due.push_back({ R, Priority });
                            }
                        }
                    }
                }

                if (MaxRecordsPerSend > 0 && Due.size() > MaxRecordsPerSend)
                {
                    std::nth_element(Due.begin(), Due.begin() + MaxRecordsPerSend, Due.end(),
                        [](const FDueRecord& A, const FDueRecord& B) { return A.Priority > B.Priority; });
                    Due.resize(MaxRecordsPerSend);
                }
                for (const FDueRecord& D : Due)
                {
                    const auto It = CV.Relevant.find(D.Record.Guid);
                    if (It != CV.Relevant.end())
                    {
                        It->second.bNeedsBaseline = false;
                        It->second.TimeSinceSent  = 0.0f;
                    }
                    Records.push_back(D.Record);
                }

                // Expire entries not seen relevant this tick (left the AOI / destroyed) -> grace then despawn.
                for (auto It = CV.Relevant.begin(); It != CV.Relevant.end(); )
                {
                    FRelevantEntry& E = It->second;
                    if (E.RelevantTick != State.RelevancyTick)
                    {
                        const uint32 Guid = It->first;
                        const bool bDestroyed = (State.GuidTable.Find(FNetGUID{ Guid }) == ECS::NullEntity);
                        E.TimeOutOfAOI += SinceServiced;
                        if (bDestroyed || E.TimeOutOfAOI >= Grace)
                        {
                            if (E.bDynamic)
                            {
                                Ops.push_back({ EClientOp::Despawn, Guid, 0 });
                            }
                            It = CV.Relevant.erase(It);
                            continue;
                        }
                    }
                    ++It;
                }

                CV.bForceBaseline = false;
                ClientRelevant[ci] = static_cast<uint32>(CV.Relevant.size());
            });

            PhaseMark(ENetPhase::Relevancy);
            // Serial, since writing a message mints shared export indices. Each one is identical for every recipient.
            THashMap<uint32, TVector<uint8>> SpawnCache;
            THashMap<uint32, TVector<uint8>> BaselineCache;
            for (uint32 ci = 0; ci < NumClients; ++ci)
            {
                LUMINA_PROFILE_SECTION("Net/ClientOps");
                RelevantSum += ClientRelevant[ci];
                RelevantMax  = Math::Max(RelevantMax, ClientRelevant[ci]);
                TVector<uint8>& Reliable = ClientReliable[ci];
                for (const FClientOp& Op : ClientOps[ci])
                {
                    if (Op.Op == EClientOp::Despawn)
                    {
                        TVector<uint8> Buf;
                        FNetArchive W(Buf);
                        uint8 Type = static_cast<uint8>(ENetMessage::DespawnEntity);
                        W << Type;
                        Net::WriteNetGuid(W, Op.Guid);
                        Net::AppendFramedMessage(Reliable, Buf.data(), static_cast<SIZE_T>(Buf.size()));
                        ++DespawnsSum;
                        continue;
                    }

                    const ECS::FEntity Ent = State.GuidTable.Find(FNetGUID{ Op.Guid });
                    if (Ent == ECS::NullEntity || !Registry.IsValid(Ent))
                    {
                        continue;
                    }

                    if (Op.Op == EClientOp::Spawn)
                    {
                        auto [It, bNew] = SpawnCache.try_emplace(Op.Guid);
                        if (bNew)
                        {
                            WriteSpawnMessage(Registry, State, Ent, Op.Guid, Ex.OwnerConn[Op.Rec], Ex.Pos[Op.Rec], Ex.Rot[Op.Rec].ToQuat(),
                                Ex.Scale[Op.Rec].ToVector(NetQuantize::ScaleQuantum), It->second);
                        }
                        Net::AppendFramedMessage(Reliable, It->second.data(), static_cast<SIZE_T>(It->second.size()));
                        ++SpawnsSum;
                        continue;
                    }

                    // Refs minted here are flushed by the export step before this reliable batch.
                    if (!Registry.HasAny<FComponentRepState>(Ent))
                    {
                        continue;
                    }
                    auto [It, bNew] = BaselineCache.try_emplace(Op.Guid);
                    if (bNew)
                    {
                        FComponentRepState& CDiff = Registry.GetOrEmplace<FComponentRepState>(Ent);
                        TVector<Net::FComponentRepOut> Comps = Net::CollectComponentFields(Registry, Ent, State, /*bBaseline*/true, &CDiff);
                        TVector<Net::FScriptRepOut> Scripts;
                        Net::CollectScriptFieldsInto(Registry, Ent, State, /*bBaseline*/true, &CDiff, Scripts);
                        FNetArchive W(It->second);
                        Net::BindWriters(W, State);
                        uint8 PType = static_cast<uint8>(ENetMessage::PropertyUpdate);
                        W << PType;
                        Net::WriteNetGuid(W, Op.Guid);
                        Net::WriteEntityComponents(W, Registry, Ent, &Comps);
                        Net::WriteEntityScripts(W, Scripts);
                    }
                    Net::AppendFramedMessage(Reliable, It->second.data(), static_cast<SIZE_T>(It->second.size()));
                }
            }

            LUMINA_PROFILE_VALUE("Net/RelevantMax", static_cast<int64>(RelevantMax));
            LUMINA_PROFILE_VALUE("Net/Spawns",      static_cast<int64>(SpawnsSum));
            LUMINA_PROFILE_VALUE("Net/Despawns",    static_cast<int64>(DespawnsSum));

            PhaseMark(ENetPhase::Messages);
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
            PhaseMark(ENetPhase::Exports);
            if (!ReliableBroadcast.empty())
            {
                State.Stats.ReliableBatchBytes = static_cast<uint32>(ReliableBroadcast.size());
                State.Transport->Broadcast(ReliableBroadcast.data(), static_cast<SIZE_T>(ReliableBroadcast.size()), 0, ESendMode::Reliable);
            }

            PhaseMark(ENetPhase::Exports);
            // Ownership changes resolve to GUIDs here, after this tick's spawns have minted theirs.
            TVector<FOwnerRecord> OwnerChanges;
            for (ECS::FEntity Entity : State.OwnershipChanged)
            {
                const SNetworkComponent* Net = Registry.IsValid(Entity) ? Registry.TryGet<SNetworkComponent>(Entity) : nullptr;
                if (Net != nullptr && Net->bNetLoadOnClient && Net->NetGUID.Value != 0)
                {
                    OwnerChanges.push_back({ Net->NetGUID.Value, Net->OwningConnectionId });
                }
            }
            State.OwnershipChanged.clear();

            TVector<FOwnerRecord> JoinerTable;
            if (!State.OwnershipJoiners.empty())
            {
                for (auto [Entity, Net] : Registry.View<SNetworkComponent>().Each())
                {
                    if (Net.bNetLoadOnClient && Net.NetGUID.Value != 0 && Net.NetGUID.Value < NetGUID_DynamicStart && Net.OwningConnectionId != 0)
                    {
                        JoinerTable.push_back({ Net.NetGUID.Value, Net.OwningConnectionId });
                    }
                }
            }

            PhaseMark(ENetPhase::Exports);
            // Phase B builds every client's payload across workers, since only the transport calls must stay serial.
            TVector<TVector<uint8>> ClientUnreliable; ClientUnreliable.resize(NumClients);
            TVector<uint32>         ClientLargest;    ClientLargest.resize(NumClients);
            TVector<uint32>         ClientBytes;      ClientBytes.resize(NumClients);
            TVector<uint32>         ClientChunks;     ClientChunks.resize(NumClients);
            Task::ParallelFor(NumClients, [&](uint32 ci)
            {
                const uint32 ConnId = State.ConnectedClientIds[ci];
                TVector<uint8>& Pending = Views[ci]->PendingReliable;

                const bool bJoiner = Algo::Find(State.OwnershipJoiners, ConnId) != State.OwnershipJoiners.end();
                if (!OwnerChanges.empty() || bJoiner)
                {
                    AppendOwnership(State, ConnId, OwnerChanges, bJoiner ? &JoinerTable : nullptr, Pending);
                }
                for (const FNetWorldState::FPendingProperty& Property : State.PendingProperties)
                {
                    if (State.HoldsEntity(ConnId, Property.Guid))
                    {
                        Pending.insert(Pending.end(), State.PendingPropertyBytes.begin() + Property.Offset,
                            State.PendingPropertyBytes.begin() + Property.Offset + Property.Size);
                    }
                }
                if (!Serviced[ci])
                {
                    return;
                }

                // Spawns lead, so an update never reaches a client before the entity it changes.
                TVector<uint8>& Reliable = ClientReliable[ci];
                Reliable.insert(Reliable.end(), Pending.begin(), Pending.end());
                Pending.clear();

                const TVector<FTransformSendRecord>& Records = ClientRecords[ci];
                TVector<uint8>& Unreliable = ClientUnreliable[ci];
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
                    const NetQuantize::FQuantizedVector QAnchor = NetQuantize::FQuantizedVector::FromVector(ClientAnchor[ci], SnapshotAnchorQuantum);
                    QAnchor.Write(Writer);
                    const FVector3 Anchor = QAnchor.ToVector(SnapshotAnchorQuantum);
                    for (size_t i = Begin; i < End; ++i) { WriteTransformRecord(Writer, Records[i], Anchor); }
                    const uint32 FrameBytes = static_cast<uint32>(Buffer.size());
                    ClientLargest[ci] = Math::Max(ClientLargest[ci], FrameBytes);
                    ClientBytes[ci]  += FrameBytes;
                    ++ClientChunks[ci];
                    Net::AppendFramedMessage(Unreliable, Buffer.data(), static_cast<SIZE_T>(Buffer.size()));
                }
            });

            PhaseMark(ENetPhase::Payloads);
            uint32 LargestFrame = 0, TotalBytes = 0, Chunks = 0;
            {
                LUMINA_PROFILE_SECTION("Net/ClientSend");
                for (uint32 ci = 0; ci < NumClients; ++ci)
                {
                    const FConnectionHandle Conn{ State.ConnectedClientIds[ci] };
                    if (!ClientReliable[ci].empty())
                    {
                        State.Transport->Send(Conn, ClientReliable[ci].data(), static_cast<SIZE_T>(ClientReliable[ci].size()), 0, ESendMode::Reliable);
                    }
                    if (!ClientUnreliable[ci].empty())
                    {
                        State.Transport->Send(Conn, ClientUnreliable[ci].data(), static_cast<SIZE_T>(ClientUnreliable[ci].size()), 0, ESendMode::UnreliableSequenced);
                    }
                    LargestFrame = Math::Max(LargestFrame, ClientLargest[ci]);
                    TotalBytes  += ClientBytes[ci];
                    Chunks      += ClientChunks[ci];
                }
            }

            PhaseMark(ENetPhase::Send);
            State.OwnershipJoiners.clear();
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

        // Client -> server. Push the pose of entities this client owns (AutonomousProxy) upstream.
        void SendOwnedTransforms(ECS::FRegistry& Registry, FNetWorldState& State, float DeltaTime)
        {
            auto TransformStorage = Registry.GetStorage<STransformComponent>();
            auto View = Registry.View<SNetworkComponent, FRepTransform>();

            TVector<FTransformSendRecord> Records;
            for (ECS::FEntity Entity : View)
            {
                SNetworkComponent& Net = View.Get<SNetworkComponent>(Entity);
                FRepTransform&     Rep = View.Get<FRepTransform>(Entity);
                if (Net.LocalRole != ENetRole::AutonomousProxy || !Net.bReplicatesMovement || NetPrediction::IsPredictedCharacter(Registry, Entity))
                {
                    continue;
                }
                if (!TransformStorage.Contains(Entity))
                {
                    continue;
                }
                STransformComponent& T = TransformStorage.Get(Entity);

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
                WriteTransformRecord(Writer, R, FVector3(0.0f));
            }
            Net::SendFramed(*State.Transport, State.ServerConnection, Buffer.data(), static_cast<SIZE_T>(Buffer.size()), 0, ESendMode::UnreliableSequenced);
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
                uint8 YawByte = 0;
                Reader << YawByte;
                const float Yaw = (static_cast<float>(YawByte) / 255.0f - 0.5f) * 6.2831853f;
                Rot = FQuat(FVector3(0.0f, Yaw, 0.0f));
            }
            else
            {
                bool bYawOnly = false;
                Reader.SerializeBit(bYawOnly);
                if (bYawOnly)
                {
                    uint32 Packed = 0;
                    Reader.SerializeBits(&Packed, YawBits);
                    const float Yaw = (static_cast<float>(Packed) / static_cast<float>(1u << YawBits) - 0.5f) * TwoPi;
                    Rot = FQuat(FVector3(0.0f, Yaw, 0.0f));
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

            const ECS::FEntity Entity = State.GuidTable.Find(FNetGUID{ Guid });
            if (Entity == ECS::NullEntity || !Registry.IsValid(Entity) || !Registry.HasAll<STransformComponent>(Entity))
            {
                return true; // unknown/late entity; skip but keep parsing the batch
            }

            SNetworkComponent* Net = Registry.TryGet<SNetworkComponent>(Entity);
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
                if (SCharacterControllerComponent* Controller = Registry.TryGet<SCharacterControllerComponent>(Entity))
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
        void ApplyClientTransform(ECS::FRegistry& Registry, FNetWorldState& State, FConnectionHandle Sender, double ServerNow, const uint8* Data, SIZE_T Size)
        {
            FNetArchive Reader(Data, Size);
            uint8  Type  = 0;
            uint16 Count = 0;
            Reader << Type;
            Reader << Count;

            State.LatestServerTime = Math::Max(ServerNow, State.LatestServerTime);

            for (uint16 i = 0; i < Count; ++i)
            {
                if (!ReadTransformIntoRing(Registry, State, Reader, ServerNow, /*bSkipAutonomous*/ false, /*OwnerGate*/ Sender.Value, FVector3(0.0f)))
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
                Reader.Serialize(Payload.data(), static_cast<int64>(PayloadSize));
                if (Reader.HasError())
                {
                    return;
                }
            }

            const ECS::FEntity Entity = State.GuidTable.Find(FNetGUID{ Guid });
            if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
            {
                return;
            }
            CEntityScript* Script = Net::GetScriptAt(Registry, Entity, ScriptIndex);
            if (Script == nullptr)
            {
                return;
            }

            if (!bServer)
            {
                NetScripts::DispatchRpc(Script, RpcId, Caller, Payload.data(), PayloadSize);
                return;
            }

            const ERpcTarget Target = static_cast<ERpcTarget>(TargetByte);
            const bool bReliable = (Flags & static_cast<uint8>(ENetFlags::Unreliable)) == 0;
            auto Relay = [&](uint32 Connection)
            {
                TVector<uint8>& Batch = bReliable ? State.PendingRpcReliable[Connection] : State.PendingRpcUnreliable[Connection];
                Net::AppendScriptRpc(Batch, Guid, ScriptIndex, RpcId, Target, Flags, Sender, Payload.data(), PayloadSize);
            };

            switch (Target)
            {
            case ERpcTarget::Host:
                NetScripts::DispatchRpc(Script, RpcId, Sender, Payload.data(), PayloadSize);
                break;
            case ERpcTarget::Broadcast:
                for (uint32 Connection : State.ReadyClientIds)
                {
                    if (Connection != Sender && State.HoldsEntity(Connection, Guid))
                    {
                        Relay(Connection);
                    }
                }
                NetScripts::DispatchRpc(Script, RpcId, Sender, Payload.data(), PayloadSize);
                break;
            case ERpcTarget::Owner:
            {
                const SNetworkComponent* Net = Registry.TryGet<SNetworkComponent>(Entity);
                const uint32 Owner = Net != nullptr ? Net->OwningConnectionId : 0u;
                if (Owner == 0)
                {
                    NetScripts::DispatchRpc(Script, RpcId, Sender, Payload.data(), PayloadSize);
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
                case ENetMessage::PropertyUpdate:    return "property";
                case ENetMessage::MoveAck:           return "moveack";
                case ENetMessage::ObjectExport:      return "objects";
                case ENetMessage::AssetExport:       return "assets";
                case ENetMessage::NameExport:        return "names";
                default:                             return "other";
            }
        }

        void LogStats(FNetWorldState& State, float DeltaTime)
        {
            static const float Interval = (float)GCommandLine->GetInt("netstats").value_or(0);
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
                Peers.empty() ? 0u : RttSum / (uint32)Peers.size(), RttMax,
                State.Stats.RelevantAvg, State.Stats.RelevantMax,
                State.StatsFrames ? State.StatsFrameMsSum / State.StatsFrames : 0.0, State.StatsFrameMsMax,
                BacklogMax / 1024.0, LossMax * 100.0f, (uint64)Platform::GetProcessMemoryUsageMegaBytes(),
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
                        Breakdown += Format("  {} {:.1f}", MessageName(Type), (double)Bytes[Type] / 1024.0 / Seconds).c_str();
                    }
                }
                LOG_DISPLAY("[Net][Stats] out KB/s by type{}", Breakdown.c_str());
            }

            static const char* const PhaseNames[] = { "extract", "schedule", "relevancy", "messages", "exports", "payloads", "send", "flush" };
            FString Phases;
            for (uint32 Phase = 0; Phase < static_cast<uint32>(ENetPhase::Count); ++Phase)
            {
                Phases += Format("  {} {:.2f}/{:.1f}", PhaseNames[Phase], State.StatsFrames ? State.PhaseMsSum[Phase] / State.StatsFrames : 0.0, State.PhaseMsMax[Phase]).c_str();
                State.PhaseMsSum[Phase] = 0.0;
                State.PhaseMsMax[Phase] = 0.0;
            }
            LOG_DISPLAY("[Net][Stats] ms per tick avg/max{}", Phases.c_str());

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
            for (auto& [Connection, Batch] : State.PendingRpcReliable)
            {
                if (!Batch.empty())
                {
                    State.Transport->Send(FConnectionHandle{ Connection }, Batch.data(), static_cast<SIZE_T>(Batch.size()), 0, ESendMode::Reliable);
                    Batch.clear();
                }
            }
            for (auto& [Connection, Batch] : State.PendingRpcUnreliable)
            {
                if (!Batch.empty())
                {
                    State.Transport->Send(FConnectionHandle{ Connection }, Batch.data(), static_cast<SIZE_T>(Batch.size()), 1, ESendMode::UnreliableSequenced);
                    Batch.clear();
                }
            }
        }

        // With nobody connected the diff still runs, so the state a joiner is caught up to includes these changes.
        // Client, tells this world's scripts the session ended, once.
        void LeaveHost(CWorld* World, FNetWorldState& State, ENetLeaveReason Reason)
        {
            if (State.bLeaveDispatched)
            {
                return;
            }
            State.bLeaveDispatched = true;
            LOG_DISPLAY("[Net][Client] Left the host ({})", static_cast<uint32>(Reason));
            NetScripts::DispatchSession(World, ENetSessionEvent::LeftHost, 0, Reason);
        }

        void SeedDirtyBaselines(ECS::FRegistry& Registry, FNetWorldState& State)
        {
            TVector<ECS::FEntity> Dirty;
            for (ECS::FEntity Entity : Registry.View<FNetDirty>())
            {
                Dirty.push_back(Entity);
            }
            static thread_local TVector<Net::FComponentRepOut> Comps;
            static thread_local TVector<Net::FScriptRepOut>    Scripts;
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

        // Everything a frame produced goes out once scripts and physics have run, so it is never a frame stale.
        // A C++ script's Sync fields have no setter to catch a write, so each is compared once a frame before anything is sent.
        void PollNativeSyncFields(ECS::FRegistry& Registry)
        {
            LUMINA_PROFILE_SCOPE();
            static THashMap<const CClass*, bool> HasSyncFields;
            static TVector<FProperty*> Fields;
            for (ECS::FEntity Entity : Registry.View<SEntityScriptComponent, SNetworkComponent>())
            {
                for (const TStrongObjectPtr<CEntityScript>& Held : Registry.Get<SEntityScriptComponent>(Entity).Scripts)
                {
                    CEntityScript* Script = Held.Get();
                    if (Script == nullptr || !Script->IsReady() || ToScriptClass(Script->GetClass()) != nullptr)
                    {
                        continue;
                    }
                    auto Found = HasSyncFields.find(Script->GetClass());
                    if (Found == HasSyncFields.end())
                    {
                        Script->GetClass()->GetNetReplicatedProperties(Fields);
                        Found = HasSyncFields.emplace(Script->GetClass(), !Fields.empty()).first;
                    }
                    if (Found->second)
                    {
                        Script->PollSync();
                    }
                }
            }
        }

        void SendFrame(CWorld* World, ECS::FRegistry& Registry, FNetWorldState& State, const FSystemContext& Context, bool bServer)
        {
            LUMINA_PROFILE_SCOPE();
            if (bServer)
            {
                if (State.ConnectedClients > 0)
                {
                    State.Stats.PropertyUpdatesSent = 0;
                    State.Stats.bKeyframeThisTick   = false;

                    // A periodic keyframe re-arms every client's baseline so a dropped delta heals.
                    const SDefaultWorldSettings& WorldSettings = World->GetDefaultWorldSettings();
                    const float KeyframeInterval = WorldSettings.TransformKeyframeInterval;
                    if (KeyframeInterval > 0.0f)
                    {
                        State.TimeSinceKeyframe += static_cast<float>(Context.GetDeltaTime());
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
                    State.PendingProperties.clear();
                    State.PendingPropertyBytes.clear();
                    ReplicateDirtyProperties(Registry, State, Context.GetTime());

                    ServerReplicateRelevant(Registry, State, WorldSettings,
                        static_cast<float>(Context.GetDeltaTime()), static_cast<float>(Context.GetTime()), ReliableBatch);
                    LUMINA_PROFILE_SECTION("Net/Acks");
                    NetPrediction::SendAcks(Registry, State, static_cast<float>(Context.GetDeltaTime()));
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
                    NetScripts::DispatchSession(World, ENetSessionEvent::JoinedHost, 0, ENetLeaveReason::None);
                }

                if (State.bClientConnected && !State.bClientReadySent)
                {
                    State.bClientReadySent = true;
                    TVector<uint8> Buffer;
                    FNetArchive Writer(Buffer);
                    uint8  Type  = static_cast<uint8>(ENetMessage::ClientReady);
                    uint32 Proto = Net::GetProtocolHash();
                    Writer << Type;
                    Writer << Proto;
                    Net::SendFramed(*State.Transport, State.ServerConnection, Buffer.data(), static_cast<SIZE_T>(Buffer.size()), 0, ESendMode::Reliable);
                }

                if (State.bClientConnected)
                {
                    SendOwnedTransforms(Registry, State, static_cast<float>(Context.GetDeltaTime()));
                    NetPrediction::SendCommands(Registry, State);
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
                LogStats(State, static_cast<float>(Context.GetDeltaTime()));
            }
        }
    }

    void SNetworkSystem::Configure()
    {
        RequireUpdate(EUpdateStage::FrameStart, EUpdatePriority::Highest);
        RequireUpdate(EUpdateStage::FrameEnd, EUpdatePriority::Low);
    }

    void SNetworkSystem::OnUpdate()
    {
        const FSystemContext& Context = GetContext();

        LUMINA_PROFILE_SCOPE();
        
        ECS::FRegistry& Registry = Context.GetRegistry();

        CWorld* World = Registry.Ctx().Get<CWorld*>();
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

            if (Physics::IPhysicsScene* Scene = World->GetPhysicsScene())
            {
                Scene->SetCharacterStepHook([World](float FixedDt, bool bAfter)
                {
                    NetPrediction::OnCharacterStep(World, FixedDt, bAfter);
                });
            }

            // Data-driven from FWorldContext, defaulting to loopback on port 7777.
            FWorldContext* WorldCtx = GWorldManager ? GWorldManager->FindContext(World) : nullptr;
            const FString  Host = WorldCtx ? WorldCtx->NetHost : FString("127.0.0.1");
            const uint16   Port = WorldCtx ? WorldCtx->NetPort : NetDefaultPort;

            if (bServer)
            {
                // Unregister a destroyed entity's dynamic GUID the instant it dies (server only, connected once).
                Registry.GetSignals<SNetworkComponent>().OnDestroy.Connect<&OnNetworkComponentDestroyed>();

                State->Transport.reset(Network::CreateTransport());
                if (GCommandLine->GetInt("netstats").value_or(0) > 0 && State->Transport != nullptr)
                {
                    TUniquePtr<FNetCountingTransport> Counting = MakeUnique<FNetCountingTransport>(Move(State->Transport));
                    State->CountingTransport = Counting.get();
                    State->Transport = Move(Counting);
                }

                FListenParams Params;
                Params.Port           = Port;
                const CNetworkSettings* NetSettings = GetDefault<CNetworkSettings>();
                Params.MaxConnections = NetSettings ? (uint32)Math::Max(NetSettings->MaxClients, 1) : 64u;
                if (TOptional<int> MaxClients = GCommandLine->GetInt("maxclients"))
                {
                    Params.MaxConnections = (uint32)Math::Clamp(MaxClients.value(), 1, 4095);
                }
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
                State->bWelcomed        = true;
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
                State->ConnectStartTime = PlatformTime::Seconds();
                LOG_DISPLAY("[Net] Client connecting to {}:{}...", Host.c_str(), Port);
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
            PollNativeSyncFields(Registry);
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
            NetPrediction::TickBudgets(World, Registry, static_cast<float>(Context.GetDeltaTime()));
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
                    Ids.erase(Algo::Remove(Ids, Event.Connection.Value), Ids.end());
                    auto& Joiners = State->OwnershipJoiners;
                    Joiners.erase(Algo::Remove(Joiners, Event.Connection.Value), Joiners.end());
                    State->PendingRpcReliable.erase(Event.Connection.Value);
                    State->PendingRpcUnreliable.erase(Event.Connection.Value);

                    // Told before ownership is released, so gameplay can still find what the player had.
                    auto& Ready = State->ReadyClientIds;
                    if (Algo::Find(Ready, Event.Connection.Value) != Ready.end())
                    {
                        Ready.erase(Algo::Remove(Ready, Event.Connection.Value), Ready.end());
                        NetScripts::DispatchConnection(World, Event.Connection.Value, false);
                    }
                    State->ClientViews.erase(Event.Connection.Value); // drop its per-client relevancy state

                    // A player's own entities leave with them instead of lingering as host-owned leftovers.
                    TVector<ECS::FEntity> Leaving;
                    for (ECS::FEntity Entity : Registry.View<SNetworkComponent>())
                    {
                        const SNetworkComponent& Net = Registry.Get<SNetworkComponent>(Entity);
                        if (Net.OwningConnectionId == Event.Connection.Value && Net.bDestroyWithOwner)
                        {
                            Leaving.push_back(Entity);
                        }
                    }
                    for (ECS::FEntity Entity : Leaving)
                    {
                        if (Registry.IsValid(Entity))
                        {
                            World->DestroyEntity(Entity);
                        }
                    }

                    // Release anything this connection owned so it doesn't stay stuck as an orphan proxy.
                    for (ECS::FEntity Entity : Registry.View<SNetworkComponent>())
                    {
                        SNetworkComponent& Net = Registry.Get<SNetworkComponent>(Entity);
                        if (Net.OwningConnectionId == Event.Connection.Value)
                        {
                            Net.OwningConnectionId = 0;
                            State->OwnershipChanged.push_back(Entity);
                        }
                    }
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
                                State->PendingLeaveReason = ENetLeaveReason::ProtocolMismatch;
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
                                    GEngine->Travel(FStringView(MapPath.c_str(), MapPath.size()), World);
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
                            State->OwnershipJoiners.push_back(Event.Connection.Value);

                            // Every changed stable entity's state, built before the index tables so any reference it mints goes out with them.
                            TVector<uint8> StableBaselines;
                            for (ECS::FEntity Ent : Registry.View<SNetworkComponent, FComponentRepState>())
                            {
                                const SNetworkComponent& StableNet = Registry.Get<SNetworkComponent>(Ent);
                                if (!StableNet.bReplicates || StableNet.NetGUID.Value == 0 || StableNet.NetGUID.Value >= NetGUID_DynamicStart)
                                {
                                    continue;
                                }
                                TVector<Net::FComponentRepOut> Comps = Net::CollectComponentFields(Registry, Ent, *State, /*bBaseline*/true, nullptr);
                                TVector<Net::FScriptRepOut> Scripts;
                                Net::CollectScriptFieldsInto(Registry, Ent, *State, /*bBaseline*/true, nullptr, Scripts);
                                TVector<uint8> Message;
                                FNetArchive W(Message);
                                Net::BindWriters(W, *State);
                                uint8 PType = static_cast<uint8>(ENetMessage::PropertyUpdate);
                                W << PType;
                                Net::WriteNetGuid(W, StableNet.NetGUID.Value);
                                Net::WriteEntityComponents(W, Registry, Ent, &Comps);
                                Net::WriteEntityScripts(W, Scripts);
                                Net::AppendFramedMessage(StableBaselines, Message.data(), static_cast<SIZE_T>(Message.size()));
                            }

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

                            // A joiner far from a stable entity would otherwise only catch up once it comes into view.
                            if (!StableBaselines.empty())
                            {
                                State->Transport->Send(Event.Connection, StableBaselines.data(), static_cast<SIZE_T>(StableBaselines.size()), 0, ESendMode::Reliable);
                            }

                            LOG_DISPLAY("[Net][Server] Client {} ready; sent index tables", Event.Connection.Value);

                            auto& Ready = State->ReadyClientIds;
                            if (Algo::Find(Ready, Event.Connection.Value) == Ready.end())
                            {
                                Ready.push_back(Event.Connection.Value);
                                NetScripts::DispatchConnection(World, Event.Connection.Value, true);
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
                    case ENetMessage::MoveCommands:
                        if (bServer)
                        {
                            NetPrediction::ReceiveCommands(World, Registry, Event.Connection.Value, Msg, MsgSize);
                        }
                        break;
                    case ENetMessage::MoveAck:
                        if (!bServer)
                        {
                            NetPrediction::ReceiveAck(World, Registry, *State, Msg, MsgSize);
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

        State->StatsReceiveMsSum += (PlatformTime::Seconds() - State->StatsTickStart) * 1000.0;

        // Roles fall out of net mode + ownership; recompute every tick (cheap, few networked entities).
        RefreshNetRoles(Registry, *State, bServer);
        NetPrediction::RefreshDrives(World, Registry, bServer);

        // Smoothing back onto the transform is done by SNetMovementInterpSystem, not here.
        EnsureRepTransforms(Registry);

        if (!bServer)
        {
            // Keep proxy physics from fighting replication; each proxy is configured once.
            ConfigureProxyPhysics(Registry, World);
        }
    }
}
