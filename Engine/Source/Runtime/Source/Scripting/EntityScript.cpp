#include "RuntimePCH.h"
#include "World/ECS/Registry.h"
#include "EntityScript.h"

#include "Assets/AssetTypes/Mesh/MeshBuildBatch.h"
#include "Assets/AssetTypes/Prefabs/Prefab.h"
#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ManagedInstance.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Reflection/Type/Function.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Serialization/NetArchive.h"
#include "Networking/INetworkRuntime.h"
#include "Networking/NetRealm.h"
#include "DotNet/NetScriptBridge.h"
#include "World/WorldContext.h"
#include "Core/Engine/GameLibrary.h"
#include "World/World.h"
#include "World/WorldManager.h"
#include "ScriptableObject.h"
#include "DotNet/DotNetHost.h"
#include "Input/InputActionMap.h"
#include "ScriptStruct.h"
#include "Core/Serialization/MemoryArchiver.h"
#include "Core/Serialization/ObjectArchiver.h"
#include "World/Entity/Components/EntityTags.h"
#include "Log/Log.h"
#include "TaskSystem/TaskSystem.h"

namespace Lumina
{
    namespace
    {
        // Cloned, never shared, since one object on two entities carries the first's state into the second.
        CEntityScript* CloneScript(CEntityScript* Source)
        {
            if (Source == nullptr || Source->GetClass() == nullptr)
            {
                return nullptr;
            }

            CObject* Created = NewObject(Source->GetClass(), nullptr, NAME_None, FGuid::New(), OF_Transient);
            CEntityScript* Clone = static_cast<CEntityScript*>(Created);
            if (Clone != nullptr)
            {
                Source->CopyPropertiesTo(Clone);
            }
            return Clone;
        }

        void CloneScripts(const TVector<TStrongObjectPtr<CEntityScript>>& Source, TVector<TStrongObjectPtr<CEntityScript>>& Out)
        {
            Out.clear();
            Out.reserve(Source.size());
            for (const TStrongObjectPtr<CEntityScript>& Held : Source)
            {
                if (CEntityScript* Clone = CloneScript(Held.Get()))
                {
                    Out.push_back(Clone);
                }
            }
        }
    }

    SEntityScriptComponent::SEntityScriptComponent(const SEntityScriptComponent& Other)
    {
        CloneScripts(Other.Scripts, Scripts);
        Pending = Other.Pending;
    }

    // A component dies with its entity, which is how a script destroyed by another script's update is noticed.
    SEntityScriptComponent::~SEntityScriptComponent()
    {
        if (!Scripts.empty())
        {
            EntityScripts::NoteStructureChange();
        }
    }

    SEntityScriptComponent& SEntityScriptComponent::operator=(const SEntityScriptComponent& Other)
    {
        if (this != &Other)
        {
            CloneScripts(Other.Scripts, Scripts);
            Pending = Other.Pending;
            EntityScripts::NoteStructureChange();
        }
        return *this;
    }

    SEntityScriptComponent& SEntityScriptComponent::operator=(SEntityScriptComponent&& Other)
    {
        if (this != &Other)
        {
            Scripts = Move(Other.Scripts);
            Pending = Move(Other.Pending);
            EntityScripts::NoteStructureChange();
        }
        return *this;
    }

    namespace
    {
        // Read by the managed batch between scripts, so it is a plain 32-bit counter any thread may bump.
        std::atomic<uint32> GStructureEpoch{0};
        static_assert(sizeof(std::atomic<uint32>) == sizeof(uint32) && std::atomic<uint32>::is_always_lock_free);

        // Set on whichever thread runs a chunk of the parallel pass, where a structural change breaks the promise.
        thread_local bool GInParallelScriptPass = false;

        void BumpStructureEpoch()
        {
            GStructureEpoch.fetch_add(1u, std::memory_order_relaxed);
        }

        void ReportParallelStructureChange()
        {
            static std::atomic<bool> bReported{false};
            if (!bReported.exchange(true, std::memory_order_relaxed))
            {
                LOG_ERROR("A ParallelUpdate script attached, detached, spawned or destroyed something during the parallel pass. "
                    "ParallelUpdate promises a script touches only its own entity, so the rest of that pass ran serially.");
            }
        }
    }

    struct FSyncSnapshot
    {
        ~FSyncSnapshot()
        {
            for (size_t Index = 0; Index < Fields.size(); ++Index)
            {
                Fields[Index]->DestructValue(Slot(Index));
            }
        }

        void* Slot(size_t Index) { return reinterpret_cast<uint8*>(Storage.data()) + Offsets[Index]; }

        TVector<FProperty*>         Fields;
        TVector<size_t>             Offsets;
        TVector<std::max_align_t>   Storage;
    };

    // A script destroyed outright, not detached, must still stop a batch that has it queued.
    CEntityScript::~CEntityScript()
    {
        EntityScripts::NoteStructureChange();
        delete SyncSnapshot;
    }

    // A fault is not a broken promise, so it stops a parallel pass without being reported as one.
    void CEntityScript::MarkFaulted()
    {
        bFaulted = true;
        BumpStructureEpoch();
    }

    namespace
    {
        thread_local bool   GArrivingRpc = false;
        thread_local int32  GRpcDepth    = 0;
        thread_local uint32 GRpcCaller   = 0;

        // Marks this thread as applying something that arrived, so a write it makes is not sent straight back.
        struct FReceiveScope
        {
            explicit FReceiveScope(uint32 CallerId)
                : OuterCaller(GRpcCaller)
            {
                GRpcCaller = CallerId;
                ++GRpcDepth;
            }

            ~FReceiveScope()
            {
                --GRpcDepth;
                GRpcCaller = OuterCaller;
            }

            uint32 OuterCaller;
        };

        // FNV-1a, so an id is the same on every peer and needs no table to agree on.
        uint32 HashInto(uint32 Hash, const char* Text)
        {
            for (; *Text != '\0'; ++Text)
            {
                Hash ^= static_cast<uint8>(*Text);
                Hash *= 16777619u;
            }
            return Hash;
        }

        uint32 RpcIdOf(const CClass* Class, const FFunction& Function)
        {
            uint32 Hash = HashInto(2166136261u, Class->GetName().c_str());
            Hash = HashInto(Hash, ".");
            return HashInto(Hash, Function.GetFunctionName().c_str());
        }

        ERpcTarget TargetOf(const FFunction& Function)
        {
            const EFunctionFlags Flags = Function.GetFunctionFlags();
            if (EnumHasAnyFlags(Flags, EFunctionFlags::RpcHost))
            {
                return ERpcTarget::Host;
            }
            return EnumHasAnyFlags(Flags, EFunctionFlags::RpcOwner) ? ERpcTarget::Owner : ERpcTarget::Broadcast;
        }

        // A frame big enough for the function's parameters, aligned the way FFunction expects.
        struct FCallFrame
        {
            explicit FCallFrame(const FFunction& InFunction)
                : Function(InFunction)
                , Storage(InFunction.GetParmsSize() / sizeof(std::max_align_t) + 1)
            {
                Function.InitializeFrame(Data());
            }

            ~FCallFrame()
            {
                Function.DestructFrame(Data());
            }

            void* Data() { return Storage.data(); }

            const FFunction& Function;
            TVector<std::max_align_t> Storage;
        };

        // One value of a property outside any container, for a write that has not been accepted yet.
        struct FValueBuffer
        {
            explicit FValueBuffer(const FProperty& InField)
                : Field(InField)
                , Storage(InField.GetElementSize() / sizeof(std::max_align_t) + 1)
            {
                Field.ConstructValue(Data());
            }

            ~FValueBuffer()
            {
                Field.DestructValue(Data());
            }

            void* Data() { return Storage.data(); }

            const FProperty& Field;
            TVector<std::max_align_t> Storage;
        };

        // Only entity ids are bound, so names, objects and assets travel inline and a relayed call reads the same on every peer.
        void BindCallArchive(FNetArchive& Archive, const CWorld* World)
        {
            if (INetworkRuntime* Net = GetNetworkRuntime())
            {
                Net->BindEntityIds(World, Archive);
            }
        }

        const FFunction* FindNamedFunction(const CEntityScript* Script, const FProperty* Field, FStringView Key)
        {
            const FCStringView Name = Field->GetMetadata(Key);
            if (Name.empty())
            {
                return nullptr;
            }
            const FFunction* Function = Script->GetClass()->FindFunction(FName(FString(Name.data(), Name.size()).c_str()));
            if (Function == nullptr)
            {
                LOG_WARN("{}.{} names {} function '{}', which is not a reflected function of the script.",
                    Script->GetClass()->GetName().c_str(), Field->GetPropertyName().c_str(), FString(Key.data(), Key.size()).c_str(),
                    FString(Name.data(), Name.size()).c_str());
            }
            return Function;
        }
    }

    bool CEntityScript::IsHost() const
    {
        return OwningWorld == nullptr || OwningWorld->GetNetMode() != ENetMode::Client;
    }

    bool CEntityScript::IsOwner() const
    {
        INetworkRuntime* Net = GetNetworkRuntime();
        if (Net == nullptr || OwningWorld == nullptr || !Net->IsEntityNetworked(OwningWorld, OwningEntity))
        {
            return true;
        }
        return Net->GetEntityOwner(OwningWorld, OwningEntity) == Net->GetLocalConnectionId(OwningWorld);
    }

    uint32 CEntityScript::GetRpcCaller() const
    {
        if (GRpcDepth > 0)
        {
            return GRpcCaller;
        }
        INetworkRuntime* Net = GetNetworkRuntime();
        return (Net != nullptr && OwningWorld != nullptr) ? Net->GetLocalConnectionId(OwningWorld) : 0u;
    }

    bool CEntityScript::IsReceivingRpc()
    {
        return GRpcDepth > 0;
    }

    uint32 CEntityScript::GetReceivingCaller()
    {
        return GRpcDepth > 0 ? GRpcCaller : 0u;
    }

    void CEntityScript::MarkNetDirty()
    {
        if (INetworkRuntime* Net = GetNetworkRuntime(); Net != nullptr && OwningWorld != nullptr)
        {
            Net->MarkEntityDirty(OwningWorld, OwningEntity);
        }
    }

    ERpcRoute CEntityScript::PlanRpc(const FFunction& Function)
    {
        // The mark belongs to exactly one call, so an RPC made from inside a received one still routes.
        if (GArrivingRpc)
        {
            GArrivingRpc = false;
            return ERpcRoute::RunHere;
        }

        INetworkRuntime* Net = GetNetworkRuntime();
        if (Net == nullptr || OwningWorld == nullptr || OwningWorld->GetNetMode() == ENetMode::Standalone)
        {
            return ERpcRoute::RunHere;
        }

        switch (TargetOf(Function))
        {
            case ERpcTarget::Host:
                return IsHost() ? ERpcRoute::RunHere : ERpcRoute::SendOnly;

            case ERpcTarget::Owner:
                return Net->GetEntityOwner(OwningWorld, OwningEntity) == Net->GetLocalConnectionId(OwningWorld)
                    ? ERpcRoute::RunHere : ERpcRoute::SendOnly;

            default:
                return ERpcRoute::SendAndRunHere;
        }
    }

    void CEntityScript::SendRpc(const FFunction& Function, const void* const* Values)
    {
        INetworkRuntime* Net = GetNetworkRuntime();
        if (Net == nullptr || OwningWorld == nullptr)
        {
            return;
        }

        TVector<uint8> Payload;
        FNetArchive Writer(Payload);
        BindCallArchive(Writer, OwningWorld);
        const TSpan<FProperty* const> Arguments = Function.GetArguments();
        for (size_t Index = 0; Index < Arguments.size(); ++Index)
        {
            Arguments[Index]->NetSerialize(Writer, const_cast<void*>(Values[Index]));
        }
        Writer.AlignToByte();

        const bool bUnreliable = EnumHasAnyFlags(Function.GetFunctionFlags(), EFunctionFlags::NetUnreliable);
        const uint8 Flags = static_cast<uint8>(bUnreliable ? ENetFlags::Unreliable : ENetFlags::None);
        Net->SendScriptRpc(this, RpcIdOf(GetClass(), Function), TargetOf(Function), Flags, Payload.data(), static_cast<uint32>(Payload.size()));
    }

    void CEntityScript::SendRpcFrame(const FFunction& Function, const void* Frame)
    {
        const TSpan<FProperty* const> Arguments = Function.GetArguments();
        TVector<const void*> Values(Arguments.size());
        for (size_t Index = 0; Index < Arguments.size(); ++Index)
        {
            Values[Index] = Arguments[Index]->GetValuePtr<void>(Frame);
        }
        SendRpc(Function, Values.data());
    }

    bool CEntityScript::RouteRpcValues(const char* FunctionName, const void* const* Values, uint32 NumValues)
    {
        const FFunction* Function = GetClass()->FindFunction(FName(FunctionName));
        if (Function == nullptr || !Function->IsRpc())
        {
            GArrivingRpc = false;
            LOG_ERROR("NET_RPC in {}::{} needs the function declared FUNCTION(Rpc = Broadcast, Host or Owner).", GetClass()->GetName().c_str(), FunctionName);
            return false;
        }

        const ERpcRoute Route = PlanRpc(*Function);
        if (Route == ERpcRoute::RunHere)
        {
            return false;
        }

        if (Function->GetArguments().size() != NumValues)
        {
            LOG_ERROR("NET_RPC in {}::{} has to pass all {} parameters, in order.", GetClass()->GetName().c_str(), FunctionName, Function->GetArguments().size());
            return false;
        }

        SendRpc(*Function, Values);
        return Route == ERpcRoute::SendOnly;
    }

    void CEntityScript::ReceiveRpc(uint32 RpcId, uint32 CallerId, const uint8* Payload, uint32 PayloadSize)
    {
        INetworkRuntime* Net = GetNetworkRuntime();
        for (const FFunction* Function : GetClass()->GetFunctions())
        {
            if (!Function->IsRpc() || RpcIdOf(GetClass(), *Function) != RpcId)
            {
                continue;
            }

            // Owner-only calls are refused unless they came from the entity's owner or from the host.
            if (EnumHasAnyFlags(Function->GetFunctionFlags(), EFunctionFlags::NetOwnerOnly) && CallerId != 0
                && (Net == nullptr || Net->GetEntityOwner(OwningWorld, OwningEntity) != CallerId))
            {
                return;
            }

            FCallFrame Frame(*Function);
            FNetArchive Reader(Payload, PayloadSize);
            BindCallArchive(Reader, OwningWorld);
            for (FProperty* Argument : Function->GetArguments())
            {
                Argument->NetSerialize(Reader, Argument->GetValuePtr<void>(Frame.Data()));
            }
            if (Reader.HasError())
            {
                LOG_WARN("[Net] RPC {}::{} arrived truncated.", GetClass()->GetName().c_str(), Function->GetFunctionName().c_str());
                return;
            }

            FReceiveScope Scope(CallerId);
            GArrivingRpc = true;
            Function->Invoke(this, Frame.Data());
            GArrivingRpc = false;
            return;
        }

        TVector<FProperty*> Fields;
        GetClass()->GetNetReplicatedProperties(Fields);
        for (FProperty* Field : Fields)
        {
            if (!IsSyncFromOwner(Field))
            {
                continue;
            }
            const bool bCorrection = RpcId == SyncFieldId(Field, true);
            if (bCorrection || RpcId == SyncFieldId(Field, false))
            {
                ReceiveSync(Field, bCorrection, CallerId, Payload, PayloadSize);
                return;
            }
        }

        LOG_WARN("[Net] {} has no RPC or Sync field with id {}; the peers are running different builds.", GetClass()->GetName().c_str(), RpcId);
    }

    void CEntityScript::ReceiveSync(FProperty* Field, bool bCorrection, uint32 CallerId, const uint8* Payload, uint32 PayloadSize)
    {
        INetworkRuntime* Net = GetNetworkRuntime();
        if (Net == nullptr || OwningWorld == nullptr)
        {
            return;
        }

        // A proposal is the host's to judge and comes only from the owner. A correction is the owner's to take.
        if (bCorrection == IsHost())
        {
            return;
        }
        if (!bCorrection && CallerId != 0 && Net->GetEntityOwner(OwningWorld, OwningEntity) != CallerId)
        {
            return;
        }

        FValueBuffer Value(*Field);
        FNetArchive Reader(Payload, PayloadSize);
        BindCallArchive(Reader, OwningWorld);
        Field->NetSerialize(Reader, Value.Data());
        if (Reader.HasError())
        {
            return;
        }

        FReceiveScope Scope(CallerId);
        if (!bCorrection && !ValidateSync(Field, Value.Data()))
        {
            SendSync(Field, true);
            return;
        }
        if (Field->Identical(Field->GetValuePtr<void>(this), Value.Data()))
        {
            return;
        }

        ApplySync(Field, Value.Data());
        if (!bCorrection)
        {
            MarkNetDirty();
        }
    }

    void CEntityScript::ApplySync(FProperty* Field, const void* Value)
    {
        const char* Name = Field->GetPropertyName().c_str();
        const uint32 NameSize = static_cast<uint32>(strlen(Name) + 1);
        NetScripts::DispatchSyncChanging(this, Name, NameSize);
        Field->CopyCompleteValue(Field->GetValuePtr<void>(this), Value);
        NetScripts::DispatchSyncChanged(this, Name, NameSize);
    }

    uint32 CEntityScript::SyncFieldId(const FProperty* Field, bool bCorrection) const
    {
        uint32 Hash = HashInto(2166136261u, GetClass()->GetName().c_str());
        Hash = HashInto(Hash, ".");
        Hash = HashInto(Hash, Field->GetPropertyName().c_str());
        return HashInto(Hash, bCorrection ? "#correct" : "#sync");
    }

    bool CEntityScript::IsSyncFromOwner(const FProperty* Field)
    {
        const FCStringView Sync = Field->GetMetadata("Sync");
        return FStringView(Sync.data(), Sync.size()).find("FromOwner") != FStringView::npos;
    }

    void CEntityScript::NotifySyncChanged(const FProperty* Field, const void* OldValue)
    {
        const FFunction* Function = FindNamedFunction(this, Field, "Change");
        if (Function == nullptr)
        {
            return;
        }

        const TSpan<FProperty* const> Arguments = Function->GetArguments();
        if (Arguments.size() != 0 && Arguments.size() != 2)
        {
            LOG_WARN("{}::{} is a change handler, so it takes nothing or (Old, New).", GetClass()->GetName().c_str(), Function->GetFunctionName().c_str());
            return;
        }

        FCallFrame Frame(*Function);
        if (Arguments.size() == 2)
        {
            Arguments[0]->CopyCompleteValue(Arguments[0]->GetValuePtr<void>(Frame.Data()), OldValue);
            Arguments[1]->CopyCompleteValue(Arguments[1]->GetValuePtr<void>(Frame.Data()), Field->GetValuePtr<void>(this));
        }
        Function->Invoke(this, Frame.Data());
    }

    bool CEntityScript::ValidateSync(const FProperty* Field, const void* Proposed)
    {
        const FFunction* Function = FindNamedFunction(this, Field, "Validate");
        if (Function == nullptr)
        {
            return true;
        }

        const TSpan<FProperty* const> Arguments = Function->GetArguments();
        const FProperty* Return = Function->GetReturnParam();
        if (Arguments.size() != 1 || Return == nullptr || Return->GetType() != EPropertyTypeFlags::Bool)
        {
            LOG_WARN("{}::{} validates a Sync field, so it takes the proposed value and returns bool.", GetClass()->GetName().c_str(), Function->GetFunctionName().c_str());
            return true;
        }

        FCallFrame Frame(*Function);
        Arguments[0]->CopyCompleteValue(Arguments[0]->GetValuePtr<void>(Frame.Data()), Proposed);
        Function->Invoke(this, Frame.Data());
        return *Return->GetValuePtr<bool>(Frame.Data());
    }

    void CEntityScript::QuantizeSync(const FProperty* Field)
    {
        const FCStringView Text = Field->GetMetadata("Quantize");
        const double Step = Text.empty() ? 0.0 : std::strtod(Text.data(), nullptr);
        if (Step <= 0.0)
        {
            return;
        }

        void* Value = Field->GetValuePtr<void>(this);
        auto Snap = [Step](double In) { return std::round(In / Step) * Step; };
        switch (Field->GetType())
        {
            case EPropertyTypeFlags::Float:  *static_cast<float*>(Value)  = static_cast<float>(Snap(*static_cast<float*>(Value))); break;
            case EPropertyTypeFlags::Double: *static_cast<double*>(Value) = Snap(*static_cast<double*>(Value)); break;
            case EPropertyTypeFlags::Int8:   *static_cast<int8*>(Value)   = static_cast<int8>(Snap(*static_cast<int8*>(Value))); break;
            case EPropertyTypeFlags::Int16:  *static_cast<int16*>(Value)  = static_cast<int16>(Snap(*static_cast<int16*>(Value))); break;
            case EPropertyTypeFlags::Int32:  *static_cast<int32*>(Value)  = static_cast<int32>(Snap(*static_cast<int32*>(Value))); break;
            case EPropertyTypeFlags::Int64:  *static_cast<int64*>(Value)  = static_cast<int64>(Snap(static_cast<double>(*static_cast<int64*>(Value)))); break;
            case EPropertyTypeFlags::UInt8:  *static_cast<uint8*>(Value)  = static_cast<uint8>(Snap(*static_cast<uint8*>(Value))); break;
            case EPropertyTypeFlags::UInt16: *static_cast<uint16*>(Value) = static_cast<uint16>(Snap(*static_cast<uint16*>(Value))); break;
            case EPropertyTypeFlags::UInt32: *static_cast<uint32*>(Value) = static_cast<uint32>(Snap(*static_cast<uint32*>(Value))); break;
            case EPropertyTypeFlags::UInt64: *static_cast<uint64*>(Value) = static_cast<uint64>(Snap(static_cast<double>(*static_cast<uint64*>(Value)))); break;
            default: break;
        }
    }

    void CEntityScript::SendSync(const FProperty* Field, bool bCorrection)
    {
        INetworkRuntime* Net = GetNetworkRuntime();
        if (Net == nullptr || OwningWorld == nullptr)
        {
            return;
        }
        TVector<uint8> Payload;
        FNetArchive Writer(Payload);
        BindCallArchive(Writer, OwningWorld);
        const_cast<FProperty*>(Field)->NetSerialize(Writer, Field->GetValuePtr<void>(this));
        Writer.AlignToByte();
        Net->SendScriptRpc(this, SyncFieldId(Field, bCorrection), bCorrection ? ERpcTarget::Owner : ERpcTarget::Host,
            static_cast<uint8>(ENetFlags::None), Payload.data(), static_cast<uint32>(Payload.size()));
    }

    FSyncSnapshot& CEntityScript::EnsureSyncSnapshot()
    {
        if (SyncSnapshot != nullptr)
        {
            return *SyncSnapshot;
        }

        SyncSnapshot = new FSyncSnapshot();
        GetClass()->GetNetReplicatedProperties(SyncSnapshot->Fields);
        size_t Size = 0;
        for (const FProperty* Field : SyncSnapshot->Fields)
        {
            SyncSnapshot->Offsets.push_back(Size);
            Size += (Field->GetElementSize() + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t) * sizeof(std::max_align_t);
        }
        SyncSnapshot->Storage.resize(Size / sizeof(std::max_align_t) + 1);
        for (size_t Index = 0; Index < SyncSnapshot->Fields.size(); ++Index)
        {
            FProperty* Field = SyncSnapshot->Fields[Index];
            Field->ConstructValue(SyncSnapshot->Slot(Index));
            Field->CopyCompleteValue(SyncSnapshot->Slot(Index), Field->GetValuePtr<void>(this));
        }
        return *SyncSnapshot;
    }

    void CEntityScript::PollSync()
    {
        if (OwningWorld == nullptr || OwningWorld->GetNetMode() == ENetMode::Standalone || bFaulted)
        {
            return;
        }

        // The first sighting is the baseline, which the entity's initial replication already carries.
        if (SyncSnapshot == nullptr)
        {
            EnsureSyncSnapshot();
            return;
        }

        const bool bHost = IsHost();
        const bool bOwner = !bHost && IsOwner();
        bool bDirty = false;
        for (size_t Index = 0; Index < SyncSnapshot->Fields.size(); ++Index)
        {
            FProperty* Field = SyncSnapshot->Fields[Index];
            const bool bWriter = bHost || (bOwner && IsSyncFromOwner(Field));
            if (bWriter)
            {
                QuantizeSync(Field);
            }

            void* Seen = SyncSnapshot->Slot(Index);
            if (Field->Identical(Seen, Field->GetValuePtr<void>(this)))
            {
                continue;
            }

            if (bHost)
            {
                bDirty = true;
            }
            else if (bWriter)
            {
                SendSync(Field, false);
            }
            NotifySyncChanged(Field, Seen);
            Field->CopyCompleteValue(Seen, Field->GetValuePtr<void>(this));
        }

        if (bDirty)
        {
            MarkNetDirty();
        }
    }

    void CEntityScript::SyncArriving()
    {
        EnsureSyncSnapshot();
    }

    void CEntityScript::SyncArrived(const char* Names, uint32 NamesSize)
    {
        if (SyncSnapshot == nullptr)
        {
            return;
        }

        for (uint32 Start = 0; Start < NamesSize;)
        {
            const char* Name = Names + Start;
            Start += static_cast<uint32>(strlen(Name) + 1);
            for (size_t Index = 0; Index < SyncSnapshot->Fields.size(); ++Index)
            {
                FProperty* Field = SyncSnapshot->Fields[Index];
                if (strcmp(Field->GetPropertyName().c_str(), Name) != 0)
                {
                    continue;
                }
                void* Seen = SyncSnapshot->Slot(Index);
                if (!Field->Identical(Seen, Field->GetValuePtr<void>(this)))
                {
                    NotifySyncChanged(Field, Seen);
                    Field->CopyCompleteValue(Seen, Field->GetValuePtr<void>(this));
                }
                break;
            }
        }
    }

    bool SEntityScriptComponent::Serialize(FArchive& Ar)
    {
        if (Ar.IsWriting())
        {
            int32 Count = (int32)Pending.size();
            for (const TStrongObjectPtr<CEntityScript>& Held : Scripts)
            {
                if (Held.Get() != nullptr && Held.Get()->GetClass() != nullptr)
                {
                    ++Count;
                }
            }
            Ar << Count;

            // Written back byte for byte, so a world saved while a script failed to compile keeps it.
            for (FPendingScript& Held : Pending)
            {
                Ar << Held.ClassName;

                int64 ScriptSize = (int64)Held.Bytes.size();
                Ar << ScriptSize;
                if (ScriptSize > 0)
                {
                    Ar.Serialize(Held.Bytes.data(), ScriptSize);
                }
            }

            for (const TStrongObjectPtr<CEntityScript>& Held : Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (Script == nullptr || Script->GetClass() == nullptr)
                {
                    continue;
                }
                FName ClassName = Script->GetClass()->GetName();
                Ar << ClassName;

                // Length-prefixed, so a reader that cannot resolve this class can skip exactly this script.
                const int64 SizePos = Ar.Tell();
                int64 ScriptSize = 0;
                Ar << ScriptSize;

                const int64 DataStart = Ar.Tell();
                Script->GetClass()->SerializeTaggedProperties(Ar, Script);
                const int64 DataEnd = Ar.Tell();

                ScriptSize = DataEnd - DataStart;
                Ar.Seek(SizePos);
                Ar << ScriptSize;
                Ar.Seek(DataEnd);
            }
            return true;
        }

        if (Ar.IsReading())
        {
            int32 Count = 0;
            Ar << Count;

            // Older files have no per-script length, so an unresolvable class there still ends the record.
            const bool bLengthPrefixed =
                Ar.GetFileVersion() >= (int32)ELuminaEngineVersion::ENTITY_SCRIPT_LENGTH_PREFIX;

            Scripts.clear();
            Pending.clear();
            for (int32 Index = 0; Index < Count; ++Index)
            {
                FName ClassName;
                Ar << ClassName;

                int64 ScriptSize = 0;
                int64 DataStart  = 0;
                if (bLengthPrefixed)
                {
                    Ar << ScriptSize;
                    DataStart = Ar.Tell();
                }

                // Resolved through the redirect registry, since an alias is what carries a renamed class across.
                CClass* ScriptClass = FScriptableRegistry::ResolveClass(ClassName);
                CEntityScript* Script = nullptr;
                if (ScriptClass != nullptr && ScriptClass->IsChildOf(CEntityScript::StaticClass()))
                {
                    Script = static_cast<CEntityScript*>(
                        NewObject(ScriptClass, nullptr, NAME_None, FGuid::New(), OF_Transient));
                }

                if (Script == nullptr)
                {
                    if (!bLengthPrefixed)
                    {
                        LOG_WARN("SEntityScriptComponent: script class '{}' could not be resolved and this "
                                 "file has no per-script length, so the rest were dropped.", ClassName.c_str());
                        break;
                    }

                    // Held verbatim rather than discarded, since the class is usually just not loaded yet.
                    FPendingScript Held;
                    Held.ClassName   = ClassName;
                    Held.FileVersion = Ar.GetFileVersion();
                    Held.MakeReader  = Ar.GetDeferredReaderFactory();
                    Held.Bytes.resize((size_t)Math::Max<int64>(ScriptSize, 0));
                    if (ScriptSize > 0)
                    {
                        Ar.Serialize(Held.Bytes.data(), ScriptSize);
                    }
                    Pending.push_back(Move(Held));

                    LOG_WARN("SEntityScriptComponent: script class '{}' is not loaded; holding it until a "
                             "script reload can restore it.", ClassName.c_str());
                    Ar.Seek(DataStart + ScriptSize);
                    continue;
                }

                ScriptClass->SerializeTaggedProperties(Ar, Script);

                // Trusted over wherever the property reader stopped, so drift cannot shift the next script.
                if (bLengthPrefixed)
                {
                    Ar.Seek(DataStart + ScriptSize);
                }

                Scripts.push_back(Script);
            }
            return true;
        }

        return true;
    }

    namespace
    {
        // Snapshotting as a strong ref keeps a script detached mid-pass alive until the snapshot dies.

        using FScriptSnapshot = TVector<TStrongObjectPtr<CEntityScript>>;

        // Counted by the PrePhysics pass, which visits every script, so a world with none in the later phase skips that walk.
        struct FScriptPhaseCensus
        {
            uint32 PostPhysicsScripts = 0;
        };

        // Runs OnUpdate for a run of C# scripts in one crossing and returns how many ran before the epoch moved.
        DotNet::TManagedExport<int32 (*)(void* const*, int32, float, const uint32*)> GDispatchUpdates("DispatchEntityScriptUpdates");

        // Whether a class's OnUpdate is a C# override, which is what the batch can run without the native shim.
        bool HasManagedUpdate(const CClass* Class)
        {
            static const FName OnUpdateName("OnUpdate");
            return Class != nullptr && FindScriptOverride(Class, OnUpdateName) != nullptr;
        }

        // Below this many scripts the pass stays on the calling thread, where a fan-out costs more than it saves.
        constexpr uint32 kParallelMinScripts   = 512;
        constexpr uint32 kParallelChunkScripts = 256;

        // Inherited like the C# attribute, so a subclass keeps its parent's promise.
        bool IsParallelUpdate(const CClass* Class)
        {
            static const FName ParallelUpdateName("ParallelUpdate");
            for (const CClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
            {
                const CScriptClass* Minted = ToScriptClass(Current);
                if ((Minted != nullptr && Minted->bScriptParallelUpdate) || Current->HasMeta(ParallelUpdateName))
                {
                    return true;
                }
            }
            return false;
        }

        // Cut on entity boundaries, so every script of one entity runs on one thread in its usual order.
        void BuildEntityChunks(const TVector<ECS::FEntity>& Entities, TVector<uint32>& OutStarts)
        {
            OutStarts.clear();
            const uint32 Num = (uint32)Entities.size();
            uint32 Start = 0;
            while (Start < Num)
            {
                OutStarts.push_back(Start);
                uint32 End = Start + kParallelChunkScripts < Num ? Start + kParallelChunkScripts : Num;
                while (End < Num && Entities[End] == Entities[End - 1])
                {
                    ++End;
                }
                Start = End;
            }
            OutStarts.push_back(Num);
        }

        template<typename TFunc>
        void RunChunks(uint32 NumScripts, uint32 NumChunks, TFunc&& RunChunk)
        {
            if (NumScripts < kParallelMinScripts || NumChunks <= 1)
            {
                for (uint32 Chunk = 0; Chunk < NumChunks; ++Chunk)
                {
                    RunChunk(Chunk);
                }
                return;
            }
            Task::ParallelFor(NumChunks, RunChunk, 1);
        }

        // Phase, opt-in and override are class data, so a run of one class resolves them once.
        struct FClassFacts
        {
            const CClass*      Class     = nullptr;
            bool               bParallel = false;
            bool               bManaged  = false;
            EScriptUpdatePhase Phase     = EScriptUpdatePhase::PrePhysics;

            void See(const CEntityScript* Script);
        };

        enum class EClaimResult : uint8
        {
            Skipped,
            Claimed,
            NeedsInstance,
        };

        struct FClaimChunk
        {
            TVector<void*>          Handles;
            TVector<CEntityScript*> Scripts;
            TVector<ECS::FEntity>   Entities;
            uint32                  PostPhysics = 0;
            uint32                  Ran         = 0;
        };

        // Above this many entities the claim and the run share one pass, cut into chunks of this size.
        constexpr uint32 kParallelClaimMinEntities = 2048;
        constexpr uint32 kParallelClaimChunk       = 512;

        class FParallelScriptPassScope
        {
        public:

            FParallelScriptPassScope()  { GInParallelScriptPass = true; }
            ~FParallelScriptPassScope() { GInParallelScriptPass = false; }

            LE_NO_COPYMOVE(FParallelScriptPassScope);
        };

        // Both tick passes rebuild these every call, so they are parked per thread rather than reallocated.
        struct FTickScratch
        {
            TVector<ECS::FEntity> Entities;
            FScriptSnapshot       Scripts;

            // Unpinned, since freeing a queued script means detaching or destroying it, and either one moves the epoch.
            TVector<void*>          BatchHandles;
            TVector<ECS::FEntity>   BatchEntities;
            TVector<CEntityScript*> BatchScripts;

            // One entry per script in entity order, with a null handle for a script that is called directly.
            TVector<uint8>          Claimed;
            TVector<void*>          ParallelHandles;
            TVector<CEntityScript*> ParallelScripts;
            TVector<ECS::FEntity>   ParallelEntities;
            TVector<uint32>         ChunkStarts;
            TVector<uint32>         ChunkRan;
            TVector<FClaimChunk>    ClaimChunks;
        };

        // A nested tick falls back to its own storage rather than walking the outer pass's buffers.
        class FTickScratchGuard
        {
        public:

            FTickScratchGuard()
            {
                if (Depth()++ == 0u)
                {
                    Parked = &ParkedStorage();
                }
            }

            ~FTickScratchGuard() { --Depth(); }

            LE_NO_COPYMOVE(FTickScratchGuard);

            FTickScratch& Get() { return (Parked != nullptr) ? *Parked : Local; }

        private:

            static uint32& Depth()               { static thread_local uint32 Value = 0; return Value; }
            static FTickScratch& ParkedStorage() { static thread_local FTickScratch Storage; return Storage; }

            FTickScratch* Parked = nullptr;
            FTickScratch  Local;
        };

        void SnapshotScripts(ECS::FRegistry& Registry, ECS::FEntity Entity, FScriptSnapshot& Out)
        {
            Out.clear();

            // try_get on a dead entity indexes the sparse set out of bounds rather than returning null.
            if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
            {
                return;
            }

            SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return;
            }

            Out.reserve(Component->Scripts.size());
            for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                if (Held.Get() != nullptr)
                {
                    Out.push_back(Held);
                }
            }
        }

        // Type-uniform, so it is one class-level byte rather than anything stored per script instance.
        EScriptUpdatePhase ScriptPhase(const CEntityScript* Script)
        {
            const CScriptClass* Class = Script != nullptr ? ToScriptClass(Script->GetClass()) : nullptr;
            return Class != nullptr ? static_cast<EScriptUpdatePhase>(Class->ScriptUpdatePhase)
                                    : EScriptUpdatePhase::PrePhysics;
        }

        bool IsOutsideRealm(const CEntityScript* Script, CWorld* World)
        {
            return !NetRealm::Allows(Script->GetClass(), World);
        }

        // Re-resolved per dispatch, since an earlier callback may have removed this script or its entity.
        bool IsStillAttached(ECS::FRegistry& Registry, ECS::FEntity Entity, const CEntityScript* Script)
        {
            if (Script == nullptr || !Registry.IsValid(Entity))
            {
                return false;
            }

            const SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return false;
            }

            for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                if (Held.Get() == Script)
                {
                    return true;
                }
            }
            return false;
        }

        // A loaded prefab owns a registry and is not a world, so the world sweep alone left its scripts.
        // A script attached to a new entity during the pass readies on the next tick like any other.
        void SnapshotScriptedEntities(ECS::FRegistry& Registry, TVector<ECS::FEntity>& Out)
        {
            Out.clear();

            // With nothing excluded and no holes the view is the pool itself, copied back to front in its walk order.
            const auto Pool     = Registry.FindStorage<SEntityScriptComponent>();
            const auto Disabled = Registry.FindStorage<SDisabledTag>();
            const auto Paused   = Registry.FindStorage<SScriptDisabledTag>();
            if (Pool && !Pool->HasTombstones() && (!Disabled || Disabled->IsEmpty()) && (!Paused || Paused->IsEmpty()))
            {
                const ECS::FEntity* Dense = Pool->GetDenseData();
                const size_t        Num   = Pool->GetDenseSize();
                Out.resize(Num);
                for (size_t Index = 0; Index < Num; ++Index)
                {
                    Out[Index] = Dense[Num - 1u - Index];
                }
                return;
            }

            auto View = Registry.View<SEntityScriptComponent>(ECS::TExclude<SDisabledTag, SScriptDisabledTag>{});
            Out.reserve(View.Num());
            for (ECS::FEntity Entity : View)
            {
                Out.push_back(Entity);
            }
        }

        void FClassFacts::See(const CEntityScript* Script)
        {
            if (Script->GetClass() != Class)
            {
                Class     = Script->GetClass();
                bParallel = IsParallelUpdate(Class);
                bManaged  = HasManagedUpdate(Class);
                Phase     = ScriptPhase(Script);
            }
        }

        // Queues the entity's ticking scripts when all of them promised to touch nothing else and owe no lifecycle.
        EClaimResult ClaimEntity(ECS::FRegistry& Registry, ECS::FEntity Entity, EScriptUpdatePhase Phase, bool bDrainLifecycle,
                                 bool bMayCreateInstances, FClassFacts& Facts, TVector<void*>& OutHandles,
                                 TVector<CEntityScript*>& OutScripts, TVector<ECS::FEntity>& OutEntities, uint32& OutPostPhysics)
        {
            const SEntityScriptComponent* Component = (Entity != ECS::NullEntity && Registry.IsValid(Entity))
                ? Registry.TryGet<SEntityScriptComponent>(Entity) : nullptr;
            if (Component == nullptr)
            {
                return EClaimResult::Skipped;
            }

            bool   bAnyTicking     = false;
            uint32 PostPhysicsHere = 0;
            for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                const CEntityScript* Script = Held.Get();
                if (Script == nullptr || (bDrainLifecycle && (Script->GetOwningEntity() == ECS::NullEntity || (!Script->IsFaulted() && !Script->IsReady()))))
                {
                    return EClaimResult::Skipped;
                }
                Facts.See(Script);
                PostPhysicsHere += (Facts.Phase == EScriptUpdatePhase::PostPhysics) ? 1u : 0u;
                if (!Script->ShouldTick() || Facts.Phase != Phase)
                {
                    continue;
                }
                if (!Facts.bParallel)
                {
                    return EClaimResult::Skipped;
                }
                bAnyTicking = true;
            }
            if (!bAnyTicking)
            {
                return EClaimResult::Skipped;
            }

            const size_t Start = OutScripts.size();
            for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                CEntityScript* Script = Held.Get();
                Facts.See(Script);
                if (!Script->ShouldTick() || Facts.Phase != Phase)
                {
                    continue;
                }

                void* Handle = nullptr;
                if (Facts.bManaged)
                {
                    const uint32 Generation = ManagedInstances::GetHandleGeneration();
                    Handle = Script->CachedHandleGeneration == Generation ? Script->CachedManagedHandle : nullptr;
                    if (Handle == nullptr)
                    {
                        // Creating one runs a managed constructor, which only the serial pass may do.
                        if (!bMayCreateInstances)
                        {
                            OutHandles.resize(Start);
                            OutScripts.resize(Start);
                            OutEntities.resize(Start);
                            return EClaimResult::NeedsInstance;
                        }
                        Handle = Scriptable::GetOrCreateInstance(Script);
                        Script->CachedManagedHandle    = Handle;
                        Script->CachedHandleGeneration = Generation;
                    }
                    if (Handle == nullptr)
                    {
                        OutHandles.resize(Start);
                        OutScripts.resize(Start);
                        OutEntities.resize(Start);
                        return EClaimResult::Skipped;
                    }
                }
                OutHandles.push_back(Handle);
                OutScripts.push_back(Script);
                OutEntities.push_back(Entity);
            }

            OutPostPhysics += PostPhysicsHere;
            return EClaimResult::Claimed;
        }

        // Runs claimed scripts in order until the structure epoch moves, returning how many ran.
        uint32 RunClaimedSpan(void* const* Handles, CEntityScript* const* Scripts, uint32 Count, float DeltaTime, uint32 ClaimEpoch)
        {
            auto* Dispatch = GDispatchUpdates.Get();
            const uint32* EpochAddress = reinterpret_cast<const uint32*>(&GStructureEpoch);

            uint32 Index = 0;
            while (Index < Count && GStructureEpoch.load(std::memory_order_relaxed) == ClaimEpoch)
            {
                if (Handles[Index] == nullptr)
                {
                    CEntityScript* Script = Scripts[Index];
                    LUMINA_PROFILE_SECTION_NAMED(Script->GetClass()->GetName().c_str());
                    Script->OnUpdate(DeltaTime);
                    ++Index;
                    continue;
                }
                if (Dispatch == nullptr)
                {
                    break;
                }

                uint32 RunEnd = Index;
                while (RunEnd < Count && Handles[RunEnd] != nullptr)
                {
                    ++RunEnd;
                }
                LUMINA_PROFILE_SECTION_NAMED(Scripts[Index]->GetClass()->GetName().c_str());
                const uint32 Ran = (uint32)Dispatch(const_cast<void**>(Handles) + Index, (int32)(RunEnd - Index), DeltaTime, EpochAddress);
                Index += Ran;
                if (Index < RunEnd)
                {
                    break;
                }
            }
            return Index;
        }

        // Claims every entity whose ticking scripts promised to touch nothing else, creating managed instances as needed.
        uint32 ClaimEntitiesSerial(ECS::FRegistry& Registry, const TVector<ECS::FEntity>& Entities, EScriptUpdatePhase Phase,
                                   bool bDrainLifecycle, FTickScratch& Scratch)
        {
            const uint32 NumEntities = (uint32)Entities.size();
            uint32      PostPhysics = 0;
            FClassFacts Facts;
            for (uint32 EntityIndex = 0; EntityIndex < NumEntities; ++EntityIndex)
            {
                if (ClaimEntity(Registry, Entities[EntityIndex], Phase, bDrainLifecycle, true, Facts,
                                Scratch.ParallelHandles, Scratch.ParallelScripts, Scratch.ParallelEntities, PostPhysics) == EClaimResult::Claimed)
                {
                    Scratch.Claimed[EntityIndex] = 1;
                }
            }
            return PostPhysics;
        }
    }

    namespace EntityScripts
    {
        CEntityScript* Attach(ECS::FRegistry& Registry, ECS::FEntity Entity, CClass* ScriptClass)
        {
            if (ScriptClass == nullptr || !ScriptClass->IsChildOf(CEntityScript::StaticClass()))
            {
                LOG_WARN("EntityScripts::Attach: '{}' is not a CEntityScript.",
                    ScriptClass ? ScriptClass->GetName().c_str() : "(null)");
                return nullptr;
            }

            // find, not get, since a bare registry in a test or tool has no world and its scripts have none.
            CWorld** WorldPtr = Registry.Ctx().Find<CWorld*>();
            if (WorldPtr != nullptr && !NetRealm::Allows(ScriptClass, *WorldPtr))
            {
                return nullptr;
            }

            CObject* Created = NewObject(ScriptClass, nullptr, NAME_None, FGuid::New(), OF_Transient);
            CEntityScript* Script = static_cast<CEntityScript*>(Created);
            if (Script == nullptr)
            {
                return nullptr;
            }

            Script->SetOwner(Entity, WorldPtr != nullptr ? *WorldPtr : nullptr);

            SEntityScriptComponent& Component = Registry.GetOrEmplace<SEntityScriptComponent>(Entity);
            Component.Scripts.push_back(Script);
            NoteStructureChange();

            // By the first tick every sibling script added the same frame exists, so OnReady can reference them.
            {
                LUMINA_PROFILE_SECTION_NAMED(Script->GetClass()->GetName().c_str());
                Script->OnAttach();
            }
            return Script;
        }

        void NoteStructureChange()
        {
            if (GInParallelScriptPass)
            {
                ReportParallelStructureChange();
            }
            BumpStructureEpoch();
        }

        void Tick(ECS::FRegistry& Registry, float DeltaTime, EScriptUpdatePhase Phase)
        {
            if (Phase == EScriptUpdatePhase::PostPhysics)
            {
                const FScriptPhaseCensus* Census = Registry.Ctx().Find<FScriptPhaseCensus>();
                if (Census != nullptr && Census->PostPhysicsScripts == 0)
                {
                    return;
                }
            }

            // Scripts build meshes one at a time, so their builds are held and run together when the tick ends.
            FMeshBuildBatchScope MeshBatch(Registry);

            // Attach and OnReady are phase-independent, so a PostPhysics script is built in the same pass.
            const bool bDrainLifecycle = Phase == EScriptUpdatePhase::PrePhysics;

            // A C++ subclass runs its own override and a C# one runs the generated shim, indistinguishably.
            CWorld** WorldPtr = Registry.Ctx().Find<CWorld*>();
            CWorld* World = WorldPtr != nullptr ? *WorldPtr : nullptr;

            FTickScratchGuard      ScratchGuard;
            FTickScratch&          Scratch       = ScratchGuard.Get();
            TVector<ECS::FEntity>& Entities      = Scratch.Entities;
            FScriptSnapshot&       Scripts       = Scratch.Scripts;
            TVector<void*>&        BatchHandles  = Scratch.BatchHandles;
            TVector<ECS::FEntity>& BatchEntities = Scratch.BatchEntities;
            TVector<CEntityScript*>& BatchScripts = Scratch.BatchScripts;

            SnapshotScriptedEntities(Registry, Entities);

            uint32 PostPhysicsScripts = 0;

            // The unbatched path, which re-checks attachment after every callback exactly as it always has.
            auto RunEntity = [&](ECS::FEntity Entity)
            {
                SnapshotScripts(Registry, Entity, Scripts);

                // Until a callback runs, the snapshot is exactly what the component holds, so there is nothing to re-check.
                bool bUserCodeRan = false;

                for (TStrongObjectPtr<CEntityScript>& Held : Scripts)
                {
                    CEntityScript* Script = Held.Get();
                    if (bUserCodeRan ? !IsStillAttached(Registry, Entity, Script) : Script == nullptr)
                    {
                        continue;
                    }

                    const EScriptUpdatePhase ScriptUpdatePhase = ScriptPhase(Script);
                    PostPhysicsScripts += (ScriptUpdatePhase == EScriptUpdatePhase::PostPhysics) ? 1u : 0u;

                    // The one place entity and registry are both in hand, and it runs before OnReady.
                    if (bDrainLifecycle && Script->GetOwningEntity() == ECS::NullEntity)
                    {
                        if (IsOutsideRealm(Script, World))
                        {
                            continue;
                        }
                        Script->SetOwner(Entity, World);
                        {
                            LUMINA_PROFILE_SECTION_NAMED(Script->GetClass()->GetName().c_str());
                            Script->OnAttach();
                        }
                        bUserCodeRan = true;

                        // OnAttach is user code; it may have detached this very script.
                        if (!IsStillAttached(Registry, Entity, Script))
                        {
                            continue;
                        }
                    }

                    if (Script->IsFaulted())
                    {
                        continue;
                    }

                    if (bDrainLifecycle && !Script->IsReady())
                    {
                        Script->MarkReady();
                        {
                            // Once per script, so naming it by class costs nothing per frame and splits the script system's time.
                            LUMINA_PROFILE_SECTION_NAMED(Script->GetClass()->GetName().c_str());
                            Script->OnReady();
                        }
                        bUserCodeRan = true;

                        if (!IsStillAttached(Registry, Entity, Script))
                        {
                            continue;
                        }
                    }

                    if (Script->ShouldTick() && ScriptUpdatePhase == Phase)
                    {
                        // Named by class, so a capture splits the script system's time by script and not just in total.
                        LUMINA_PROFILE_SECTION_NAMED(Script->GetClass()->GetName().c_str());
                        Script->OnUpdate(DeltaTime);
                        bUserCodeRan = true;
                    }
                }
            };

            // Queues the entity's updates when every script on it is settled and either C# or idle this phase.
            const CClass*      CachedClass    = nullptr;
            bool               bCachedManaged = false;
            EScriptUpdatePhase CachedPhase    = EScriptUpdatePhase::PrePhysics;
            auto TryQueueEntity = [&](ECS::FEntity Entity) -> bool
            {
                // Read in place rather than snapshotted, since nothing here runs script code that could change the list.
                if (Entity == ECS::NullEntity || !Registry.IsValid(Entity))
                {
                    return false;
                }
                const SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
                if (Component == nullptr)
                {
                    return false;
                }

                for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
                {
                    const CEntityScript* Script = Held.Get();
                    if (Script == nullptr)
                    {
                        return false;
                    }

                    // Attach and OnReady are owed whatever the script's phase, and only the checked path runs them.
                    if (bDrainLifecycle && (Script->GetOwningEntity() == ECS::NullEntity || (!Script->IsFaulted() && !Script->IsReady())))
                    {
                        return false;
                    }

                    // Phase and override are class data, so a run of one class resolves them once.
                    const CClass* Class = Script->GetClass();
                    if (Class != CachedClass)
                    {
                        CachedClass    = Class;
                        bCachedManaged = HasManagedUpdate(Class);
                        CachedPhase    = ScriptPhase(Script);
                    }
                    if (!Script->ShouldTick() || CachedPhase != Phase)
                    {
                        continue;
                    }
                    if (!bCachedManaged)
                    {
                        return false;
                    }
                }

                const size_t Start = BatchHandles.size();
                const uint32 EpochBefore = GStructureEpoch.load(std::memory_order_relaxed);
                uint32 PostPhysicsHere = 0;
                for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
                {
                    CEntityScript* Script = Held.Get();
                    const EScriptUpdatePhase ScriptUpdatePhase = Script->GetClass() == CachedClass ? CachedPhase : ScriptPhase(Script);
                    PostPhysicsHere += (ScriptUpdatePhase == EScriptUpdatePhase::PostPhysics) ? 1u : 0u;
                    if (!Script->ShouldTick() || ScriptUpdatePhase != Phase)
                    {
                        continue;
                    }

                    // Creating the managed instance runs its constructor, which could in principle touch this very list.
                    const uint32 Generation = ManagedInstances::GetHandleGeneration();
                    void* Handle = Script->CachedHandleGeneration == Generation ? Script->CachedManagedHandle : nullptr;
                    if (Handle == nullptr)
                    {
                        Handle = Scriptable::GetOrCreateInstance(Script);
                        Script->CachedManagedHandle    = Handle;
                        Script->CachedHandleGeneration = Generation;
                    }
                    if (Handle == nullptr || GStructureEpoch.load(std::memory_order_relaxed) != EpochBefore)
                    {
                        BatchHandles.resize(Start);
                        BatchEntities.resize(Start);
                        BatchScripts.resize(Start);
                        return false;
                    }
                    BatchHandles.push_back(Handle);
                    BatchEntities.push_back(Entity);
                    BatchScripts.push_back(Script);
                }
                PostPhysicsScripts += PostPhysicsHere;
                return true;
            };

            // The epoch the queued entries were validated under, so a change before or during dispatch is caught.
            uint32 QueuedEpoch = 0;

            // Dispatches the first FlushCount entries; a structural change sends everything still queued down the checked path.
            auto FlushBatch = [&](size_t FlushCount)
            {
                const size_t Total = BatchHandles.size();
                if (FlushCount == 0 || Total == 0)
                {
                    return;
                }

                size_t Ran = 0;
                auto* Dispatch = GDispatchUpdates.Get();
                if (Dispatch != nullptr && GStructureEpoch.load(std::memory_order_relaxed) == QueuedEpoch)
                {
                    const CClass* Class = BatchScripts.front()->GetClass();
                    LUMINA_PROFILE_SECTION_NAMED(Class->GetName().c_str());
                    Ran = (size_t)Dispatch(BatchHandles.data(), (int32)FlushCount, DeltaTime, reinterpret_cast<const uint32*>(&GStructureEpoch));
                }

                if (Ran == FlushCount && GStructureEpoch.load(std::memory_order_relaxed) == QueuedEpoch)
                {
                    BatchHandles.erase(BatchHandles.begin(), BatchHandles.begin() + (ptrdiff_t)FlushCount);
                    BatchEntities.erase(BatchEntities.begin(), BatchEntities.begin() + (ptrdiff_t)FlushCount);
                    BatchScripts.erase(BatchScripts.begin(), BatchScripts.begin() + (ptrdiff_t)FlushCount);
                    return;
                }

                // Something attached, detached, destroyed or faulted, so the rest goes the checked way.
                size_t Index = Ran;
                if (Ran > 0)
                {
                    const ECS::FEntity Interrupted = BatchEntities[Ran - 1];
                    for (; Index < Total && BatchEntities[Index] == Interrupted; ++Index)
                    {
                        CEntityScript* Script = BatchScripts[Index];
                        if (IsStillAttached(Registry, Interrupted, Script) && Script->ShouldTick() && ScriptPhase(Script) == Phase)
                        {
                            LUMINA_PROFILE_SECTION_NAMED(Script->GetClass()->GetName().c_str());
                            Script->OnUpdate(DeltaTime);
                        }
                    }
                }
                for (ECS::FEntity Previous = ECS::NullEntity; Index < Total; ++Index)
                {
                    if (BatchEntities[Index] != Previous)
                    {
                        Previous = BatchEntities[Index];
                        RunEntity(Previous);
                    }
                }

                BatchHandles.clear();
                BatchEntities.clear();
                BatchScripts.clear();
            };

            // Entities whose ticking scripts all promised to touch nothing else run first, spread across the workers.
            TVector<uint8>&          Claimed          = Scratch.Claimed;
            TVector<void*>&          ParallelHandles  = Scratch.ParallelHandles;
            TVector<CEntityScript*>& ParallelScripts  = Scratch.ParallelScripts;
            TVector<ECS::FEntity>&   ParallelEntities = Scratch.ParallelEntities;
            Claimed.assign(Entities.size(), 0);
            ParallelHandles.clear();
            ParallelScripts.clear();
            ParallelEntities.clear();

            const uint32 ClaimEpoch = GStructureEpoch.load(std::memory_order_relaxed);

            // Whatever a parallel run did not get to goes the checked serial way, finishing an interrupted entity first.
            auto FinishRemainder = [&](const TVector<ECS::FEntity>& RunEntities, const TVector<CEntityScript*>& RunScripts, uint32 Begin, uint32 Ran, uint32 End)
            {
                uint32 Index = Begin + Ran;
                if (Index >= End)
                {
                    return;
                }
                if (Ran > 0)
                {
                    const ECS::FEntity Interrupted = RunEntities[Index - 1];
                    for (; Index < End && RunEntities[Index] == Interrupted; ++Index)
                    {
                        CEntityScript* Script = RunScripts[Index];
                        if (IsStillAttached(Registry, Interrupted, Script) && Script->ShouldTick() && ScriptPhase(Script) == Phase)
                        {
                            LUMINA_PROFILE_SECTION_NAMED(Script->GetClass()->GetName().c_str());
                            Script->OnUpdate(DeltaTime);
                        }
                    }
                }
                for (ECS::FEntity Previous = ECS::NullEntity; Index < End; ++Index)
                {
                    if (RunEntities[Index] != Previous)
                    {
                        Previous = RunEntities[Index];
                        RunEntity(Previous);
                    }
                }
            };

            const uint32 NumThreads = (GTaskSystem != nullptr) ? GTaskSystem->GetNumTaskThreads() : 0u;
            if ((uint32)Entities.size() >= kParallelClaimMinEntities && NumThreads > 1)
            {
                LUMINA_PROFILE_SECTION("Parallel Entity Scripts");

                // Each chunk claims its own entities and runs them at once, which no other chunk reads.
                const uint32 NumEntities = (uint32)Entities.size();
                const uint32 NumChunks   = (NumEntities + kParallelClaimChunk - 1) / kParallelClaimChunk;
                if ((uint32)Scratch.ClaimChunks.size() < NumChunks)
                {
                    Scratch.ClaimChunks.resize(NumChunks);
                }

                Task::ParallelFor(NumChunks, [&](uint32 Chunk)
                {
                    FParallelScriptPassScope PassScope;
                    FClaimChunk& Out = Scratch.ClaimChunks[Chunk];
                    Out.Handles.clear();
                    Out.Scripts.clear();
                    Out.Entities.clear();
                    Out.PostPhysics = 0;
                    Out.Ran         = 0;

                    FClassFacts  Facts;
                    const uint32 End = Math::Min(NumEntities, (Chunk + 1) * kParallelClaimChunk);
                    for (uint32 EntityIndex = Chunk * kParallelClaimChunk; EntityIndex < End; ++EntityIndex)
                    {
                        // An entity still owed a managed instance runs on the serial path, which may create one.
                        const EClaimResult Result = ClaimEntity(Registry, Entities[EntityIndex], Phase, bDrainLifecycle, false,
                                                                Facts, Out.Handles, Out.Scripts, Out.Entities, Out.PostPhysics);
                        Claimed[EntityIndex] = Result == EClaimResult::Claimed ? 1u : 0u;
                    }

                    Out.Ran = RunClaimedSpan(Out.Handles.data(), Out.Scripts.data(), (uint32)Out.Scripts.size(), DeltaTime, ClaimEpoch);
                }, 1);

                for (uint32 Chunk = 0; Chunk < NumChunks; ++Chunk)
                {
                    const FClaimChunk& In = Scratch.ClaimChunks[Chunk];
                    PostPhysicsScripts += In.PostPhysics;
                    FinishRemainder(In.Entities, In.Scripts, 0u, In.Ran, (uint32)In.Scripts.size());
                }
            }
            else
            {
                uint32 ClaimedPostPhysics = ClaimEntitiesSerial(Registry, Entities, Phase, bDrainLifecycle, Scratch);

                // A managed constructor run while claiming may have changed things, which leaves every claim suspect.
                if (GStructureEpoch.load(std::memory_order_relaxed) != ClaimEpoch)
                {
                    Claimed.assign(Entities.size(), 0);
                    ParallelHandles.clear();
                    ParallelScripts.clear();
                    ParallelEntities.clear();
                    ClaimedPostPhysics = 0;
                }
                PostPhysicsScripts += ClaimedPostPhysics;

                if (!ParallelScripts.empty())
                {
                    LUMINA_PROFILE_SECTION("Parallel Entity Scripts");

                    TVector<uint32>& ChunkStarts = Scratch.ChunkStarts;
                    TVector<uint32>& ChunkRan    = Scratch.ChunkRan;
                    BuildEntityChunks(ParallelEntities, ChunkStarts);
                    const uint32 NumChunks = (uint32)ChunkStarts.size() - 1;
                    ChunkRan.assign(NumChunks, 0);

                    RunChunks((uint32)ParallelScripts.size(), NumChunks, [&](uint32 Chunk)
                    {
                        FParallelScriptPassScope PassScope;
                        const uint32 Begin = ChunkStarts[Chunk];
                        ChunkRan[Chunk] = RunClaimedSpan(ParallelHandles.data() + Begin, ParallelScripts.data() + Begin,
                                                         ChunkStarts[Chunk + 1] - Begin, DeltaTime, ClaimEpoch);
                    });

                    for (uint32 Chunk = 0; Chunk < NumChunks; ++Chunk)
                    {
                        FinishRemainder(ParallelEntities, ParallelScripts, ChunkStarts[Chunk], ChunkRan[Chunk], ChunkStarts[Chunk + 1]);
                    }
                }
            }

            const bool bProfilerAttached = TracyIsConnected;
            for (size_t EntityIndex = 0; EntityIndex < Entities.size(); ++EntityIndex)
            {
                if (Claimed[EntityIndex] != 0)
                {
                    continue;
                }
                const ECS::FEntity Entity = Entities[EntityIndex];
                const size_t QueuedBefore = BatchHandles.size();
                if (QueuedBefore == 0)
                {
                    QueuedEpoch = GStructureEpoch.load(std::memory_order_relaxed);
                }

                if (TryQueueEntity(Entity))
                {
                    // A connected profiler still splits the time by class, so a run never mixes two.
                    if (bProfilerAttached && QueuedBefore > 0 && BatchHandles.size() > QueuedBefore
                        && BatchScripts[QueuedBefore]->GetClass() != BatchScripts.front()->GetClass())
                    {
                        FlushBatch(QueuedBefore);
                    }
                    continue;
                }

                FlushBatch(BatchHandles.size());
                RunEntity(Entity);
            }
            FlushBatch(BatchHandles.size());

            // Parked storage would otherwise hold the last entity's scripts alive until the next tick.
            Scripts.clear();

            if (bDrainLifecycle)
            {
                FScriptPhaseCensus* Census = Registry.Ctx().Find<FScriptPhaseCensus>();
                if (Census == nullptr)
                {
                    Census = &Registry.Ctx().Emplace<FScriptPhaseCensus>();
                }
                Census->PostPhysicsScripts = PostPhysicsScripts;
            }
        }

        void TickFixed(ECS::FRegistry& Registry, float FixedDeltaTime)
        {
            FTickScratchGuard      ScratchGuard;
            FTickScratch&          Scratch  = ScratchGuard.Get();
            TVector<ECS::FEntity>& Entities = Scratch.Entities;
            FScriptSnapshot&       Scripts  = Scratch.Scripts;

            SnapshotScriptedEntities(Registry, Entities);

            auto RunEntity = [&](ECS::FEntity Entity)
            {
                SnapshotScripts(Registry, Entity, Scripts);
                for (TStrongObjectPtr<CEntityScript>& Held : Scripts)
                {
                    // Fixed update only runs on a readied script, so none sees a fixed step before its OnReady.
                    CEntityScript* Script = Held.Get();
                    if (Script != nullptr && Script->ShouldTick() && IsStillAttached(Registry, Entity, Script))
                    {
                        Script->OnFixedUpdate(FixedDeltaTime);
                    }
                }
            };

            // The same split as Tick, without the managed batching, since a fixed step crosses per script either way.
            TVector<uint8>&          Claimed          = Scratch.Claimed;
            TVector<CEntityScript*>& ParallelScripts  = Scratch.ParallelScripts;
            TVector<ECS::FEntity>&   ParallelEntities = Scratch.ParallelEntities;
            Claimed.assign(Entities.size(), 0);
            ParallelScripts.clear();
            ParallelEntities.clear();

            const uint32 ClaimEpoch = GStructureEpoch.load(std::memory_order_relaxed);
            {
                const CClass* ClassSeen     = nullptr;
                bool          bSeenParallel = false;

                for (size_t EntityIndex = 0; EntityIndex < Entities.size(); ++EntityIndex)
                {
                    const ECS::FEntity Entity = Entities[EntityIndex];
                    const SEntityScriptComponent* Component = (Entity != ECS::NullEntity && Registry.IsValid(Entity))
                        ? Registry.TryGet<SEntityScriptComponent>(Entity) : nullptr;
                    if (Component == nullptr)
                    {
                        continue;
                    }

                    bool bEligible   = true;
                    bool bAnyTicking = false;
                    for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
                    {
                        const CEntityScript* Script = Held.Get();
                        if (Script == nullptr)
                        {
                            bEligible = false;
                            break;
                        }
                        if (!Script->ShouldTick())
                        {
                            continue;
                        }
                        if (Script->GetClass() != ClassSeen)
                        {
                            ClassSeen     = Script->GetClass();
                            bSeenParallel = IsParallelUpdate(ClassSeen);
                        }
                        if (!bSeenParallel)
                        {
                            bEligible = false;
                            break;
                        }
                        bAnyTicking = true;
                    }
                    if (!bEligible || !bAnyTicking)
                    {
                        continue;
                    }

                    for (const TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
                    {
                        if (Held->ShouldTick())
                        {
                            ParallelScripts.push_back(Held.Get());
                            ParallelEntities.push_back(Entity);
                        }
                    }
                    Claimed[EntityIndex] = 1;
                }
            }

            if (!ParallelScripts.empty())
            {
                LUMINA_PROFILE_SECTION("Parallel Entity Scripts Fixed");

                TVector<uint32>& ChunkStarts = Scratch.ChunkStarts;
                TVector<uint32>& ChunkRan    = Scratch.ChunkRan;
                BuildEntityChunks(ParallelEntities, ChunkStarts);
                const uint32 NumChunks = (uint32)ChunkStarts.size() - 1;
                ChunkRan.assign(NumChunks, 0);

                RunChunks((uint32)ParallelScripts.size(), NumChunks, [&](uint32 Chunk)
                {
                    FParallelScriptPassScope PassScope;
                    const uint32 Begin = ChunkStarts[Chunk];
                    const uint32 End   = ChunkStarts[Chunk + 1];
                    uint32 Index = Begin;
                    for (; Index < End && GStructureEpoch.load(std::memory_order_relaxed) == ClaimEpoch; ++Index)
                    {
                        ParallelScripts[Index]->OnFixedUpdate(FixedDeltaTime);
                    }
                    ChunkRan[Chunk] = Index - Begin;
                });

                for (uint32 Chunk = 0; Chunk < NumChunks; ++Chunk)
                {
                    const uint32 End = ChunkStarts[Chunk + 1];
                    uint32 Index = ChunkStarts[Chunk] + ChunkRan[Chunk];
                    if (Index >= End)
                    {
                        continue;
                    }
                    if (ChunkRan[Chunk] > 0)
                    {
                        const ECS::FEntity Interrupted = ParallelEntities[Index - 1];
                        for (; Index < End && ParallelEntities[Index] == Interrupted; ++Index)
                        {
                            CEntityScript* Script = ParallelScripts[Index];
                            if (IsStillAttached(Registry, Interrupted, Script) && Script->ShouldTick())
                            {
                                Script->OnFixedUpdate(FixedDeltaTime);
                            }
                        }
                    }
                    for (ECS::FEntity Previous = ECS::NullEntity; Index < End; ++Index)
                    {
                        if (ParallelEntities[Index] != Previous)
                        {
                            Previous = ParallelEntities[Index];
                            RunEntity(Previous);
                        }
                    }
                }
            }

            for (size_t EntityIndex = 0; EntityIndex < Entities.size(); ++EntityIndex)
            {
                if (Claimed[EntityIndex] == 0)
                {
                    RunEntity(Entities[EntityIndex]);
                }
            }

            // Parked storage would otherwise hold the last entity's scripts alive until the next tick.
            Scripts.clear();
        }

        CEntityScript* Find(ECS::FRegistry& Registry, ECS::FEntity Entity, const CClass* ScriptClass)
        {
            if (ScriptClass == nullptr)
            {
                return nullptr;
            }
            SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return nullptr;
            }
            for (TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (Script != nullptr && Script->GetClass() != nullptr && Script->GetClass()->IsChildOf(ScriptClass))
                {
                    return Script;
                }
            }
            return nullptr;
        }

        void FindAll(ECS::FRegistry& Registry, ECS::FEntity Entity, const CClass* ScriptClass,
            TVector<CEntityScript*>& Out)
        {
            if (ScriptClass == nullptr)
            {
                return;
            }
            SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return;
            }
            for (TStrongObjectPtr<CEntityScript>& Held : Component->Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (Script != nullptr && Script->GetClass() != nullptr && Script->GetClass()->IsChildOf(ScriptClass))
                {
                    Out.push_back(Script);
                }
            }
        }

        bool Remove(ECS::FRegistry& Registry, ECS::FEntity Entity, CEntityScript* Script)
        {
            if (Script == nullptr || !IsStillAttached(Registry, Entity, Script))
            {
                return false;
            }
            
            TStrongObjectPtr<CEntityScript> Pinned(Script);
            NoteStructureChange();

            if (Script->IsAttached())
            {
                Script->OnDetach();
            }
            
            SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
            if (Component == nullptr)
            {
                return true;   // OnDetach tore the component down; the script is gone either way
            }

            for (auto It = Component->Scripts.begin(); It != Component->Scripts.end(); ++It)
            {
                if (It->Get() == Script)
                {
                    Component->Scripts.erase(It);
                    return true;
                }
            }
            return true;
        }

        void DispatchInput(ECS::FRegistry& Registry, ECS::FEntity Entity, const SInputEvent& Event)
        {
            FScriptSnapshot Scripts;
            SnapshotScripts(Registry, Entity, Scripts);

            for (TStrongObjectPtr<CEntityScript>& Held : Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (IsStillAttached(Registry, Entity, Script) && !Script->IsFaulted())
                {
                    Script->OnInput(Event);
                }
            }
        }

        void DispatchActions(ECS::FRegistry& Registry, ECS::FEntity Entity, const FInputActionState* States,
            int32 Count, TSpan<const int32> ChangedActionIndices)
        {
            FScriptSnapshot Scripts;
            SnapshotScripts(Registry, Entity, Scripts);

            for (TStrongObjectPtr<CEntityScript>& Held : Scripts)
            {
                CEntityScript* Script = Held.Get();
                if (!IsStillAttached(Registry, Entity, Script) || Script->IsFaulted())
                {
                    continue;
                }

                const TVector<SInputAction>& Actions = FInputActionMap::Get().GetAllActions();
                for (int32 i : ChangedActionIndices)
                {
                    if (i < Count && i < (int32)Actions.size())
                    {
                        Script->OnAction(Actions[i].Name, States[i]);
                    }
                }
            }
        }



        void DetachAll(ECS::FRegistry& Registry, ECS::FEntity Entity)
        {
            FScriptSnapshot Scripts;
            SnapshotScripts(Registry, Entity, Scripts);
            if (Scripts.empty())
            {
                return;
            }
            NoteStructureChange();

            for (TStrongObjectPtr<CEntityScript>& Held : Scripts)
            {
                // For a C# script the call would mint a managed instance purely to tear it down again.
                if (CEntityScript* Script = Held.Get(); Script != nullptr && Script->IsAttached())
                {
                    Script->OnDetach();
                }
            }
            
            if (SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity))
            {
                Component->Scripts.clear();
            }
        }

        int32 ResolvePendingScripts(ECS::FRegistry& Registry)
        {
            TVector<ECS::FEntity> Entities;
            auto View = Registry.View<SEntityScriptComponent>();
            Entities.reserve(View.Num());
            for (ECS::FEntity Entity : View)
            {
                Entities.push_back(Entity);
            }

            int32 Restored = 0;
            for (ECS::FEntity Entity : Entities)
            {
                SEntityScriptComponent* Component = Registry.TryGet<SEntityScriptComponent>(Entity);
                if (Component == nullptr || Component->Pending.empty())
                {
                    continue;
                }

                TVector<FPendingScript> StillPending;
                TVector<FPendingScript> Holding = Move(Component->Pending);
                Component->Pending.clear();

                for (FPendingScript& Held : Holding)
                {
                    CClass* ScriptClass = FScriptableRegistry::ResolveClass(Held.ClassName);
                    if (ScriptClass == nullptr || !ScriptClass->IsChildOf(CEntityScript::StaticClass()))
                    {
                        StillPending.push_back(Move(Held));
                        continue;
                    }

                    CEntityScript* Script = static_cast<CEntityScript*>(
                        NewObject(ScriptClass, nullptr, NAME_None, FGuid::New(), OF_Transient));
                    if (Script == nullptr)
                    {
                        StillPending.push_back(Move(Held));
                        continue;
                    }

                    if (!Held.Bytes.empty() && Held.MakeReader)
                    {
                        TUniquePtr<FArchive> Reader = Held.MakeReader(Held.Bytes);
                        ScriptClass->SerializeTaggedProperties(*Reader, Script);
                    }
                    else if (!Held.Bytes.empty())
                    {
                        FMemoryReader Reader(Held.Bytes);
                        Reader.SetFileVersion(Held.FileVersion);
                        ScriptClass->SerializeTaggedProperties(Reader, Script);
                    }

                    Component->Scripts.push_back(Script);
                    NoteStructureChange();
                    ++Restored;

                    LOG_INFO("SEntityScriptComponent: restored held script '{}' now that its class is loaded.",
                        Held.ClassName.c_str());
                }

                Component->Pending = Move(StillPending);
            }
            return Restored;
        }

        void NotifyScriptsReloaded(EScriptReloadReason Reason, int32 Generation)
        {
            if (GWorldManager == nullptr)
            {
                return;
            }

            GWorldManager->ForEachWorld([&](CWorld& World)
            {
                ECS::FRegistry& Registry = ECS::GetWorldRegistry(World);

                // Anything held back by a failed or late load comes in first, so it sees this reload too.
                ResolvePendingScripts(Registry);

                TVector<ECS::FEntity> Entities;
                auto View = Registry.View<SEntityScriptComponent>();
                Entities.reserve(View.Num());
                for (ECS::FEntity Entity : View)
                {
                    Entities.push_back(Entity);
                }

                SScriptReloadContext Context;
                Context.Reason     = Reason;
                Context.WorldType  = World.GetWorldType();
                Context.Generation = Generation;

                for (ECS::FEntity Entity : Entities)
                {
                    FScriptSnapshot Scripts;
                    SnapshotScripts(Registry, Entity, Scripts);

                    Context.Entity = Entity;
                    for (TStrongObjectPtr<CEntityScript>& Held : Scripts)
                    {
                        CEntityScript* Script = Held.Get();

                        // A C++ script's instance never went anywhere, so it has nothing to rebuild.
                        if (Script == nullptr || !Script->IsAttached() || ToScriptClass(Script->GetClass()) == nullptr)
                        {
                            continue;
                        }
                        if (!IsStillAttached(Registry, Entity, Script))
                        {
                            continue;
                        }
                        Script->OnReloaded(Context);
                    }
                }
            });
        }

        void DetachAllInRegistry(ECS::FRegistry& Registry)
        {
            // Disabled entities are included, since a disabled script still ran OnAttach and is owed its OnDetach.
            TVector<ECS::FEntity> Entities;
            auto View = Registry.View<SEntityScriptComponent>();
            Entities.reserve(View.Num());
            for (ECS::FEntity Entity : View)
            {
                Entities.push_back(Entity);
            }

            for (ECS::FEntity Entity : Entities)
            {
                if (Registry.IsValid(Entity))
                {
                    DetachAll(Registry, Entity);
                }
            }
        }
    }
}
