#include "RuntimePCH.h"
#include "NetReplication.h"
#include "World/ECS/Registry.h"
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
#include "World/Entity/Components/Component.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/RelationshipComponent.h"
#include "Components/NetworkComponent.h"
#include "Networking/INetworkTransport.h"
#include "Log/Log.h"

namespace Lumina::Net
{
    namespace
    {
        uint32 HashStructName(const FName& Name)
        {
            return Hash::GetHash32(Name.c_str());
        }

        // Constant time, since the storage is keyed by type id rather than scanned.
        void* FindComponentPtr(ECS::FRegistry& Registry, ECS::FEntity Entity, const CStruct* Type)
        {
            const FComponentOps* Ops = Type != nullptr ? Type->GetComponentOps() : nullptr;
            if (Ops == nullptr)
            {
                return nullptr;
            }
            if (auto* Set = Registry.FindStorage(static_cast<ECS::FComponentTypeID>(Ops->TypeId)))
            {
                if (Set->Contains(Entity))
                {
                    return Set->GetRaw(Entity);
                }
            }
            return nullptr;
        }

        struct FReplType
        {
            uint32   Hash;
            CStruct* Struct;
        };

        // The same build gives an identical order, so the wire carries a 1-byte index, not the hash.
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
                ForEachComponentStruct([&T](CStruct* St)
                {
                    T.ByIndex.push_back({ HashStructName(St->GetName()), St });
                });
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

        // A parent guid of 0 detaches, and an unspawned parent is retried when it spawns.
        void ApplyReplicatedParent(ECS::FRegistry& Registry, ECS::FEntity Child, uint32 ParentGuid)
        {
            FNetWorldState* State = Registry.Ctx().Find<FNetWorldState>();
            if (State == nullptr || !Registry.IsValid(Child))
            {
                return;
            }

            uint32 CurParentGuid = 0;
            if (const FRelationshipComponent* Rel = Registry.TryGet<FRelationshipComponent>(Child); Rel && Rel->Parent != ECS::NullEntity)
            {
                if (const SNetworkComponent* PNet = Registry.TryGet<SNetworkComponent>(Rel->Parent))
                {
                    CurParentGuid = PNet->NetGUID.Value;
                }
            }

            const uint32 ChildKey = static_cast<uint32>((Child).Value);
            if (CurParentGuid == ParentGuid)
            {
                State->PendingAttach.erase(ChildKey); // already attached as desired
                return;
            }

            if (ParentGuid == 0)
            {
                ECS::Utils::ReparentEntity(Registry, Child, ECS::NullEntity, /*bPreserveWorld*/ false);
                State->PendingAttach.erase(ChildKey);
                return;
            }

            const ECS::FEntity Parent = State->GuidTable.Find(FNetGUID{ ParentGuid });
            if (Parent != ECS::NullEntity && Registry.IsValid(Parent))
            {
                ECS::Utils::ReparentEntity(Registry, Child, Parent, /*bPreserveWorld*/ false);
                State->PendingAttach.erase(ChildKey);
            }
            else
            {
                State->PendingAttach[ChildKey] = ParentGuid; // the parent is not spawned yet, so retry on its spawn
            }
        }
    }

    void DrainPendingAttach(ECS::FRegistry& Registry, FNetWorldState& State, uint32 NewGuid, ECS::FEntity NewEntity)
    {
        for (auto It = State.PendingAttach.begin(); It != State.PendingAttach.end(); )
        {
            if (It->second == NewGuid)
            {
                const ECS::FEntity Child = static_cast<ECS::FEntity>(It->first);
                if (Registry.IsValid(Child))
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

    bool ParentReplicates(ECS::FRegistry& Registry, ECS::FEntity Parent)
    {
        if (Parent == ECS::NullEntity)
        {
            return false;
        }
        const SNetworkComponent* PNet = Registry.TryGet<SNetworkComponent>(Parent);
        return PNet != nullptr && PNet->bReplicates && PNet->bNetLoadOnClient && PNet->NetGUID.Value != 0;
    }

    uint32 GetProtocolHash()
    {
        // Bump on any hand-rolled wire-format change (message layout, codec) that reflection won't catch.
        constexpr uint32 NetProtocolVersion = 1;

        // The same build gives the same table order, so a differing component set flips the hash.
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

    void CollectComponentFieldsInto(ECS::FRegistry& Registry, ECS::FEntity Entity, FNetWorldState& State,
                                    bool bBaseline, FComponentRepState* DiffState, TVector<FComponentRepOut>& Out)
    {
        LUMINA_PROFILE_SCOPE();

        // Elements are reused so each block keeps its capacity instead of being freed and remade.
        SIZE_T Emitted = 0;

        // Parked per thread; every buffer below is rebuilt from scratch on each call.
        static thread_local TVector<uint8>  HookScratch;
        static thread_local TVector<uint8>  CurBytes;
        static thread_local TVector<uint32> CurOffsets;
        static thread_local TVector<uint8>  Mask;

        HookScratch.clear();

        // BindWriters sets the hooks so refs mint into the outgoing maps exactly as a live write would.
        FNetArchive HookSrc(HookScratch);
        Net::BindWriters(HookSrc, State);

        const FReplTypeTable& Types = ReplTypes();
        for (Lumina::ECS::FSparseSet* SetPtr : Registry.GetActiveStorages())
        {
            const Lumina::ECS::FComponentTypeID Id = SetPtr->GetTypeInfo().TypeID;
            Lumina::ECS::FSparseSet& Set = *SetPtr;
            if (!Set.Contains(Entity))
            {
                continue;
            }
            CStruct* St = FindComponentStructByTypeId(Id);
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
            void* Ptr = Set.GetRaw(Entity);

            // Current field bytes, flat; compare to the last-sent baseline to build the changed-field mask.
            St->NetSerializeReplicatedFlat(HookSrc, Ptr, CurBytes, CurOffsets);
            const uint32 N = CurOffsets.empty() ? 0u : (uint32)CurOffsets.size() - 1u;

            FRepFieldSnapshot* Base = nullptr;
            if (DiffState != nullptr)
            {
                Base = &DiffState->LastSent[WireIndex];
            }

            // A layout change makes the old snapshot meaningless, so every field counts as changed.
            const bool bBaseUsable = Base != nullptr && Base->Num() == N;

            const uint32 MaskBytes = (N + 7) / 8;
            Mask.clear();
            Mask.resize(MaskBytes, 0);

            bool bAny = false;
            for (uint32 i = 0; i < N; ++i)
            {
                const uint32 Size = CurOffsets[i + 1] - CurOffsets[i];

                const bool bChanged = bBaseline || !bBaseUsable
                    || Base->FieldSize(i) != Size
                    || Memory::Memcmp(Base->FieldData(i), CurBytes.data() + CurOffsets[i], Size) != 0;

                if (bChanged)
                {
                    Mask[i >> 3] |= static_cast<uint8>(1u << (i & 7));
                    bAny = true;
                }
            }

            if (Base != nullptr)
            {
                Base->Bytes.assign(CurBytes.begin(), CurBytes.end());
                Base->Offsets.assign(CurOffsets.begin(), CurOffsets.end());
            }

            // A baseline keeps a fieldless component so the client still emplaces the tag-only one.
            if (!bBaseline && !bAny)
            {
                continue;
            }

            FComponentRepOut& C = (Emitted < Out.size()) ? Out[Emitted] : Out.emplace_back();
            ++Emitted;

            C.WireIndex = WireIndex;
            C.Block.clear();
            C.Block.insert(C.Block.end(), Mask.begin(), Mask.end());
            for (uint32 i = 0; i < N; ++i)
            {
                if (Mask[i >> 3] & (1u << (i & 7)))
                {
                    const uint8* Field = CurBytes.data() + CurOffsets[i];
                    C.Block.insert(C.Block.end(), Field, Field + (CurOffsets[i + 1] - CurOffsets[i]));
                }
            }
        }

        Out.resize(Emitted);
    }

    TVector<FComponentRepOut> CollectComponentFields(ECS::FRegistry& Registry, ECS::FEntity Entity, FNetWorldState& State, bool bBaseline, FComponentRepState* DiffState)
    {
        TVector<FComponentRepOut> Out;
        CollectComponentFieldsInto(Registry, Entity, State, bBaseline, DiffState, Out);
        return Out;
    }

    void WriteEntityComponents(FNetArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity, const TVector<FComponentRepOut>* Components)
    {
        LUMINA_PROFILE_SCOPE();
        // Recipient-independent, so the same precomputed blocks serve every recipient.
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
        if (const FRelationshipComponent* Rel = Registry.TryGet<FRelationshipComponent>(Entity);
            Rel && Rel->Parent != ECS::NullEntity && ParentReplicates(Registry, Rel->Parent))
        {
            ParentGuid = Registry.Get<SNetworkComponent>(Rel->Parent).NetGUID.Value;
        }
        WriteNetGuid(Ar, ParentGuid);
    }

    void ReadEntityComponents(FNetArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        LUMINA_PROFILE_SCOPE();
        uint16 Count = 0;
        Ar << Count;

        // Patched after the loop so an OnRep handler sees every other component already applied.
        TVector<CStruct*> Applied;

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
                // An unknown component cannot be skipped without a size prefix, so abort the entity.
                LOG_WARN("[Net] Replication: component type index {} out of range -- aborting.", Index);
                Ar.SetHasError(true);
                return;
            }

            CStruct* St = Types.ByIndex[Index].Struct;
            if (St == nullptr)
            {
                Ar.SetHasError(true);
                return;
            }

            // The bitmask width is the struct's replicated-field count, the same on both peers.
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

            // Applies only the changed fields into the live component, emplacing a default first if absent.
            void* Ptr = FindComponentPtr(Registry, Entity, St);
            if (Ptr == nullptr)
            {
                if (const FComponentOps* Ops = St->GetComponentOps())
                {
                    Ops->EmplaceDefault(Registry, Entity);
                }
                Ptr = FindComponentPtr(Registry, Entity, St);
            }

            if (Ptr != nullptr)
            {
                St->NetReadReplicatedMasked(Ar, Ptr, Mask.data());
                Applied.push_back(St);
            }
            else
            {
                Ar.SetHasError(true); // couldn't materialize, stream would desync
                return;
            }
        }

        // Fire on_update<T> for each applied component
        for (CStruct* Type : Applied)
        {
            if (!Registry.IsValid(Entity))
            {
                break; // a prior handler's script logic destroyed the entity
            }
            if (const FComponentOps* Ops = Type->GetComponentOps())
            {
                Ops->Patch(Registry, Entity);
            }
        }

        // Always read to stay aligned, then resolved and reparented.
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
        static thread_local TVector<uint8> Framed;
        Framed.clear();
        AppendFramedMessage(Framed, Msg, MsgSize);
        Transport.Send(Connection, Framed.data(), static_cast<SIZE_T>(Framed.size()), Channel, Mode);
    }

    void BroadcastFramed(INetworkTransport& Transport, const uint8* Msg, SIZE_T MsgSize, uint8 Channel, ESendMode Mode)
    {
        static thread_local TVector<uint8> Framed;
        Framed.clear();
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

        // The result is cached including null, so a missing asset is tried once rather than per reference.
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
