#include "RuntimePCH.h"
#include "Net/NetMessages.h"

#include "Core/Math/Math.h"
#include "Core/Object/Class.h"
#include "Core/Serialization/NetArchive.h"
#include "Memory/Memory.h"
#include "Memory/Memcpy.h"
#include "Net/NetReplication.h"
#include "Net/NetWorldState.h"
#include "Networking/NetworkEvents.h"
#include "World/ECS/EventQueue.h"

namespace Lumina
{
    void FScriptNetMessages::Register(ECS::FComponentTypeID TypeID, uint32 SenderOffset, TSpan<const uint32> EntityOffsets)
    {
        TScopeLock Scope(Lock);
        FScriptNetMessageLayout& Layout = Layouts[TypeID];
        Layout.SenderOffset = SenderOffset;
        Layout.EntityOffsets.Reset();
        Layout.EntityOffsets.Append(EntityOffsets.GetData(), static_cast<int32>(EntityOffsets.Num()));
    }

    bool FScriptNetMessages::Find(ECS::FComponentTypeID TypeID, FScriptNetMessageLayout& OutLayout) const
    {
        TScopeLock Scope(Lock);
        const FScriptNetMessageLayout* Layout = Layouts.Find(TypeID);
        if (Layout == nullptr)
        {
            return false;
        }
        OutLayout = *Layout;
        return true;
    }

    namespace NetMessagesInternal
    {
        uint32 TypeHashOf(const ECS::FComponentTypeInfo& Info)
        {
            return static_cast<uint32>(Info.Name.GetStableHash());
        }

        // Only a struct derived from SNetMessage may cross, so a peer cannot raise an engine event such as a session change.
        const CStruct* MessageStructOf(const ECS::FComponentTypeInfo& Info)
        {
            const CStruct* Struct = Info.GetStruct();
            return Struct != nullptr && Struct->IsChildOf(SNetMessage::StaticStruct()) ? Struct : nullptr;
        }

        ECS::FComponentTypeID FindTypeByHash(FNetWorldState& State, uint32 Hash)
        {
            if (const ECS::FComponentTypeID* Known = State.MessageTypes.Find(Hash))
            {
                return *Known;
            }
            const ECS::FComponentTypeRegistry& Types = ECS::FComponentTypeRegistry::Get();
            for (int32 Index = 0; Index < Types.Num(); ++Index)
            {
                const ECS::FComponentTypeID TypeID = static_cast<ECS::FComponentTypeID>(Index);
                if (TypeHashOf(Types.GetInfo(TypeID)) == Hash)
                {
                    State.MessageTypes.Add(Hash, TypeID);
                    return TypeID;
                }
            }
            return ECS::InvalidComponentTypeID;
        }

        // A scratch copy of one event at its own alignment, freed when it goes out of scope.
        struct FEventScratch
        {
            explicit FEventScratch(const ECS::FComponentTypeInfo& Info)
                : Data(Memory::Malloc(Math::Max(Info.Size, 1u), Math::Max(Info.Alignment, 1u)))
            {
            }

            ~FEventScratch()
            {
                Memory::Free(Data);
            }

            LE_NO_COPYMOVE(FEventScratch);

            void* Data;
        };

        void TranslateEntities(uint8* Bytes, const FScriptNetMessageLayout& Layout, uint32 Size, const TFunction<uint32(uint32)>& Translate)
        {
            if (!Translate)
            {
                return;
            }
            for (uint32 Offset : Layout.EntityOffsets)
            {
                if (Offset + sizeof(uint32) <= Size)
                {
                    uint32 Id = 0;
                    Memory::Memcpy(&Id, Bytes + Offset, sizeof(uint32));
                    Id = Translate(Id);
                    Memory::Memcpy(Bytes + Offset, &Id, sizeof(uint32));
                }
            }
        }

        void WriteSender(uint8* Bytes, uint32 Offset, uint32 Size, uint32 Sender)
        {
            if (Offset + sizeof(uint32) <= Size)
            {
                Memory::Memcpy(Bytes + Offset, &Sender, sizeof(uint32));
            }
        }
    }

    bool NetMessages::Write(FNetWorldState& State, const FScriptNetMessages& Scripts, ECS::FComponentTypeID TypeID, const void* Message, TVector<uint8>& OutMsg)
    {
        using namespace NetMessagesInternal;

        const ECS::FComponentTypeInfo& Info = ECS::FComponentTypeRegistry::Get().GetInfo(TypeID);
        const CStruct* Struct = MessageStructOf(Info);
        FScriptNetMessageLayout Layout;
        if (Struct == nullptr && !Scripts.Find(TypeID, Layout))
        {
            return false;
        }

        FNetArchive Writer(OutMsg);
        uint8 Type = static_cast<uint8>(ENetMessage::EventMessage);
        Writer << Type;
        uint32 Hash = TypeHashOf(Info);
        Writer.SerializeBits(&Hash, 32);
        Net::BindEntityIds(Writer, State);

        if (Struct != nullptr)
        {
            Struct->NetSerializeAll(Writer, const_cast<void*>(Message));
        }
        else
        {
            TVector<uint8> Bytes;
            Bytes.Append(static_cast<const uint8*>(Message), static_cast<int32>(Info.Size));
            TranslateEntities(Bytes.GetData(), Layout, Info.Size, Writer.EntityToNetGUID);
            uint32 Size = Info.Size;
            Writer << Size;
            Writer.Serialize(Bytes.GetData(), Size);
        }
        return !Writer.HasError() && static_cast<SIZE_T>(OutMsg.Num()) <= Net::MaxFramedMessageSize / 2;
    }

    void NetMessages::Apply(FNetWorldState& State, const FScriptNetMessages& Scripts, uint32 Sender, const uint8* Data, SIZE_T Size)
    {
        using namespace NetMessagesInternal;

        if (State.Events == nullptr)
        {
            return;
        }

        FNetArchive Reader(Data, Size);
        uint8 Type = 0;
        Reader << Type;
        uint32 Hash = 0;
        Reader.SerializeBits(&Hash, 32);
        const ECS::FComponentTypeID TypeID = FindTypeByHash(State, Hash);
        if (Reader.HasError() || TypeID == ECS::InvalidComponentTypeID)
        {
            return;
        }

        const ECS::FComponentTypeInfo& Info = ECS::FComponentTypeRegistry::Get().GetInfo(TypeID);
        Net::BindEntityIds(Reader, State);
        FEventScratch Scratch(Info);

        if (const CStruct* Struct = MessageStructOf(Info))
        {
            Struct->InitializeStruct(Scratch.Data);
            Struct->NetSerializeAll(Reader, Scratch.Data);
            if (!Reader.HasError())
            {
                static_cast<SNetMessage*>(Scratch.Data)->Sender = Sender;
                State.Events->Get(TypeID).Send(Scratch.Data);
            }
            Struct->DestroyStruct(Scratch.Data);
            return;
        }

        FScriptNetMessageLayout Layout;
        uint32 Bytes = 0;
        Reader << Bytes;
        if (!Scripts.Find(TypeID, Layout) || Bytes != Info.Size || Reader.HasError())
        {
            return;
        }
        Reader.Serialize(Scratch.Data, Bytes);
        if (Reader.HasError())
        {
            return;
        }
        uint8* Raw = static_cast<uint8*>(Scratch.Data);
        TranslateEntities(Raw, Layout, Bytes, Reader.NetGUIDToEntity);
        WriteSender(Raw, Layout.SenderOffset, Bytes, Sender);
        State.Events->Get(TypeID).Send(Raw);
    }

    void NetMessages::Deliver(ECS::FEventQueues& Events, const FScriptNetMessages& Scripts, ECS::FComponentTypeID TypeID, const void* Message, uint32 Sender)
    {
        using namespace NetMessagesInternal;

        const ECS::FComponentTypeInfo& Info = ECS::FComponentTypeRegistry::Get().GetInfo(TypeID);
        FEventScratch Scratch(Info);
        if (const CStruct* Struct = MessageStructOf(Info))
        {
            Struct->InitializeStruct(Scratch.Data);
            Struct->CopyStruct(Scratch.Data, Message);
            static_cast<SNetMessage*>(Scratch.Data)->Sender = Sender;
            Events.Get(TypeID).Send(Scratch.Data);
            Struct->DestroyStruct(Scratch.Data);
            return;
        }

        FScriptNetMessageLayout Layout;
        if (Scripts.Find(TypeID, Layout))
        {
            Memory::Memcpy(Scratch.Data, Message, Info.Size);
            WriteSender(static_cast<uint8*>(Scratch.Data), Layout.SenderOffset, Info.Size, Sender);
            Events.Get(TypeID).Send(Scratch.Data);
        }
    }
}
