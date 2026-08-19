#include "RuntimePCH.h"
#include "NetReplication.h"
#include "NetWorldState.h"
#include "ScriptRepState.h"
#include "Core/Profiler/Profile.h"
#include "Core/Serialization/NetArchive.h"
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Math/Hash/Hash.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "Assets/AssetRef.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "Components/NetworkComponent.h"
#include "Networking/INetworkTransport.h"
#include "Log/Log.h"
#include "entt/entt.hpp"

namespace Lumina::Net
{
    namespace
    {
        uint32 HashStructName(const FName& Name)
        {
            return Hash::GetHash32(Name.c_str());
        }

        CStruct* StructFromMeta(const entt::meta_type& Type)
        {
            entt::meta_any S = ECS::Utils::InvokeMetaFunc(Type, entt::hashed_string("static_struct"));
            return S ? S.cast<CStruct*>() : nullptr;
        }

        // Live pointer to Entity's component of the given meta type, or null. O(1): the component's storage
        // is keyed by its type-info hash (the inverse of entt::resolve(Set.info())), so resolve it directly
        // instead of scanning every storage.
        void* FindComponentPtr(entt::registry& Registry, entt::entity Entity, const entt::meta_type& Type)
        {
            if (auto* Set = Registry.storage(Type.info().hash()))
            {
                if (Set->contains(Entity))
                {
                    return Set->value(Entity);
                }
            }
            return nullptr;
        }

        struct FReplType
        {
            uint32          Hash;
            entt::meta_type Type;
            CStruct*        Struct;
        };

        // Every reflected component, sorted by name-hash so the array index is a stable cross-peer type id.
        // Same build => identical set + order on both peers, so we send a 1-byte varint index on the wire
        // instead of the 4-byte hash. ByIndex[i] is the type; HashToIndex maps a local component to its index.
        struct FReplTypeTable
        {
            TVector<FReplType>      ByIndex;
            THashMap<uint32, uint32> HashToIndex;
        };

        const FReplTypeTable& ReplTypes()
        {
            static const FReplTypeTable Table = []
            {
                FReplTypeTable T;
                for (auto&& [Id, Type] : entt::resolve())
                {
                    if (CStruct* St = StructFromMeta(Type))
                    {
                        T.ByIndex.push_back({ HashStructName(St->GetName()), Type, St });
                    }
                }
                Algo::Sort(T.ByIndex.begin(), T.ByIndex.end(),[](const FReplType& A, const FReplType& B)
                {
                    return A.Hash < B.Hash;
                });
                
                for (uint32 i = 0; i < static_cast<uint32>(T.ByIndex.size()); ++i)
                {
                    T.HashToIndex[T.ByIndex[i].Hash] = i;
                }
                return T;
            }();
            return Table;
        }

        // Client, apply a replicated attachment: reparent Child under the entity owning ParentGuid. No-op when
        // already correct; ParentGuid 0 detaches; an unspawned parent is recorded in PendingAttach and retried
        // when it spawns (DrainPendingAttach). Keep-local reparent -- the child's local transform is replicated.
        void ApplyReplicatedParent(entt::registry& Registry, entt::entity Child, uint32 ParentGuid)
        {
            FNetWorldState* State = Registry.ctx().find<FNetWorldState>();
            if (State == nullptr || !Registry.valid(Child))
            {
                return;
            }

            uint32 CurParentGuid = 0;
            if (const FRelationshipComponent* Rel = Registry.try_get<FRelationshipComponent>(Child); Rel && Rel->Parent != entt::null)
            {
                if (const SNetworkComponent* PNet = Registry.try_get<SNetworkComponent>(Rel->Parent))
                {
                    CurParentGuid = PNet->NetGUID.Value;
                }
            }

            const uint32 ChildKey = static_cast<uint32>(entt::to_integral(Child));
            if (CurParentGuid == ParentGuid)
            {
                State->PendingAttach.erase(ChildKey); // already attached as desired
                return;
            }

            if (ParentGuid == 0)
            {
                ECS::Utils::ReparentEntity(Registry, Child, entt::null, /*bPreserveWorld*/ false);
                State->PendingAttach.erase(ChildKey);
                return;
            }

            const entt::entity Parent = State->GuidTable.Find(FNetGUID{ ParentGuid });
            if (Parent != entt::null && Registry.valid(Parent))
            {
                ECS::Utils::ReparentEntity(Registry, Child, Parent, /*bPreserveWorld*/ false);
                State->PendingAttach.erase(ChildKey);
            }
            else
            {
                State->PendingAttach[ChildKey] = ParentGuid; // parent not spawned yet -- retry on its spawn
            }
        }
    }

    void DrainPendingAttach(entt::registry& Registry, FNetWorldState& State, uint32 NewGuid, entt::entity NewEntity)
    {
        for (auto It = State.PendingAttach.begin(); It != State.PendingAttach.end(); )
        {
            if (It->second == NewGuid)
            {
                const entt::entity Child = static_cast<entt::entity>(It->first);
                if (Registry.valid(Child))
                {
                    ECS::Utils::ReparentEntity(Registry, Child, NewEntity, /*bPreserveWorld*/ false);
                }
                It = State.PendingAttach.erase(It);
            }
            else
            {
                ++It;
            }
        }
    }

    bool ParentReplicates(entt::registry& Registry, entt::entity Parent)
    {
        if (Parent == entt::null)
        {
            return false;
        }
        const SNetworkComponent* PNet = Registry.try_get<SNetworkComponent>(Parent);
        return PNet != nullptr && PNet->bReplicates && PNet->bNetLoadOnClient && PNet->NetGUID.Value != 0;
    }

    uint32 GetProtocolHash()
    {
        // Bump on any hand-rolled wire-format change (message layout, codec) that reflection won't catch.
        constexpr uint32 NetProtocolVersion = 1;

        // FNV-1a over the version + the sorted replicated-component name-hashes. Same build => same table
        // order => same hash; a differing component set on either peer flips it.
        uint32 H = 2166136261u;
        auto Combine = [&H](uint32 V) { H ^= V; H *= 16777619u; };
        Combine(NetProtocolVersion);
        for (const FReplType& T : ReplTypes().ByIndex)
        {
            Combine(T.Hash);
        }
        return H;
    }

    void WriteNetGuid(FNetArchive& Ar, uint32 Guid)
    {
        const uint32 Encoded = (Guid >= NetGUID_DynamicStart)
            ? (((Guid - NetGUID_DynamicStart) << 1) | 1u)
            : (Guid << 1);
        WriteVarUInt(Ar, Encoded);
    }

    uint32 ReadNetGuid(FNetArchive& Ar)
    {
        const uint32 Encoded = ReadVarUInt(Ar);
        return (Encoded & 1u) ? (NetGUID_DynamicStart + (Encoded >> 1)) : (Encoded >> 1);
    }

    TVector<FComponentRepOut> CollectComponentFields(entt::registry& Registry, entt::entity Entity, FNetWorldState& State, bool bBaseline, FComponentRepState* DiffState)
    {
        LUMINA_PROFILE_SCOPE();
        TVector<FComponentRepOut> Out;

        // Hook-carrier: BindWriters sets the object/asset/name net-index hooks that NetSerializeReplicatedToBuffers
        // copies onto each per-field temp archive, so refs mint into State's outgoing maps (and queue exports)
        // exactly as the live write would. The scratch buffer itself is never written to.
        TVector<uint8> HookScratch;
        FNetArchive HookSrc(HookScratch);
        Net::BindWriters(HookSrc, State);

        const FReplTypeTable& Types = ReplTypes();
        for (auto [Id, Set] : Registry.storage())
        {
            if (!Set.contains(Entity))
            {
                continue;
            }
            entt::meta_type Type = entt::resolve(Set.info());
            if (!Type)
            {
                continue;
            }
            CStruct* St = StructFromMeta(Type);
            if (St == nullptr)
            {
                continue;
            }
            const auto It = Types.HashToIndex.find(HashStructName(St->GetName()));
            if (It == Types.HashToIndex.end())
            {
                continue; // not a known replicated type (shouldn't happen for a reflected component)
            }

            const uint32 WireIndex = It->second;
            void* Ptr = Set.value(Entity);

            // Current per-field bytes; compare to the last-sent baseline to build the changed-field mask.
            TVector<TVector<uint8>> Cur;
            St->NetSerializeReplicatedToBuffers(HookSrc, Ptr, Cur);
            const uint32 N = static_cast<uint32>(Cur.size());

            TVector<TVector<uint8>>* Base = nullptr;
            if (DiffState != nullptr)
            {
                Base = &DiffState->LastSent[WireIndex];
                if (static_cast<uint32>(Base->size()) != N)
                {
                    Base->assign(N, TVector<uint8>{}); // first sight / layout change -> all fields count as changed
                }
            }

            const uint32 MaskBytes = (N + 7) / 8;
            TVector<uint8> Mask(MaskBytes, 0);
            bool bAny = false;
            for (uint32 i = 0; i < N; ++i)
            {
                const bool bChanged = bBaseline || Base == nullptr || (*Base)[i] != Cur[i];
                if (bChanged)
                {
                    Mask[i >> 3] |= static_cast<uint8>(1u << (i & 7));
                    bAny = true;
                }
                if (Base != nullptr)
                {
                    (*Base)[i] = Cur[i];
                }
            }

            // On a delta, skip a component with no changed fields. On a baseline, keep it even when it has
            // no replicated fields so the client still emplaces the (possibly tag-only) component.
            if (!bBaseline && !bAny)
            {
                continue;
            }

            FComponentRepOut C;
            C.WireIndex = WireIndex;
            C.Block.insert(C.Block.end(), Mask.begin(), Mask.end());
            for (uint32 i = 0; i < N; ++i)
            {
                if (Mask[i >> 3] & (1u << (i & 7)))
                {
                    C.Block.insert(C.Block.end(), Cur[i].begin(), Cur[i].end());
                }
            }
            Out.push_back(std::move(C));
        }

        return Out;
    }

    void WriteEntityComponents(FNetArchive& Ar, entt::registry& Registry, entt::entity Entity, const TVector<FComponentRepOut>* Components)
    {
        LUMINA_PROFILE_SCOPE();
        // Component blocks precomputed by CollectComponentFields: [varint wire type id][changed-field bitmask
        // ++ changed fields]. Recipient-independent, so the same blocks serve every recipient.
        uint16 Count = Components ? static_cast<uint16>(Components->size()) : 0;
        Ar << Count;
        if (Components != nullptr)
        {
            for (const FComponentRepOut& C : *Components)
            {
                WriteVarUInt(Ar, C.WireIndex);
                if (!C.Block.empty())
                {
                    Ar.Serialize(const_cast<uint8*>(C.Block.data()), static_cast<int64>(C.Block.size()));
                }
            }
        }

        uint32 ParentGuid = 0;
        if (const FRelationshipComponent* Rel = Registry.try_get<FRelationshipComponent>(Entity);
            Rel && Rel->Parent != entt::null && ParentReplicates(Registry, Rel->Parent))
        {
            ParentGuid = Registry.get<SNetworkComponent>(Rel->Parent).NetGUID.Value;
        }
        WriteNetGuid(Ar, ParentGuid);
    }

    void ReadEntityComponents(FNetArchive& Ar, entt::registry& Registry, entt::entity Entity)
    {
        LUMINA_PROFILE_SCOPE();
        uint16 Count = 0;
        Ar << Count;

        // Components applied this message; patched after the loop to fire on_update<T> (the "OnRep" signal)
        // once the entity is fully updated, so a handler reacting to one component sees the others applied.
        TVector<entt::meta_type> Applied;

        const FReplTypeTable& Types = ReplTypes();
        for (uint16 i = 0; i < Count; ++i)
        {
            const uint32 Index = ReadVarUInt(Ar);
            if (Ar.HasError())
            {
                return;
            }

            if (Index >= static_cast<uint32>(Types.ByIndex.size()))
            {
                // Can't skip an unknown component without a size prefix. Same-build peers never hit this;
                // abort the entity if they do.
                LOG_WARN("[Net] Replication: component type index {} out of range -- aborting.", Index);
                Ar.SetHasError(true);
                return;
            }

            entt::meta_type Type = Types.ByIndex[Index].Type;
            CStruct* St = Types.ByIndex[Index].Struct;
            if (St == nullptr)
            {
                Ar.SetHasError(true);
                return;
            }

            // Changed-field bitmask: width is the struct's replicated-field count (same on both peers).
            const uint32 N = St->GetNetReplicatedPropertyCount();
            const uint32 MaskBytes = (N + 7) / 8;
            TVector<uint8> Mask(MaskBytes, 0);
            if (MaskBytes > 0)
            {
                Ar.Serialize(Mask.data(), static_cast<int64>(MaskBytes));
                if (Ar.HasError())
                {
                    return;
                }
            }

            // Apply only the changed replicated fields into the live component, preserving the rest. Emplace a
            // default first if absent.
            void* Ptr = FindComponentPtr(Registry, Entity, Type);
            if (Ptr == nullptr)
            {
                entt::meta_any Default{};
                ECS::Utils::InvokeMetaFunc(Type, entt::hashed_string("emplace"), entt::forward_as_meta(Registry), Entity, entt::forward_as_meta(Default));
                Ptr = FindComponentPtr(Registry, Entity, Type);
            }

            if (Ptr != nullptr)
            {
                St->NetReadReplicatedMasked(Ar, Ptr, Mask.data());
                Applied.push_back(Type);
            }
            else
            {
                Ar.SetHasError(true); // couldn't materialize, stream would desync
                return;
            }
        }

        // Fire on_update<T> for each applied component
        entt::meta_any Signal{};
        for (const entt::meta_type& Type : Applied)
        {
            if (!Registry.valid(Entity))
            {
                break; // a prior handler's script logic destroyed the entity
            }
            ECS::Utils::InvokeMetaFunc(Type, entt::hashed_string("patch"), entt::forward_as_meta(Registry), Entity, entt::forward_as_meta(Signal));
        }

        // Attachment link (mirrors WriteEntityComponents): always read to stay aligned, then resolve + reparent.
        const uint32 ParentGuid = ReadNetGuid(Ar);
        ApplyReplicatedParent(Registry, Entity, ParentGuid);
    }

    void AppendFramedMessage(TVector<uint8>& Batch, const uint8* Msg, SIZE_T MsgSize)
    {
        if (MsgSize == 0)
        {
            return;
        }
        if (MsgSize > MaxFramedMessageSize)
        {
            LOG_WARN("[Net] Message of {} bytes exceeds the 64K frame limit -- dropped.", (uint64)MsgSize);
            return;
        }
        const uint16 Len = static_cast<uint16>(MsgSize);
        Batch.push_back(static_cast<uint8>(Len & 0xFF));
        Batch.push_back(static_cast<uint8>((Len >> 8) & 0xFF));
        Batch.insert(Batch.end(), Msg, Msg + MsgSize);
    }

    void SendFramed(INetworkTransport& Transport, FConnectionHandle Connection, const uint8* Msg, SIZE_T MsgSize, uint8 Channel, ESendMode Mode)
    {
        TVector<uint8> Framed;
        AppendFramedMessage(Framed, Msg, MsgSize);
        Transport.Send(Connection, Framed.data(), static_cast<SIZE_T>(Framed.size()), Channel, Mode);
    }

    void BroadcastFramed(INetworkTransport& Transport, const uint8* Msg, SIZE_T MsgSize, uint8 Channel, ESendMode Mode)
    {
        TVector<uint8> Framed;
        AppendFramedMessage(Framed, Msg, MsgSize);
        Transport.Broadcast(Framed.data(), static_cast<SIZE_T>(Framed.size()), Channel, Mode);
    }

    void ForEachFramedMessage(const uint8* Data, SIZE_T Size, const TFunction<void(const uint8*, SIZE_T)>& Fn)
    {
        SIZE_T Offset = 0;
        while (Offset + 2 <= Size)
        {
            const uint16 Len = static_cast<uint16>(Data[Offset]) | (static_cast<uint16>(Data[Offset + 1]) << 8);
            Offset += 2;
            if (Offset + Len > Size)
            {
                break; // truncated / corrupt
            }
            Fn(Data + Offset, static_cast<SIZE_T>(Len));
            Offset += Len;
        }
    }

    namespace
    {
        // Get-or-assign a stable index for an object, queuing an export of its GUID the first time.
        uint32 NetObj_GetOrAssign(FNetObjectMap& Map, CObject* Obj)
        {
            if (Obj == nullptr) { return 0; }
            auto It = Map.ObjToIndex.find(Obj);
            if (It != Map.ObjToIndex.end()) { return It->second; }
            const uint32 Index = Map.NextIndex++;
            Map.ObjToIndex[Obj]    = Index;
            Map.IndexToGuid[Index] = Obj->GetGUID();
            Map.PendingExports.push_back(Index);
            return Index;
        }

        // Resolve an index to an object via the sender's exported GUID, loading on demand if not resident.
        // The result is cached (including null) so a missing asset is tried once, not per reference.
        CObject* NetObj_Resolve(FNetObjectMap& Map, uint32 Index)
        {
            if (Index == 0)
            {
                return nullptr;
            }
            auto Oit = Map.IndexToObject.find(Index);
            if (Oit != Map.IndexToObject.end())
            {
                return Oit->second;
            }
            
            auto Git = Map.IndexToGuid.find(Index);
            if (Git == Map.IndexToGuid.end())
            {
                return nullptr;
            }
            
            CObject* Obj = FindObject<CObject>(Git->second);
            if (Obj == nullptr)
            {
                Obj = StaticLoadObject(Git->second);
            }
            
            if (Obj == nullptr)
            {
                LOG_WARN("[Net] Replicated object index {} (GUID {}) not found and load failed -- marking failed (null).", Index, Git->second.ToString().c_str());
            }
            Map.IndexToObject[Index] = Obj; // cache (incl. null) so we don't reload on every reference
            return Obj;
        }

        // Get-or-assign a stable index for an asset ref, keyed by GUID (else Path) so it dedupes.
        uint32 NetAsset_GetOrAssign(FNetAssetMap& Map, const FAssetRef& Ref)
        {
            if (Ref.IsNull()) { return 0; }
            const FString& Key = !Ref.Guid.empty() ? Ref.Guid : Ref.Path;
            auto It = Map.KeyToIndex.find(Key);
            if (It != Map.KeyToIndex.end()) { return It->second; }
            const uint32 Index = Map.NextIndex++;
            Map.KeyToIndex[Key]   = Index;
            Map.IndexToRef[Index] = Ref;
            Map.PendingExports.push_back(Index);
            return Index;
        }

        FAssetRef NetAsset_Resolve(FNetAssetMap& Map, uint32 Index)
        {
            if (Index == 0) { return FAssetRef(); }
            auto It = Map.IndexToRef.find(Index);
            return It != Map.IndexToRef.end() ? It->second : FAssetRef();
        }

        // Get-or-assign a stable index for a name, keyed by the FName itself so it dedupes.
        uint32 NetName_GetOrAssign(FNetNameMap& Map, const FName& Name)
        {
            if (Name.IsNone()) { return 0; }
            auto It = Map.KeyToIndex.find(Name);
            if (It != Map.KeyToIndex.end()) { return It->second; }
            const uint32 Index = Map.NextIndex++;
            Map.KeyToIndex[Name]   = Index;
            Map.IndexToName[Index] = Name;
            Map.PendingExports.push_back(Index);
            return Index;
        }

        FName NetName_Resolve(FNetNameMap& Map, uint32 Index)
        {
            if (Index == 0) { return FName(); }
            auto It = Map.IndexToName.find(Index);
            return It != Map.IndexToName.end() ? It->second : FName();
        }
    }

    void BindWriters(FNetArchive& Ar, FNetWorldState& State)
    {
        Ar.ObjectToNetIndex   = [&State](CObject* O)         { return NetObj_GetOrAssign(State.OutObjects, O); };
        Ar.AssetRefToNetIndex = [&State](const FAssetRef& R) { return NetAsset_GetOrAssign(State.OutAssets, R); };
        Ar.NameToNetIndex     = [&State](const FName& N)     { return NetName_GetOrAssign(State.OutNames, N); };
    }

    void BindReaders(FNetArchive& Ar, FNetWorldState& State, uint32 SenderConn)
    {
        FNetObjectMap& InObj = State.InObjects[SenderConn]; // operator[] default-creates the per-connection entry
        FNetAssetMap&  InAst = State.InAssets[SenderConn];
        FNetNameMap&   InNme = State.InNames[SenderConn];

        Ar.NetIndexToObject   = [&InObj](uint32 I)
        {
            return NetObj_Resolve(InObj, I);
        };

        Ar.NetIndexToAssetRef = [&InAst](uint32 I, FAssetRef& Out)
        {
            Out = NetAsset_Resolve(InAst, I);
        };

        Ar.NetIndexToName     = [&InNme](uint32 I, FName& Out)
        {
            Out = NetName_Resolve(InNme, I);
        };
    }

    void BuildObjectExport(const FNetObjectMap& Map, const TVector<uint32>& Indices, TVector<uint8>& OutMsg)
    {
        FNetArchive Writer(OutMsg);
        uint8  Type  = static_cast<uint8>(ENetMessage::ObjectExport);
        uint16 Count = static_cast<uint16>(Indices.size());
        Writer << Type;
        Writer << Count;
        for (uint32 Index : Indices)
        {
            auto It = Map.IndexToGuid.find(Index);
            if (It == Map.IndexToGuid.end()) { continue; }
            FGuid Guid = It->second;
            Writer << Index;
            Writer << Guid;
        }
    }

    void ApplyObjectExport(FNetObjectMap& Map, const uint8* Data, SIZE_T Size)
    {
        FNetArchive Reader(Data, Size);
        uint8  Type  = 0;
        uint16 Count = 0;
        Reader << Type;
        Reader << Count;
        for (uint16 i = 0; i < Count; ++i)
        {
            uint32 Index = 0;
            FGuid  Guid;
            Reader << Index;
            Reader << Guid;
            if (Reader.HasError()) { break; }
            Map.IndexToGuid[Index] = Guid;
            Map.IndexToObject.erase(Index); // re-resolve lazily against the latest GUID
        }
    }

    void BuildAssetExport(const FNetAssetMap& Map, const TVector<uint32>& Indices, TVector<uint8>& OutMsg)
    {
        FNetArchive Writer(OutMsg);
        uint8  Type  = static_cast<uint8>(ENetMessage::AssetExport);
        uint16 Count = static_cast<uint16>(Indices.size());
        Writer << Type;
        Writer << Count;
        for (uint32 Index : Indices)
        {
            auto It = Map.IndexToRef.find(Index);
            if (It == Map.IndexToRef.end()) { continue; }
            FString Path = It->second.Path;
            FString Guid = It->second.Guid;
            Writer << Index;
            Writer << Path;
            Writer << Guid;
        }
    }

    void ApplyAssetExport(FNetAssetMap& Map, const uint8* Data, SIZE_T Size)
    {
        FNetArchive Reader(Data, Size);
        uint8  Type  = 0;
        uint16 Count = 0;
        Reader << Type;
        Reader << Count;
        for (uint16 i = 0; i < Count; ++i)
        {
            uint32  Index = 0;
            FString Path, Guid;
            Reader << Index;
            Reader << Path;
            Reader << Guid;
            if (Reader.HasError()) { break; }
            FAssetRef Ref;
            Ref.Path = Path;
            Ref.Guid = Guid;
            Map.IndexToRef[Index] = Ref;
        }
    }

    void BuildNameExport(const FNetNameMap& Map, const TVector<uint32>& Indices, TVector<uint8>& OutMsg)
    {
        FNetArchive Writer(OutMsg);
        uint8  Type  = static_cast<uint8>(ENetMessage::NameExport);
        uint16 Count = static_cast<uint16>(Indices.size());
        Writer << Type;
        Writer << Count;
        for (uint32 Index : Indices)
        {
            auto It = Map.IndexToName.find(Index);
            if (It == Map.IndexToName.end()) { continue; }
            FString Str = It->second.ToString();
            Writer << Index;
            Writer << Str;
        }
    }

    void ApplyNameExport(FNetNameMap& Map, const uint8* Data, SIZE_T Size)
    {
        FNetArchive Reader(Data, Size);
        uint8  Type  = 0;
        uint16 Count = 0;
        Reader << Type;
        Reader << Count;
        for (uint16 i = 0; i < Count; ++i)
        {
            uint32  Index = 0;
            FString Str;
            Reader << Index;
            Reader << Str;
            if (Reader.HasError()) { break; }
            Map.IndexToName[Index] = FName(Str); // interns the string in this peer's name table
        }
    }
}
