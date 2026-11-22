#include "RuntimePCH.h"
#include "ScriptNet.h"

#include "Core/Object/Class.h"
#include "Core/Object/ScriptClass.h"
#include "Core/Reflection/PropertySnapshot.h"
#include "Core/Reflection/Type/Function.h"
#include "Core/Reflection/Type/LuminaTypes.h"
#include "Core/Serialization/NetArchive.h"
#include "DotNet/DotNetHost.h"
#include "EntityScript.h"
#include "Networking/INetworkRuntime.h"
#include "ScriptableObject.h"
#include "World/ECS/EventQueue.h"
#include "World/ECS/Registry.h"
#include "World/World.h"

namespace Lumina
{
    namespace
    {
        thread_local bool   GArrivingRpc = false;
        thread_local int32  GRpcDepth    = 0;
        thread_local uint32 GRpcCaller   = 0;

        DotNet::TManagedExport<void (*)(void*, const char*, int32, int32)> GDispatchSync("NetDispatchSync");

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

            LE_NO_COPYMOVE(FReceiveScope);

            uint32 OuterCaller;
        };

        // Null for a C++ script, which has no managed side to notify.
        void* ManagedHandleOf(CEntityScript& Script)
        {
            return ToScriptClass(Script.GetClass()) != nullptr ? Scriptable::GetOrCreateInstance(&Script) : nullptr;
        }

        bool IsHostWorld(const CWorld* World)
        {
            return World == nullptr || World->GetNetMode() != ENetMode::Client;
        }

        // True for a standalone world and an unnetworked entity, which nothing else could control.
        bool IsOwnedHere(const CWorld* World, ECS::FEntity Entity)
        {
            INetworkRuntime* Net = GetNetworkRuntime();
            if (Net == nullptr || World == nullptr || !Net->IsEntityNetworked(World, Entity))
            {
                return true;
            }
            return Net->GetEntityOwner(World, Entity) == Net->GetLocalConnectionId(World);
        }

        void MarkDirty(const CEntityScript& Script)
        {
            if (INetworkRuntime* Net = GetNetworkRuntime(); Net != nullptr && Script.GetWorld() != nullptr && FEntityScriptAccess::IsAttached(Script))
            {
                Net->MarkEntityDirty(Script.GetWorld(), Script.GetOwningEntity());
            }
        }

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
            uint32 Hash = HashInto(2166136261u, *Class->GetName());
            Hash = HashInto(Hash, ".");
            return HashInto(Hash, *Function.GetFunctionName());
        }

        // The id an owner's write to a FromOwner field travels under, or the one the host's correction comes back under.
        uint32 SyncFieldId(const CClass* Class, const FProperty* Field, bool bCorrection)
        {
            uint32 Hash = HashInto(2166136261u, *Class->GetName());
            Hash = HashInto(Hash, ".");
            Hash = HashInto(Hash, *Field->GetPropertyName());
            return HashInto(Hash, bCorrection ? "#correct" : "#sync");
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

        // Whether the field is PROPERTY(Sync = FromOwner), the only kind an owner may write.
        bool IsSyncFromOwner(const FProperty* Field)
        {
            const FCStringView Sync = Field->GetMetadata("Sync");
            return FStringView(Sync.GetData(), Sync.Len()).Find("FromOwner") != Constants::kIndexNone;
        }

        struct FRpcRoute
        {
            const FFunction* Function = nullptr;
            FProperty* SyncField = nullptr;
            bool bCorrection = false;
        };

        FRpcRoute FindRpcRoute(const CEntityScript& Script, uint32 RpcId)
        {
            const CClass* Class = Script.GetClass();
            for (const FFunction* Function : Class->GetFunctions())
            {
                if (Function->IsRpc() && RpcIdOf(Class, *Function) == RpcId)
                {
                    return { Function, nullptr, false };
                }
            }

            TVector<FProperty*> Fields;
            Class->GetNetReplicatedProperties(Fields);
            for (FProperty* Field : Fields)
            {
                if (!IsSyncFromOwner(Field))
                {
                    continue;
                }
                if (RpcId == SyncFieldId(Class, Field, true))
                {
                    return { nullptr, Field, true };
                }
                if (RpcId == SyncFieldId(Class, Field, false))
                {
                    return { nullptr, Field, false };
                }
            }

            return {};
        }

        // One value of a property outside any container, for a write that has not been accepted yet.
        struct FValueBuffer
        {
            explicit FValueBuffer(const FProperty& InField)
                : Field(InField)
                , Storage(Memory::MallocOrThrow(InField.GetElementSize(), FFunction::kFrameAlignment))
            {
                Memory::Memzero(Storage, Field.GetElementSize());
                Field.ConstructValue(Storage);
            }

            ~FValueBuffer()
            {
                Field.DestructValue(Storage);
                Memory::Free(Storage);
            }

            LE_NO_COPYMOVE(FValueBuffer);

            void* Data() const { return Storage; }

            const FProperty& Field;
            void* Storage;
        };

        // Only entity ids are bound, so names, objects and assets travel inline and a relayed call reads the same on every peer.
        void BindCallArchive(FNetArchive& Archive, const CWorld* World)
        {
            if (INetworkRuntime* Net = GetNetworkRuntime())
            {
                Net->BindEntityIds(World, Archive);
            }
        }

        const FFunction* FindNamedFunction(const CEntityScript& Script, const FProperty* Field, FStringView Key)
        {
            const FCStringView Name = Field->GetMetadata(Key);
            if (Name.IsEmpty())
            {
                return nullptr;
            }
            const FFunction* Function = Script.GetClass()->FindFunction(FName(*FString(Name.GetData(), Name.Len())));
            if (Function == nullptr)
            {
                LOG_WARN("{}.{} names {} function '{}', which is not a reflected function of the script.",
                    *Script.GetClass()->GetName(), *Field->GetPropertyName(), *FString(Key.GetData(), Key.Len()),
                    *FString(Name.GetData(), Name.Len()));
            }
            return Function;
        }

        // Where a call to Function goes from here. Consumes the mark a received call leaves, so the arriving call runs locally.
        ERpcRoute PlanRpc(const CEntityScript& Script, const FFunction& Function)
        {
            // The mark belongs to exactly one call, so an RPC made from inside a received one still routes.
            if (GArrivingRpc)
            {
                GArrivingRpc = false;
                return ERpcRoute::RunHere;
            }

            const CWorld* World = Script.GetWorld();
            INetworkRuntime* Net = GetNetworkRuntime();
            if (Net == nullptr || World == nullptr || World->GetNetMode() == ENetMode::Standalone)
            {
                return ERpcRoute::RunHere;
            }

            switch (TargetOf(Function))
            {
                case ERpcTarget::Host:
                    return IsHostWorld(World) ? ERpcRoute::RunHere : ERpcRoute::SendOnly;

                case ERpcTarget::Owner:
                    return Net->GetEntityOwner(World, Script.GetOwningEntity()) == Net->GetLocalConnectionId(World)
                        ? ERpcRoute::RunHere : ERpcRoute::SendOnly;

                default:
                    return ERpcRoute::SendAndRunHere;
            }
        }

        // Writes each argument through its parameter's FProperty::NetSerialize and sends the call, Values in parameter order.
        void SendRpc(CEntityScript& Script, const FFunction& Function, const void* const* Values)
        {
            INetworkRuntime* Net = GetNetworkRuntime();
            if (Net == nullptr || Script.GetWorld() == nullptr)
            {
                return;
            }

            TVector<uint8> Payload;
            FNetArchive Writer(Payload);
            BindCallArchive(Writer, Script.GetWorld());
            const TSpan<FProperty* const> Arguments = Function.GetArguments();
            for (int32 Index = 0; Index < Arguments.Num(); ++Index)
            {
                Arguments[Index]->NetSerialize(Writer, const_cast<void*>(Values[Index]));
            }
            Writer.AlignToByte();

            const bool bUnreliable = EnumHasAnyFlags(Function.GetFunctionFlags(), EFunctionFlags::NetUnreliable);
            const uint8 Flags = static_cast<uint8>(bUnreliable ? ENetFlags::Unreliable : ENetFlags::None);
            Net->SendScriptRpc(&Script, RpcIdOf(Script.GetClass(), Function), TargetOf(Function), Flags, Payload.GetData(), static_cast<uint32>(Payload.Num()));
        }

        // The same, reading the arguments out of a call frame laid out for Function.
        void SendRpcFrame(CEntityScript& Script, const FFunction& Function, const void* Frame)
        {
            const TSpan<FProperty* const> Arguments = Function.GetArguments();
            TVector<const void*> Values(Arguments.Num());
            for (int32 Index = 0; Index < Arguments.Num(); ++Index)
            {
                Values[Index] = Arguments[Index]->GetValuePtr<void>(Frame);
            }
            SendRpc(Script, Function, Values.GetData());
        }

        // Sends the field's current value to the host as an owner's write, or back to the owner as a correction.
        void SendSync(CEntityScript& Script, const FProperty* Field, bool bCorrection)
        {
            INetworkRuntime* Net = GetNetworkRuntime();
            if (Net == nullptr || Script.GetWorld() == nullptr)
            {
                return;
            }
            TVector<uint8> Payload;
            FNetArchive Writer(Payload);
            BindCallArchive(Writer, Script.GetWorld());
            const_cast<FProperty*>(Field)->NetSerialize(Writer, Field->GetValuePtr<void>(&Script));
            Writer.AlignToByte();
            Net->SendScriptRpc(&Script, SyncFieldId(Script.GetClass(), Field, bCorrection), bCorrection ? ERpcTarget::Owner : ERpcTarget::Host,
                static_cast<uint8>(ENetFlags::None), Payload.GetData(), static_cast<uint32>(Payload.Num()));
        }

        // Runs the field's PROPERTY(Change = ...) function, if it names one, with OldValue and the current value.
        void NotifySyncChanged(CEntityScript& Script, const FProperty* Field, const void* OldValue)
        {
            const FFunction* Function = FindNamedFunction(Script, Field, "Change");
            if (Function == nullptr)
            {
                return;
            }

            const TSpan<FProperty* const> Arguments = Function->GetArguments();
            const bool bCallable = Arguments.IsEmpty()
                || (Arguments.Num() == 2 && Arguments[0]->HasSameValueType(Field) && Arguments[1]->HasSameValueType(Field));
            if (!bCallable)
            {
                LOG_WARN("{}::{} is a change handler, so it takes nothing or (Old, New) of {}'s type.", *Script.GetClass()->GetName(),
                    *Function->GetFunctionName(), *Field->GetPropertyName());
                return;
            }

            FFunctionFrame Frame(*Function);
            if (Arguments.Num() == 2)
            {
                Arguments[0]->CopyCompleteValue(Arguments[0]->GetValuePtr<void>(Frame.GetMemory()), OldValue);
                Arguments[1]->CopyCompleteValue(Arguments[1]->GetValuePtr<void>(Frame.GetMemory()), Field->GetValuePtr<void>(&Script));
            }
            Frame.Invoke(&Script);
        }

        // Whether the field's PROPERTY(Validate = ...) function accepts Proposed. A field without one accepts anything.
        bool ValidateSync(CEntityScript& Script, const FProperty* Field, const void* Proposed)
        {
            const FFunction* Function = FindNamedFunction(Script, Field, "Validate");
            if (Function == nullptr)
            {
                return true;
            }

            const TSpan<FProperty* const> Arguments = Function->GetArguments();
            const FProperty* Return = Function->GetReturnParam();
            if (Arguments.Num() != 1 || !Arguments[0]->HasSameValueType(Field) || Return == nullptr || Return->GetType() != EPropertyTypeFlags::Bool)
            {
                LOG_WARN("{}::{} validates a Sync field, so it takes the proposed value and returns bool.", *Script.GetClass()->GetName(), *Function->GetFunctionName());
                return true;
            }

            FFunctionFrame Frame(*Function);
            Arguments[0]->CopyCompleteValue(Arguments[0]->GetValuePtr<void>(Frame.GetMemory()), Proposed);
            Frame.Invoke(&Script);
            return *Return->GetValuePtr<bool>(Frame.GetMemory());
        }

        // Snaps a numeric field to its PROPERTY(Quantize = ...) step in place.
        void QuantizeSync(CEntityScript& Script, const FProperty* Field)
        {
            const double Step = Field->GetMetadataNumber("Quantize").Get(0.0);
            if (Step <= 0.0)
            {
                return;
            }

            void* Value = Field->GetValuePtr<void>(&Script);
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

        // The other half of BeginSyncArrival, run at the sync point once the new values are in place.
        void FinishSyncArrival(ECS::FRegistry& Registry, ECS::FEntity Entity, CEntityScript& Script, const char* Names, uint32 NamesSize)
        {
            if (void* Handle = ManagedHandleOf(Script))
            {
                if (auto* Fn = GDispatchSync.Get())
                {
                    Fn(Handle, Names, static_cast<int32>(NamesSize), 1);
                }
                return;
            }

            FScriptSyncState* State = Registry.TryGet<FScriptSyncState>(Entity);
            bool bCaptured = false;
            FPropertySnapshot* Snapshot = State != nullptr ? State->FindOrCapture(Script, bCaptured).Snapshot.Get() : nullptr;
            if (Snapshot == nullptr || bCaptured)
            {
                return;
            }

            for (uint32 Start = 0; Start < NamesSize;)
            {
                const char* Name = Names + Start;
                Start += static_cast<uint32>(strlen(Name) + 1);
                for (int32 Index = 0; Index < Snapshot->Num(); ++Index)
                {
                    FProperty* Field = Snapshot->Field(Index);
                    if (strcmp(*Field->GetPropertyName(), Name) != 0)
                    {
                        continue;
                    }
                    if (!Snapshot->Matches(Index, &Script))
                    {
                        NotifySyncChanged(Script, Field, Snapshot->Slot(Index));
                        Snapshot->Store(Index, &Script);
                    }
                    break;
                }
            }
        }

        // An owner's proposal on the host, or the host's correction on the owner, applied at the sync point it arrived for.
        void ReceiveSync(CEntityScript& Script, FProperty* Field, bool bCorrection, uint32 CallerId, const uint8* Payload, uint32 PayloadSize)
        {
            CWorld* World = Script.GetWorld();
            INetworkRuntime* Net = GetNetworkRuntime();
            if (Net == nullptr || World == nullptr)
            {
                return;
            }

            // A proposal is the host's to judge and comes only from the owner. A correction is the owner's to take.
            if (bCorrection == IsHostWorld(World))
            {
                return;
            }
            if (!bCorrection && CallerId != 0 && Net->GetEntityOwner(World, Script.GetOwningEntity()) != CallerId)
            {
                return;
            }

            FValueBuffer Value(*Field);
            FNetArchive Reader(Payload, PayloadSize);
            BindCallArchive(Reader, World);
            Field->NetSerialize(Reader, Value.Data());
            if (Reader.HasError())
            {
                return;
            }

            FReceiveScope Scope(CallerId);
            if (!bCorrection && !ValidateSync(Script, Field, Value.Data()))
            {
                SendSync(Script, Field, true);
                return;
            }
            if (Field->Identical(Field->GetValuePtr<void>(&Script), Value.Data()))
            {
                return;
            }

            const char* Name = *Field->GetPropertyName();
            const uint32 NameSize = static_cast<uint32>(strlen(Name) + 1);
            ECS::FRegistry& Registry = World->GetEntityRegistry();
            ScriptNet::BeginSyncArrival(Registry, Script.GetOwningEntity(), Script, Name, NameSize);
            Field->CopyCompleteValue(Field->GetValuePtr<void>(&Script), Value.Data());
            FinishSyncArrival(Registry, Script.GetOwningEntity(), Script, Name, NameSize);
            if (!bCorrection)
            {
                MarkDirty(Script);
            }
        }

        void ReceiveRpc(CEntityScript& Script, uint32 RpcId, uint32 CallerId, const uint8* Payload, uint32 PayloadSize)
        {
            const FRpcRoute Route = FindRpcRoute(Script, RpcId);
            if (Route.SyncField != nullptr)
            {
                ReceiveSync(Script, Route.SyncField, Route.bCorrection, CallerId, Payload, PayloadSize);
                return;
            }
            if (Route.Function == nullptr)
            {
                LOG_WARN("[Net] {} has no RPC or Sync field with id {}; the peers are running different builds.", *Script.GetClass()->GetName(), RpcId);
                return;
            }

            const FFunction* Function = Route.Function;
            INetworkRuntime* Net = GetNetworkRuntime();

            // Only the entity's owner and the host may call an RPC unless it opts into AnyCaller, so a client cannot drive what it does not own.
            if (!EnumHasAnyFlags(Function->GetFunctionFlags(), EFunctionFlags::NetAnyCaller) && CallerId != 0
                && (Net == nullptr || Net->GetEntityOwner(Script.GetWorld(), Script.GetOwningEntity()) != CallerId))
            {
                LOG_WARN_ONCE("[Net] {}::{} refused a call from connection {}, which does not own the entity. Mark it AnyCaller if every client may call it.",
                              *Script.GetClass()->GetName(), *Function->GetFunctionName(), CallerId);
                return;
            }

            FFunctionFrame Frame(*Function);
            FNetArchive Reader(Payload, PayloadSize);
            BindCallArchive(Reader, Script.GetWorld());
            for (FProperty* Argument : Function->GetArguments())
            {
                Argument->NetSerialize(Reader, Argument->GetValuePtr<void>(Frame.GetMemory()));
            }
            if (Reader.HasError())
            {
                LOG_WARN("[Net] RPC {}::{} arrived truncated.", *Script.GetClass()->GetName(), *Function->GetFunctionName());
                return;
            }

            FReceiveScope Scope(CallerId);
            GArrivingRpc = true;
            Frame.Invoke(&Script);
            GArrivingRpc = false;
        }
    }

    //~ Events

    void TEventFanOut<SScriptRpcEvent>::Deliver(ECS::FRegistry& Registry, const SScriptRpcEvent& Event)
    {
        if (CEntityScript* Script = ScriptNet::ScriptAt(Registry, Event.Entity, Event.ScriptIndex))
        {
            ReceiveRpc(*Script, Event.RpcId, Event.CallerId, Event.Payload.GetData(), static_cast<uint32>(Event.Payload.Num()));
        }
    }

    void TEventFanOut<SScriptSyncArrivedEvent>::Deliver(ECS::FRegistry& Registry, const SScriptSyncArrivedEvent& Event)
    {
        if (CEntityScript* Script = ScriptNet::ScriptAt(Registry, Event.Entity, Event.ScriptIndex))
        {
            FinishSyncArrival(Registry, Event.Entity, *Script, reinterpret_cast<const char*>(Event.Names.GetData()), static_cast<uint32>(Event.Names.Num()));
        }
    }

    //~ FScriptSyncState

    FScriptSyncState::~FScriptSyncState() = default;

    FScriptSyncState::FEntry& FScriptSyncState::FindOrCapture(const CEntityScript& Script, bool& bOutCaptured)
    {
        bOutCaptured = false;
        for (FEntry& Entry : Entries)
        {
            if (Entry.Script == &Script)
            {
                return Entry;
            }
        }

        FEntry& Entry = Entries.EmplaceGetRef();
        Entry.Script = &Script;

        TVector<FProperty*> Fields;
        Script.GetClass()->GetNetReplicatedProperties(Fields);
        if (!Fields.IsEmpty())
        {
            Entry.Snapshot = MakeUnique<FPropertySnapshot>();
            Entry.Snapshot->Capture(Fields, &Script);
            bOutCaptured = true;
        }
        return Entry;
    }

    void FScriptSyncState::Prune(const SEntityScriptComponent& Scripts)
    {
        for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
        {
            const bool bHeld = Algo::AnyOf(Scripts.Scripts, [&](const TStrongObjectPtr<CEntityScript>& Held) { return Held.Get() == Entries[Index].Script; });
            if (!bHeld)
            {
                Entries.RemoveAt(Index);
            }
        }
    }

    //~ CScriptNetLibrary

    bool CScriptNetLibrary::IsReceivingRpc()
    {
        return ScriptNet::IsReceiving();
    }

    uint32 CScriptNetLibrary::GetReceivingCaller()
    {
        return ScriptNet::ReceivingCaller();
    }

    void* CScriptNetLibrary::FindRpcFunction(CEntityScript* Script, FName Name)
    {
        const FFunction* Function = Script != nullptr ? Script->GetClass()->FindFunction(Name) : nullptr;
        return Function != nullptr && Function->IsRpc() ? const_cast<FFunction*>(Function) : nullptr;
    }

    ERpcRoute CScriptNetLibrary::PlanRpcCall(CEntityScript* Script, void* Function)
    {
        return Script != nullptr && Function != nullptr ? PlanRpc(*Script, *static_cast<const FFunction*>(Function)) : ERpcRoute::RunHere;
    }

    void* CScriptNetLibrary::MakeRpcFrame(void* Function)
    {
        const FFunction* Typed = static_cast<const FFunction*>(Function);
        if (Typed == nullptr)
        {
            return nullptr;
        }
        const size_t Size = Math::Max<size_t>(Typed->GetParmsSize(), 1);
        void* Frame = Memory::Malloc(Size, FFunction::kFrameAlignment);
        Memory::Memzero(Frame, Size);
        Typed->InitializeFrame(Frame);
        return Frame;
    }

    void CScriptNetLibrary::SendRpcCall(CEntityScript* Script, void* Function, void* Frame)
    {
        const FFunction* Typed = static_cast<const FFunction*>(Function);
        if (Typed == nullptr || Frame == nullptr)
        {
            return;
        }
        if (Script != nullptr)
        {
            SendRpcFrame(*Script, *Typed, Frame);
        }
        DiscardRpcFrame(Function, Frame);
    }

    void CScriptNetLibrary::DiscardRpcFrame(void* Function, void* Frame)
    {
        const FFunction* Typed = static_cast<const FFunction*>(Function);
        if (Typed == nullptr || Frame == nullptr)
        {
            return;
        }
        Typed->DestructFrame(Frame);
        Memory::Free(Frame);
    }

    void* CScriptNetLibrary::FindSyncField(CEntityScript* Script, FName Name)
    {
        FProperty* Field = Script != nullptr ? Script->GetClass()->GetProperty(Name) : nullptr;
        return Field != nullptr && IsSyncFromOwner(Field) ? Field : nullptr;
    }

    void CScriptNetLibrary::ForwardSyncField(CEntityScript* Script, void* Field)
    {
        if (Script != nullptr && Field != nullptr)
        {
            SendSync(*Script, static_cast<FProperty*>(Field), false);
        }
    }

    //~ ScriptNet

    bool ScriptNet::RouteRpc(CEntityScript& Script, const char* FunctionName, const void* const* Values, uint32 NumValues)
    {
        const FFunction* Function = Script.GetClass()->FindFunction(FName(FunctionName));
        if (Function == nullptr || !Function->IsRpc())
        {
            GArrivingRpc = false;
            LOG_ERROR("The generated override for {}::{} found no FUNCTION(Rpc = ...) by that name, so the call runs here only.", *Script.GetClass()->GetName(), FunctionName);
            return false;
        }

        const ERpcRoute Route = PlanRpc(Script, *Function);
        if (Route == ERpcRoute::RunHere)
        {
            return false;
        }

        if (static_cast<uint32>(Function->GetArguments().Num()) != NumValues)
        {
            LOG_ERROR("The generated override for {}::{} passed {} arguments where the reflected RPC takes {}; rebuild so the two agree.",
                *Script.GetClass()->GetName(), FunctionName, NumValues, Function->GetArguments().Num());
            return false;
        }

        SendRpc(Script, *Function, Values);
        return Route == ERpcRoute::SendOnly;
    }

    bool ScriptNet::FindRpcTarget(const CEntityScript* Script, uint32 RpcId, ERpcTarget& OutTarget)
    {
        if (Script == nullptr)
        {
            return false;
        }

        const FRpcRoute Route = FindRpcRoute(*Script, RpcId);
        if (Route.Function != nullptr)
        {
            OutTarget = TargetOf(*Route.Function);
            return true;
        }
        if (Route.SyncField != nullptr)
        {
            OutTarget = Route.bCorrection ? ERpcTarget::Owner : ERpcTarget::Host;
            return true;
        }
        return false;
    }

    CEntityScript* ScriptNet::ScriptAt(ECS::FRegistry& Registry, ECS::FEntity Entity, uint32 Index)
    {
        const SEntityScriptComponent* Scripts = Registry.IsValid(Entity) ? Registry.TryGet<SEntityScriptComponent>(Entity) : nullptr;
        return Scripts != nullptr && Index < static_cast<uint32>(Scripts->Scripts.Num()) ? Scripts->Scripts[Index].Get() : nullptr;
    }

    void ScriptNet::QueueRpc(CWorld& World, ECS::FEntity Entity, uint32 ScriptIndex, uint32 RpcId, uint32 CallerId, TVector<uint8> Payload)
    {
        SScriptRpcEvent Event;
        Event.Entity      = Entity;
        Event.ScriptIndex = ScriptIndex;
        Event.RpcId       = RpcId;
        Event.CallerId    = CallerId;
        Event.Payload     = Move(Payload);
        ECS::SendEvent(World.GetEventQueues(), Event);
    }

    void ScriptNet::BeginSyncArrival(ECS::FRegistry& Registry, ECS::FEntity Entity, CEntityScript& Script, const char* Names, uint32 NamesSize)
    {
        if (void* Handle = ManagedHandleOf(Script))
        {
            if (auto* Fn = GDispatchSync.Get())
            {
                Fn(Handle, Names, static_cast<int32>(NamesSize), 0);
            }
            return;
        }

        // A snapshot already holds what the handlers last saw, and a missing one is taken now, before the old values are gone.
        bool bCaptured = false;
        Registry.GetOrEmplace<FScriptSyncState>(Entity).FindOrCapture(Script, bCaptured);
    }

    void ScriptNet::QueueSyncArrival(CWorld& World, ECS::FEntity Entity, uint32 ScriptIndex, const char* Names, uint32 NamesSize)
    {
        SScriptSyncArrivedEvent Event;
        Event.Entity      = Entity;
        Event.ScriptIndex = ScriptIndex;
        Event.Names.Append(reinterpret_cast<const uint8*>(Names), reinterpret_cast<const uint8*>(Names) + NamesSize);
        ECS::SendEvent(World.GetEventQueues(), Event);
    }

    void ScriptNet::PollSync(CWorld& World, ECS::FEntity Entity, const SEntityScriptComponent& Scripts, FScriptSyncState& State)
    {
        State.Prune(Scripts);

        const bool bHost  = IsHostWorld(&World);
        const bool bOwner = !bHost && IsOwnedHere(&World, Entity);
        for (const TStrongObjectPtr<CEntityScript>& Held : Scripts.Scripts)
        {
            CEntityScript* Script = Held.Get();

            // A C# Sync field reports its own writes through its setter, so only C++ fields are compared.
            if (Script == nullptr || !FEntityScriptAccess::IsReady(*Script) || FEntityScriptAccess::IsFaulted(*Script) || ToScriptClass(Script->GetClass()) != nullptr)
            {
                continue;
            }

            bool bCaptured = false;
            FPropertySnapshot* Snapshot = State.FindOrCapture(*Script, bCaptured).Snapshot.Get();

            // The first sighting is the baseline, which the entity's initial replication already carries.
            if (Snapshot == nullptr || bCaptured)
            {
                continue;
            }

            bool bDirty = false;
            for (int32 Index = 0; Index < Snapshot->Num(); ++Index)
            {
                FProperty* Field = Snapshot->Field(Index);
                const bool bWriter = bHost || (bOwner && IsSyncFromOwner(Field));
                if (bWriter)
                {
                    QuantizeSync(*Script, Field);
                }
                if (Snapshot->Matches(Index, Script))
                {
                    continue;
                }

                if (bHost)
                {
                    bDirty = true;
                }
                else if (bWriter)
                {
                    SendSync(*Script, Field, false);
                }
                NotifySyncChanged(*Script, Field, Snapshot->Slot(Index));
                Snapshot->Store(Index, Script);
            }

            if (bDirty)
            {
                MarkDirty(*Script);
            }
        }
    }

    bool ScriptNet::IsReceiving()
    {
        return GRpcDepth > 0;
    }

    uint32 ScriptNet::ReceivingCaller()
    {
        return GRpcDepth > 0 ? GRpcCaller : 0u;
    }
}
