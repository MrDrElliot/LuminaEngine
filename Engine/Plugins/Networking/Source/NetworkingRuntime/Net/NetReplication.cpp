#include "RuntimePCH.h"
#include "NetReplication.h"
#include "World/ECS/Registry.h"
#include "NetWorldState.h"
#include "ScriptRepState.h"
#include "Core/Profiler/Profile.h"
#include "Core/Serialization/NetArchive.h"
#include "Core/Object/Class.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Math/Hash/Hash.h"
#include "Containers/HashTable.h"
#include "Containers/Vector.h"
#include "UI/UIDocumentRef.h"
#include "World/Entity/Components/Component.h"
#include "World/Entity/EntityUtils.h"
#include "World/Entity/Components/TransformComponent.h"
#include "Components/NetworkComponent.h"
#include "Networking/INetworkTransport.h"
#include "Log/Log.h"
#include "Platform/Time/PlatformTime.h"
#include "Scripting/EntityScript.h"
#include "Scripting/ScriptableObject.h"
#include "Scripting/DotNet/NetScriptBridge.h"

namespace Lumina
{
    FReplTypeTable::FReplTypeTable()
    {
        ForEachComponentStruct([this](CStruct* St)
        {
            // Event types are bound to the type registry too, without component ops.
            if (St->GetComponentOps() != nullptr)
            {
                ByIndex.Emplace(Hash::GetHash32(*St->GetName()), St);
            }
        });
        Algo::Sort(ByIndex, [](const FEntry& A, const FEntry& B)
        {
            return A.Hash < B.Hash;
        });

        for (int32 i = 0; i < ByIndex.Num(); ++i)
        {
            const int32 TypeId = static_cast<int32>(ByIndex[i].Struct->GetComponentOps()->TypeId);
            if (TypeId >= TypeIdToIndex.Num())
            {
                TypeIdToIndex.SetNum(TypeId + 1, Constants::kIndexNone);
            }
            TypeIdToIndex[TypeId] = i;
        }
    }

    FReplTypeTable::~FReplTypeTable()
    {
        for (const FEntry& Entry : ByIndex)
        {
            if (Entry.Default != nullptr)
            {
                Entry.Struct->GetComponentOps()->Delete(Entry.Default);
            }
        }
    }

    void* FReplTypeTable::DefaultFor(uint32 WireIndex)
    {
        FEntry& Entry = ByIndex[WireIndex];
        if (Entry.Default == nullptr)
        {
            Entry.Default = Entry.Struct->GetComponentOps()->New();
        }
        return Entry.Default;
    }
}

namespace Lumina::Net
{
    namespace
    {
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

        template<typename TFunc>
        void ForEachReplicatedComponent(ECS::FRegistry& Registry, const FReplTypeTable& Types, ECS::FEntity Entity, TFunc&& Func)
        {
            for (ECS::FSparseSet* Set : Registry.GetActiveStorages())
            {
                const ECS::FComponentTypeID Id = Set->GetTypeInfo().TypeID;
                if (Id >= Types.TypeIdToIndex.Num() || Types.TypeIdToIndex[Id] == Constants::kIndexNone || !Set->Contains(Entity))
                {
                    continue;
                }
                const int32 WireIndex = Types.TypeIdToIndex[Id];
                Func(static_cast<uint32>(WireIndex), Types.ByIndex[WireIndex].Struct, Set->GetRaw(Entity));
            }
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
            if (const ECS::FEntity Parent = Registry.GetHierarchy().GetParent(Child); Parent != ECS::NullEntity)
            {
                if (const SNetworkComponent* PNet = Registry.TryGet<ECS::Read<SNetworkComponent>>(Parent))
                {
                    CurParentGuid = PNet->NetGUID.Value;
                }
            }

            const uint32 ChildKey = static_cast<uint32>((Child).Value);
            if (CurParentGuid == ParentGuid)
            {
                State->PendingAttach.Remove(ChildKey); // already attached as desired
                return;
            }

            if (ParentGuid == 0)
            {
                ECS::Utils::ReparentEntity(Registry, Child, ECS::NullEntity, /*bPreserveWorld*/ false);
                State->PendingAttach.Remove(ChildKey);
                return;
            }

            const ECS::FEntity Parent = State->GuidTable.Find(SNetGUID{ ParentGuid });
            if (Parent != ECS::NullEntity && Registry.IsValid(Parent))
            {
                ECS::Utils::ReparentEntity(Registry, Child, Parent, /*bPreserveWorld*/ false);
                State->PendingAttach.Remove(ChildKey);
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
            if (It->Value == NewGuid)
            {
                const ECS::FEntity Child = static_cast<ECS::FEntity>(It->Key);
                if (Registry.IsValid(Child))
                {
                    ECS::Utils::ReparentEntity(Registry, Child, NewEntity, /*bPreserveWorld*/ false);
                }
                It = State.PendingAttach.Remove(It);
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
        const SNetworkComponent* PNet = Registry.TryGet<ECS::Read<SNetworkComponent>>(Parent);
        return PNet != nullptr && PNet->bReplicates && PNet->bNetLoadOnClient && PNet->NetGUID.Value != 0;
    }

    uint32 GetProtocolHash(const FReplTypeTable& Types)
    {
        // Bump on any hand-rolled wire-format change (message layout, codec) that reflection won't catch.
        constexpr uint32 NetProtocolVersion = 4;

        // The same build gives the same table order, so a differing component set flips the hash.
        uint32 H = 2166136261u;
        auto Combine = [&H](uint32 V) { H ^= V; H *= 16777619u; };
        Combine(NetProtocolVersion);
        for (const FReplTypeTable::FEntry& T : Types.ByIndex)
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
        int32 Emitted = 0;

        // Parked per thread; every buffer below is rebuilt from scratch on each call.
        static thread_local TVector<uint8>  HookScratch;
        static thread_local TVector<uint8>  CurBytes;
        static thread_local TVector<uint32> CurOffsets;
        static thread_local TVector<uint8>  Mask;

        HookScratch.Reset();

        // BindWriters sets the hooks so refs mint into the outgoing maps exactly as a live write would.
        FNetArchive HookSrc(HookScratch);
        Net::BindWriters(HookSrc, State);

        ForEachReplicatedComponent(Registry, State.ReplTypes, Entity, [&](uint32 WireIndex, CStruct* St, void* Ptr)
        {
            // Current field bytes, flat; compare to the last-sent baseline to build the changed-field mask.
            St->NetSerializeReplicatedFlat(HookSrc, Ptr, CurBytes, CurOffsets);
            const uint32 N = CurOffsets.IsEmpty() ? 0u : (uint32)CurOffsets.Num() - 1u;

            FRepFieldSnapshot* Base = nullptr;
            if (DiffState != nullptr)
            {
                Base = &DiffState->LastSent[WireIndex];
            }

            // A layout change makes the old snapshot meaningless, so every field counts as changed.
            const bool bBaseUsable = Base != nullptr && Base->Num() == N;

            const uint32 MaskBytes = (N + 7) / 8;
            Mask.Reset();
            Mask.SetNum(MaskBytes, 0);

            bool bAny = false;
            for (uint32 i = 0; i < N; ++i)
            {
                const uint32 Size = CurOffsets[i + 1] - CurOffsets[i];

                const bool bChanged = bBaseline || !bBaseUsable
                    || Base->FieldSize(i) != Size
                    || Memory::Memcmp(Base->FieldData(i), CurBytes.GetData() + CurOffsets[i], Size) != 0;

                if (bChanged)
                {
                    Mask[i >> 3] |= static_cast<uint8>(1u << (i & 7));
                    bAny = true;
                }
            }

            if (Base != nullptr)
            {
                Base->Bytes.Reset();
                Base->Bytes.Append(CurBytes.begin(), CurBytes.end());
                Base->Offsets.Reset();
                Base->Offsets.Append(CurOffsets.begin(), CurOffsets.end());
            }

            // A baseline keeps a fieldless component so the client still emplaces the tag-only one.
            if (!bBaseline && !bAny)
            {
                return;
            }

            FComponentRepOut& C = (Emitted < Out.Num()) ? Out[Emitted] : Out.EmplaceGetRef();
            ++Emitted;

            C.WireIndex = WireIndex;
            C.Block.Reset();
            C.Block.Append(Mask);
            for (uint32 i = 0; i < N; ++i)
            {
                if (Mask[i >> 3] & (1u << (i & 7)))
                {
                    const uint8* Field = CurBytes.GetData() + CurOffsets[i];
                    C.Block.Append(Field, Field + (CurOffsets[i + 1] - CurOffsets[i]));
                }
            }
        });

        Out.SetNum(Emitted);
    }

    void WriteEntityComponents(FNetArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity, const TVector<FComponentRepOut>* Components)
    {
        LUMINA_PROFILE_SCOPE();
        // Recipient-independent, so the same precomputed blocks serve every recipient.
        uint16 Count = Components ? static_cast<uint16>(Components->Num()) : 0;
        Ar << Count;
        if (Components != nullptr)
        {
            for (const FComponentRepOut& C : *Components)
            {
                WriteVarUInt(Ar, C.WireIndex);
                if (!C.Block.IsEmpty())
                {
                    Ar.Serialize(const_cast<uint8*>(C.Block.GetData()), static_cast<int64>(C.Block.Num()));
                }
            }
        }

        uint32 ParentGuid = 0;
        if (const ECS::FEntity Parent = Registry.GetHierarchy().GetParent(Entity);
            Parent != ECS::NullEntity && ParentReplicates(Registry, Parent))
        {
            ParentGuid = Registry.Get<ECS::Read<SNetworkComponent>>(Parent).NetGUID.Value;
        }
        WriteNetGuid(Ar, ParentGuid);
    }

    void ReadEntityComponents(FNetArchive& Ar, ECS::FRegistry& Registry, const FNetWorldState& State, ECS::FEntity Entity)
    {
        LUMINA_PROFILE_SCOPE();
        uint16 Count = 0;
        Ar << Count;

        // Patched after the loop so an OnRep handler sees every other component already applied.
        TVector<CStruct*> Applied;

        const FReplTypeTable& Types = State.ReplTypes;
        for (uint16 i = 0; i < Count; ++i)
        {
            const uint32 Index = ReadVarUInt(Ar);
            if (Ar.HasError())
            {
                return;
            }

            if (Index >= static_cast<uint32>(Types.ByIndex.Num()))
            {
                // An unknown component cannot be skipped without a size prefix, so abort the entity.
                LOG_WARN("[Net] Replication: component type index {} out of range -- aborting.", Index);
                Ar.SetHasError(true);
                return;
            }

            CStruct* St = Types.ByIndex[Index].Struct;

            // The bitmask width is the struct's replicated-field count, the same on both peers.
            const uint32 N = St->GetNetReplicatedPropertyCount();
            const uint32 MaskBytes = (N + 7) / 8;
            TVector<uint8> Mask(MaskBytes, 0);
            if (MaskBytes > 0)
            {
                Ar.Serialize(Mask.GetData(), static_cast<int64>(MaskBytes));
                if (Ar.HasError())
                {
                    return;
                }
            }

            // Applies only the changed fields into the live component, emplacing a default first if absent.
            void* Ptr = FindComponentPtr(Registry, Entity, St);
            if (Ptr == nullptr)
            {
                St->GetComponentOps()->EmplaceDefault(Registry, Entity);
                Ptr = FindComponentPtr(Registry, Entity, St);
            }

            if (Ptr != nullptr)
            {
                St->NetReadReplicatedMasked(Ar, Ptr, Mask.GetData());
                Applied.Add(St);
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
            Type->GetComponentOps()->Patch(Registry, Entity);
        }

        // Always read to stay aligned, then resolved and reparented.
        const uint32 ParentGuid = ReadNetGuid(Ar);
        ApplyReplicatedParent(Registry, Entity, ParentGuid);
    }

    namespace
    {
        const SEntityScriptComponent* ScriptsOf(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            return Registry.TryGet<ECS::Read<SEntityScriptComponent>>(Entity);
        }

        void WriteClassName(FNetArchive& Ar, FName Name)
        {
            if (Ar.NameToNetIndex)
            {
                WriteVarUInt(Ar, Ar.NameToNetIndex(Name));
                return;
            }
            Ar << Name;
        }

        FName ReadClassName(FNetArchive& Ar)
        {
            FName Name;
            if (Ar.NetIndexToName)
            {
                Ar.NetIndexToName(ReadVarUInt(Ar), Name);
                return Name;
            }
            Ar << Name;
            return Name;
        }
    }

    // The Rate a Sync field declares, as the shortest gap between sends, or zero when it may go every tick.
    static double MinSendInterval(const FProperty* Property)
    {
        const double Hz = Property->GetMetadataNumber("Rate").Get(0.0);
        return Hz > 0.0 ? 1.0 / Hz : 0.0;
    }

    bool CollectScriptFieldsInto(ECS::FRegistry& Registry, ECS::FEntity Entity, FNetWorldState& State,
                                 bool bBaseline, FComponentRepState* DiffState, TVector<FScriptRepOut>& Out)
    {
        LUMINA_PROFILE_SCOPE();
        Out.Reset();

        const SEntityScriptComponent* Component = ScriptsOf(Registry, Entity);
        if (Component == nullptr)
        {
            return false;
        }

        static thread_local TVector<uint8>      HookScratch;
        static thread_local TVector<uint8>      CurBytes;
        static thread_local TVector<uint32>     CurOffsets;
        static thread_local TVector<FProperty*> Fields;
        static thread_local TVector<uint8>      Held;
        static thread_local TVector<uint8>      Merged;
        static thread_local TVector<uint32>     MergedOffsets;
        bool bAnyHeld = false;
        const double Now = PlatformTime::Seconds();

        HookScratch.Reset();
        FNetArchive HookSrc(HookScratch);
        Net::BindWriters(HookSrc, State);

        for (int32 Index = 0; Index < Component->Scripts.Num(); ++Index)
        {
            CEntityScript* Script = Component->Scripts[Index].Get();
            if (Script == nullptr)
            {
                continue;
            }
            CClass* Class = Script->GetClass();
            if (Class->GetNetReplicatedPropertyCount() == 0)
            {
                continue;
            }

            Class->NetSerializeReplicatedFlat(HookSrc, Script, CurBytes, CurOffsets);
            const uint32 N = CurOffsets.IsEmpty() ? 0u : (uint32)CurOffsets.Num() - 1u;

            FRepFieldSnapshot* Base = DiffState != nullptr ? &DiffState->ScriptLastSent[Index] : nullptr;
            const bool bBaseUsable = Base != nullptr && Base->Num() == N;

            FScriptRepOut Rep;
            Rep.ScriptIndex = Index;
            Rep.Block.SetNum((N + 7) / 8, 0);

            Held.Init(0, N);
            bool bFieldsResolved = false;

            bool bAny = false;
            bool bHeldHere = false;
            for (uint32 i = 0; i < N; ++i)
            {
                const uint32 Size = CurOffsets[i + 1] - CurOffsets[i];
                const bool bChanged = bBaseline || !bBaseUsable
                    || Base->FieldSize(i) != Size
                    || Memory::Memcmp(Base->FieldData(i), CurBytes.GetData() + CurOffsets[i], Size) != 0;
                if (!bChanged)
                {
                    continue;
                }

                // A rate-limited field that went out too recently waits, keeping its old baseline so it still reads as changed.
                if (!bBaseline && bBaseUsable && DiffState != nullptr)
                {
                    if (!bFieldsResolved)
                    {
                        Class->GetNetReplicatedProperties(Fields);
                        bFieldsResolved = true;
                    }
                    const double Interval = i < static_cast<uint32>(Fields.Num()) ? MinSendInterval(Fields[i]) : 0.0;
                    if (Interval > 0.0)
                    {
                        TVector<double>& SentTimes = DiffState->ScriptFieldSentTime[Index];
                        SentTimes.SetNum(N, -1.0e9);
                        if (Now - SentTimes[i] < Interval)
                        {
                            Held[i] = 1;
                            bHeldHere = true;
                            continue;
                        }
                        SentTimes[i] = Now;
                    }
                }

                Rep.Block[i >> 3] |= static_cast<uint8>(1u << (i & 7));
                bAny = true;
            }

            if (Base != nullptr && !bHeldHere)
            {
                Base->Bytes.Reset();
                Base->Bytes.Append(CurBytes.begin(), CurBytes.end());
                Base->Offsets.Reset();
                Base->Offsets.Append(CurOffsets.begin(), CurOffsets.end());
            }
            else if (Base != nullptr)
            {
                Merged.Reset();
                MergedOffsets.Init(0u, 1);
                for (uint32 i = 0; i < N; ++i)
                {
                    const uint8* Source = Held[i] ? Base->FieldData(i) : CurBytes.GetData() + CurOffsets[i];
                    const uint32 Size = Held[i] ? Base->FieldSize(i) : CurOffsets[i + 1] - CurOffsets[i];
                    Merged.Append(Source, Size);
                    MergedOffsets.Add(static_cast<uint32>(Merged.Num()));
                }
                Base->Bytes.Reset();
                Base->Bytes.Append(Merged.begin(), Merged.end());
                Base->Offsets.Reset();
                Base->Offsets.Append(MergedOffsets.begin(), MergedOffsets.end());
                bAnyHeld = true;
            }

            if (!bAny)
            {
                continue;
            }

            for (uint32 i = 0; i < N; ++i)
            {
                if (Rep.Block[i >> 3] & (1u << (i & 7)))
                {
                    const uint8* Field = CurBytes.GetData() + CurOffsets[i];
                    Rep.Block.Append(Field, Field + (CurOffsets[i + 1] - CurOffsets[i]));
                }
            }
            Out.Add(Move(Rep));
        }
        return bAnyHeld;
    }

    void WriteScriptManifest(FNetArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SEntityScriptComponent* Component = ScriptsOf(Registry, Entity);
        const uint32 Count = Component != nullptr ? (uint32)Component->Scripts.Num() : 0u;
        WriteVarUInt(Ar, Count);
        for (uint32 Index = 0; Index < Count; ++Index)
        {
            CEntityScript* Script = Component->Scripts[Index].Get();
            WriteClassName(Ar, Script != nullptr ? Script->GetClass()->GetName() : FName());
        }
    }

    uint32 ReadScriptManifest(FNetArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const uint32 Count = ReadVarUInt(Ar);
        for (uint32 Index = 0; Index < Count && !Ar.HasError(); ++Index)
        {
            const FName ClassName = ReadClassName(Ar);
            const SEntityScriptComponent* Component = ScriptsOf(Registry, Entity);
            const uint32 Present = Component != nullptr ? (uint32)Component->Scripts.Num() : 0u;
            if (Index < Present || ClassName.IsNone())
            {
                continue;
            }

            CClass* ScriptClass = FScriptableRegistry::ResolveClass(ClassName);
            if (ScriptClass == nullptr || EntityScripts::Attach(Registry, Entity, ScriptClass) == nullptr)
            {
                LOG_WARN("[Net] Spawned entity names script class '{}', which this peer cannot load; its later scripts shift index.", *ClassName);
            }
        }
        return Count;
    }

    void WriteEntityScripts(FNetArchive& Ar, const TVector<FScriptRepOut>& Scripts)
    {
        WriteVarUInt(Ar, (uint32)Scripts.Num());
        for (const FScriptRepOut& Rep : Scripts)
        {
            WriteVarUInt(Ar, Rep.ScriptIndex);
            if (!Rep.Block.IsEmpty())
            {
                Ar.Serialize(const_cast<uint8*>(Rep.Block.GetData()), static_cast<int64>(Rep.Block.Num()));
            }
        }
    }

    void ReadEntityScripts(FNetArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        LUMINA_PROFILE_SCOPE();
        const uint32 Count = ReadVarUInt(Ar);

        static thread_local TVector<FProperty*> Props;
        static thread_local TVector<uint8>      Mask;
        static thread_local TVector<char>       Names;

        for (uint32 i = 0; i < Count; ++i)
        {
            const uint32 Index = ReadVarUInt(Ar);
            if (Ar.HasError())
            {
                return;
            }

            const SEntityScriptComponent* Component = ScriptsOf(Registry, Entity);
            CEntityScript* Script = (Component != nullptr && Index < (uint32)Component->Scripts.Num())
                ? Component->Scripts[Index].Get() : nullptr;
            if (Script == nullptr)
            {
                // Without the class there is no way to know how many bytes to skip.
                LOG_WARN("[Net] Replicated fields for missing script {} on entity {}; dropping the rest of the update.", Index, Entity.Value);
                Ar.SetHasError(true);
                return;
            }

            CClass* Class = Script->GetClass();
            Class->GetNetReplicatedProperties(Props);
            const uint32 N = (uint32)Props.Num();
            Mask.Init(0, (N + 7) / 8);
            if (!Mask.IsEmpty())
            {
                Ar.Serialize(Mask.GetData(), static_cast<int64>(Mask.Num()));
                if (Ar.HasError())
                {
                    return;
                }
            }

            Names.Reset();
            for (uint32 Field = 0; Field < N; ++Field)
            {
                if (Mask[Field >> 3] & (1u << (Field & 7)))
                {
                    const char* FieldName = *Props[Field]->GetPropertyName();
                    Names.Append(FieldName, FieldName + strlen(FieldName));
                    Names.Add('\0');
                }
            }

            NetScripts::DispatchSyncChanging(Script, Names.GetData(), (uint32)Names.Num());
            Class->NetReadReplicatedMasked(Ar, Script, Mask.GetData());
            if (Ar.HasError())
            {
                return;
            }
            NetScripts::DispatchSyncChanged(Script, Names.GetData(), (uint32)Names.Num());

            if (!Registry.IsValid(Entity))
            {
                return;
            }
        }
    }

    int32 FindScriptIndex(ECS::FRegistry& Registry, ECS::FEntity Entity, const CEntityScript* Script)
    {
        if (const SEntityScriptComponent* Component = Registry.TryGet<ECS::Read<SEntityScriptComponent>>(Entity))
        {
            for (int32 Index = 0; Index < Component->Scripts.Num(); ++Index)
            {
                if (Component->Scripts[Index].Get() == Script)
                {
                    return Index;
                }
            }
        }
        return Constants::kIndexNone;
    }

    CEntityScript* GetScriptAt(ECS::FRegistry& Registry, ECS::FEntity Entity, uint32 Index)
    {
        const SEntityScriptComponent* Component = Registry.TryGet<ECS::Read<SEntityScriptComponent>>(Entity);
        return (Component != nullptr && Index < (uint32)Component->Scripts.Num()) ? Component->Scripts[Index].Get() : nullptr;
    }

    void AppendScriptRpc(TVector<uint8>& Batch, uint32 Guid, uint32 ScriptIndex, uint32 RpcId, ERpcTarget Target,
                         uint8 Flags, uint32 Caller, const uint8* Payload, uint32 PayloadSize)
    {
        static thread_local TVector<uint8> Message;
        Message.Reset();
        FNetArchive Writer(Message);
        uint8 Type = static_cast<uint8>(ENetMessage::ScriptRpc);
        uint8 TargetByte = static_cast<uint8>(Target);
        Writer << Type;
        WriteNetGuid(Writer, Guid);
        WriteVarUInt(Writer, ScriptIndex);
        WriteVarUInt(Writer, RpcId);
        Writer << TargetByte;
        Writer << Flags;
        WriteVarUInt(Writer, Caller);
        WriteVarUInt(Writer, PayloadSize);
        if (PayloadSize > 0)
        {
            Writer.Serialize(const_cast<uint8*>(Payload), static_cast<int64>(PayloadSize));
        }
        AppendFramedMessage(Batch, Message.GetData(), static_cast<SIZE_T>(Message.Num()));
    }

    void WriteSpawnComponents(FNetArchive& Ar, ECS::FRegistry& Registry, FNetWorldState& State, ECS::FEntity Entity)
    {
        LUMINA_PROFILE_SCOPE();
        struct FSpawnComponent { uint32 WireIndex; CStruct* Struct; void* Data; };
        TVector<FSpawnComponent> Components;

        const CStruct* TransformStruct = STransformComponent::StaticStruct();
        ForEachReplicatedComponent(Registry, State.ReplTypes, Entity, [&](uint32 WireIndex, CStruct* St, void* Data)
        {
            const FSpawnComponent Entry{ WireIndex, St, Data };
            if (St == TransformStruct)
            {
                Components.Insert(Entry, 0);
            }
            else
            {
                Components.Add(Entry);
            }
        });

        uint16 Count = static_cast<uint16>(Components.Num());
        Ar << Count;
        for (const FSpawnComponent& Component : Components)
        {
            WriteVarUInt(Ar, Component.WireIndex);
            if (Component.Data != nullptr)
            {
                if (void* Defaults = State.ReplTypes.DefaultFor(Component.WireIndex))
                {
                    Component.Struct->NetWriteAllDelta(Ar, Component.Data, Defaults);
                }
                else
                {
                    Component.Struct->NetSerializeAll(Ar, Component.Data);
                }
            }
        }

        uint32 ParentGuid = 0;
        if (const ECS::FEntity Parent = Registry.GetHierarchy().GetParent(Entity);
            Parent != ECS::NullEntity && ParentReplicates(Registry, Parent))
        {
            ParentGuid = Registry.Get<ECS::Read<SNetworkComponent>>(Parent).NetGUID.Value;
        }
        WriteNetGuid(Ar, ParentGuid);
    }

    void ReadSpawnComponents(FNetArchive& Ar, ECS::FRegistry& Registry, const FNetWorldState& State, ECS::FEntity Entity)
    {
        LUMINA_PROFILE_SCOPE();
        uint16 Count = 0;
        Ar << Count;

        TVector<CStruct*> Applied;
        const FReplTypeTable& Types = State.ReplTypes;
        for (uint16 i = 0; i < Count; ++i)
        {
            const uint32 Index = ReadVarUInt(Ar);
            if (Ar.HasError() || Index >= static_cast<uint32>(Types.ByIndex.Num()))
            {
                Ar.SetHasError(true);
                return;
            }
            CStruct* St = Types.ByIndex[Index].Struct;
            const FComponentOps* Ops = St->GetComponentOps();

            // Read into a detached copy first, so the component's construct hooks see its real values.
            void* Staged = Ops->New();
            if (Staged == nullptr)
            {
                Ops->EmplaceDefault(Registry, Entity);
                continue;
            }
            St->NetReadAllDelta(Ar, Staged);
            if (Ar.HasError())
            {
                Ops->Delete(Staged);
                return;
            }
            Ops->EmplaceCopy(Registry, Entity, Staged);
            Ops->Delete(Staged);
            Applied.Add(St);
        }

        for (CStruct* Type : Applied)
        {
            if (!Registry.IsValid(Entity))
            {
                break;
            }
            Type->GetComponentOps()->Patch(Registry, Entity);
        }

        const uint32 ParentGuid = ReadNetGuid(Ar);
        ApplyReplicatedParent(Registry, Entity, ParentGuid);
    }

    void WriteScriptStates(FNetArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity)
    {
        const SEntityScriptComponent* Component = Registry.TryGet<ECS::Read<SEntityScriptComponent>>(Entity);
        const uint32 Count = Component != nullptr ? (uint32)Component->Scripts.Num() : 0u;
        for (uint32 Index = 0; Index < Count; ++Index)
        {
            if (CEntityScript* Script = Component->Scripts[Index].Get())
            {
                Script->GetClass()->NetSerializeAll(Ar, Script);
            }
        }
    }

    void ReadScriptStates(FNetArchive& Ar, ECS::FRegistry& Registry, ECS::FEntity Entity, uint32 Count)
    {
        for (uint32 Index = 0; Index < Count && !Ar.HasError(); ++Index)
        {
            CEntityScript* Script = GetScriptAt(Registry, Entity, Index);
            if (Script == nullptr)
            {
                // Without the class the bytes cannot be skipped, so the rest of the message is unreadable.
                Ar.SetHasError(true);
                return;
            }
            Script->GetClass()->NetSerializeAll(Ar, Script);
        }
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
        Batch.Add(static_cast<uint8>(Len & 0xFF));
        Batch.Add(static_cast<uint8>((Len >> 8) & 0xFF));
        Batch.Append(Msg, IntCastChecked<int32>(MsgSize));
    }

    void AppendCountedMessages(TVector<uint8>& Batch, ENetMessage Type, int32 NumEntries, TFunctionRef<bool(FNetArchive&, int32)> WriteEntry)
    {
        constexpr SIZE_T HeaderBytes = sizeof(uint8) + sizeof(uint16);
        TVector<uint8> Entries;
        TVector<uint8> Entry;
        TVector<uint8> Message;
        uint16 Count = 0;
        auto Flush = [&]()
        {
            if (Count == 0)
            {
                return;
            }
            Message.Reset();
            FNetArchive Writer(Message);
            uint8 TypeByte = static_cast<uint8>(Type);
            Writer << TypeByte;
            Writer << Count;
            Message.Append(Entries.GetData(), Entries.Num());
            AppendFramedMessage(Batch, Message.GetData(), static_cast<SIZE_T>(Message.Num()));
            Entries.Reset();
            Count = 0;
        };
        for (int32 Index = 0; Index < NumEntries; ++Index)
        {
            Entry.Reset();
            FNetArchive Writer(Entry);
            if (!WriteEntry(Writer, Index))
            {
                continue;
            }
            if (HeaderBytes + Entries.Num() + Entry.Num() > MaxFramedMessageSize || Count == UINT16_MAX)
            {
                Flush();
            }
            Entries.Append(Entry.GetData(), Entry.Num());
            ++Count;
        }
        Flush();
    }

    void SendFramed(INetworkTransport& Transport, FConnectionHandle Connection, const uint8* Msg, SIZE_T MsgSize, uint8 Channel, ESendMode Mode)
    {
        static thread_local TVector<uint8> Framed;
        Framed.Reset();
        AppendFramedMessage(Framed, Msg, MsgSize);
        Transport.Send(Connection, Framed.GetData(), static_cast<SIZE_T>(Framed.Num()), Channel, Mode);
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
            const FGuid Guid = Obj != nullptr ? Obj->GetGUID() : FGuid::Invalid();
            if (!Guid.IsValid()) { return 0; }
            auto It = Map.GuidToIndex.Find(Guid);
            if (It != nullptr) { return (*It); }
            const uint32 Index = Map.NextIndex++;
            Map.GuidToIndex[Guid]  = Index;
            Map.IndexToGuid[Index] = Guid;
            Map.PendingExports.Add(Index);
            return Index;
        }

        // A failure is cached, so a missing asset is tried once rather than per reference, while one that died since is looked up again.
        CObject* NetObj_Resolve(FNetObjectMap& Map, uint32 Index)
        {
            if (Index == 0)
            {
                return nullptr;
            }
            auto Oit = Map.IndexToObject.Find(Index);
            if (Oit != nullptr)
            {
                if (CObject* Live = Oit->Get())
                {
                    return Live;
                }
                Map.IndexToObject.Remove(Index);
            }
            else if (Map.FailedIndices.Contains(Index))
            {
                return nullptr;
            }
            
            auto Git = Map.IndexToGuid.Find(Index);
            if (Git == nullptr)
            {
                return nullptr;
            }
            
            CObject* Obj = FindObject<CObject>(*Git);
            if (Obj == nullptr)
            {
                Obj = StaticLoadObject(*Git);
            }
            
            if (Obj == nullptr)
            {
                LOG_WARN("[Net] Replicated object index {} (GUID {}) not found and load failed -- marking failed (null).", Index, *Git->ToString());
            }
            if (Obj != nullptr)
            {
                Map.IndexToObject[Index] = Obj;
            }
            else
            {
                Map.FailedIndices.Add(Index);
            }
            return Obj;
        }

        // Get-or-assign a stable index for an asset ref, keyed by GUID (else Path) so it dedupes.
        uint32 NetAsset_GetOrAssign(FNetAssetMap& Map, const SUIDocumentRef& Ref)
        {
            if (Ref.IsNull()) { return 0; }
            const FString& Key = !Ref.Guid.IsEmpty() ? Ref.Guid : Ref.Path;
            auto It = Map.KeyToIndex.Find(Key);
            if (It != nullptr) { return (*It); }
            const uint32 Index = Map.NextIndex++;
            Map.KeyToIndex[Key]   = Index;
            Map.IndexToRef[Index] = Ref;
            Map.PendingExports.Add(Index);
            return Index;
        }

        SUIDocumentRef NetAsset_Resolve(FNetAssetMap& Map, uint32 Index)
        {
            if (Index == 0) { return SUIDocumentRef(); }
            auto It = Map.IndexToRef.Find(Index);
            return It != nullptr ? (*It) : SUIDocumentRef();
        }

        // Get-or-assign a stable index for a name, keyed by the FName itself so it dedupes.
        uint32 NetName_GetOrAssign(FNetNameMap& Map, const FName& Name)
        {
            if (Name.IsNone()) { return 0; }
            auto It = Map.KeyToIndex.Find(Name);
            if (It != nullptr) { return (*It); }
            const uint32 Index = Map.NextIndex++;
            Map.KeyToIndex[Name]   = Index;
            Map.IndexToName[Index] = Name;
            Map.PendingExports.Add(Index);
            return Index;
        }

        FName NetName_Resolve(FNetNameMap& Map, uint32 Index)
        {
            if (Index == 0) { return FName(); }
            auto It = Map.IndexToName.Find(Index);
            return It != nullptr ? (*It) : FName();
        }

        uint32 EntityToNetGuid(const FNetWorldState& State, uint32 Raw)
        {
            const ECS::FEntity Entity = static_cast<ECS::FEntity>(Raw);
            if (State.Registry == nullptr || Entity == ECS::NullEntity || !State.Registry->IsValid(Entity))
            {
                return 0u;
            }
            const SNetworkComponent* Net = State.Registry->TryGet<ECS::Read<SNetworkComponent>>(Entity);
            return Net != nullptr ? Net->NetGUID.Value : 0u;
        }

        uint32 NetGuidToEntity(const FNetWorldState& State, uint32 Guid)
        {
            const ECS::FEntity Entity = Guid != 0 ? State.GuidTable.Find(SNetGUID{ Guid }) : ECS::NullEntity;
            return static_cast<uint32>(Entity.Value);
        }
    }

    void BindWriters(FNetArchive& Ar, FNetWorldState& State)
    {
        Ar.EntityToNetGUID    = [&State](uint32 Raw)         { return EntityToNetGuid(State, Raw); };
        Ar.ObjectToNetIndex   = [&State](CObject* O)        { return NetObj_GetOrAssign(State.OutObjects, O); };
        Ar.DocumentRefToNetIndex = [&State](const SUIDocumentRef& R) { return NetAsset_GetOrAssign(State.OutAssets, R); };
        Ar.NameToNetIndex     = [&State](const FName& N)     { return NetName_GetOrAssign(State.OutNames, N); };
    }

    void BindEntityIds(FNetArchive& Ar, FNetWorldState& State)
    {
        Ar.EntityToNetGUID = [&State](uint32 Raw)  { return EntityToNetGuid(State, Raw); };
        Ar.NetGUIDToEntity = [&State](uint32 Guid) { return NetGuidToEntity(State, Guid); };
    }

    void BindReaders(FNetArchive& Ar, FNetWorldState& State, uint32 SenderConn)
    {
        FNetObjectMap& InObj = State.InObjects[SenderConn]; // operator[] default-creates the per-connection entry
        FNetAssetMap&  InAst = State.InAssets[SenderConn];
        FNetNameMap&   InNme = State.InNames[SenderConn];

        Ar.NetGUIDToEntity    = [&State](uint32 Guid) { return NetGuidToEntity(State, Guid); };

        Ar.NetIndexToObject   = [&InObj](uint32 I)
        {
            return NetObj_Resolve(InObj, I);
        };

        Ar.NetIndexToDocumentRef = [&InAst](uint32 I, SUIDocumentRef& Out)
        {
            Out = NetAsset_Resolve(InAst, I);
        };

        Ar.NetIndexToName     = [&InNme](uint32 I, FName& Out)
        {
            Out = NetName_Resolve(InNme, I);
        };
    }

    void BuildObjectExport(const FNetObjectMap& Map, const TVector<uint32>& Indices, TVector<uint8>& Batch)
    {
        AppendCountedMessages(Batch, ENetMessage::ObjectExport, Indices.Num(), [&](FNetArchive& Writer, int32 Entry)
        {
            uint32 Index = Indices[Entry];
            auto It = Map.IndexToGuid.Find(Index);
            if (It == nullptr) { return false; }
            FGuid Guid = (*It);
            Writer << Index;
            Writer << Guid;
            return true;
        });
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
            Map.IndexToObject.Remove(Index); // re-resolve lazily against the latest GUID
            Map.FailedIndices.Remove(Index);
        }
    }

    void BuildAssetExport(const FNetAssetMap& Map, const TVector<uint32>& Indices, TVector<uint8>& Batch)
    {
        AppendCountedMessages(Batch, ENetMessage::AssetExport, Indices.Num(), [&](FNetArchive& Writer, int32 Entry)
        {
            uint32 Index = Indices[Entry];
            auto It = Map.IndexToRef.Find(Index);
            if (It == nullptr) { return false; }
            FString Path = It->Path;
            FString Guid = It->Guid;
            Writer << Index;
            Writer << Path;
            Writer << Guid;
            return true;
        });
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
            SUIDocumentRef Ref;
            Ref.Path = Path;
            Ref.Guid = Guid;
            Map.IndexToRef[Index] = Ref;
        }
    }

    void BuildNameExport(const FNetNameMap& Map, const TVector<uint32>& Indices, TVector<uint8>& Batch)
    {
        AppendCountedMessages(Batch, ENetMessage::NameExport, Indices.Num(), [&](FNetArchive& Writer, int32 Entry)
        {
            uint32 Index = Indices[Entry];
            auto It = Map.IndexToName.Find(Index);
            if (It == nullptr) { return false; }
            FString Str = It->ToString();
            Writer << Index;
            Writer << Str;
            return true;
        });
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
