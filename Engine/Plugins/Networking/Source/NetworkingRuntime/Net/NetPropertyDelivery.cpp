#include "RuntimePCH.h"
#include "NetPropertyDelivery.h"

#include "Core/Serialization/NetArchive.h"
#include "Net/NetReplication.h"
#include "Net/NetWorldState.h"

namespace Lumina::NetProperties
{
    namespace
    {
        constexpr int32 kHeaderBytes = 1 + sizeof(uint32);

        void GiveUp(FNetPropertyDelivery& Delivery, const FNetPropertyPacket& Packet)
        {
            Delivery.Resends.Append(Packet.Guids.begin(), Packet.Guids.end());
        }
    }

    void NoteReceived(FNetPropertyAcks& Acks, uint32 Seq)
    {
        if (Acks.Latest == 0)
        {
            Acks.Latest = Seq;
            Acks.Bits   = 0;
        }
        else if (Seq > Acks.Latest)
        {
            // The old newest becomes bit Shift - 1, and anything pushed past bit 31 is forgotten.
            const uint32 Shift = Seq - Acks.Latest;
            Acks.Bits   = Shift > 32 ? 0u : (Shift == 32 ? 0u : Acks.Bits << Shift) | (1u << (Shift - 1));
            Acks.Latest = Seq;
        }
        else if (Seq < Acks.Latest && Acks.Latest - Seq <= 32)
        {
            Acks.Bits |= 1u << (Acks.Latest - Seq - 1);
        }
        Acks.Repeats = 2;
    }

    bool WriteAck(FNetPropertyAcks& Acks, TVector<uint8>& OutBatch)
    {
        if (Acks.Repeats == 0 || Acks.Latest == 0)
        {
            return false;
        }
        --Acks.Repeats;

        TVector<uint8> Msg;
        FNetArchive Writer(Msg);
        uint8 Type = static_cast<uint8>(ENetMessage::PropertyAck);
        Writer << Type;
        Writer << Acks.Latest;
        Writer << Acks.Bits;
        Net::AppendFramedMessage(OutBatch, Msg.GetData(), static_cast<SIZE_T>(Msg.Num()));
        return true;
    }

    bool ReadAck(const uint8* Msg, SIZE_T Size, uint32& OutLatest, uint32& OutBits)
    {
        FNetArchive Reader(Msg, Size);
        uint8 Type = 0;
        Reader << Type;
        Reader << OutLatest;
        Reader << OutBits;
        return !Reader.HasError() && OutLatest != 0;
    }

    void Queue(FNetPropertyDelivery& Delivery, uint32 Guid, const uint8* FramedMessage, SIZE_T FramedSize)
    {
        Delivery.Pending.Append(FramedMessage, static_cast<int32>(FramedSize));
        Delivery.PendingGuids.Add(Guid);
    }

    void Pack(FNetPropertyDelivery& Delivery, double Now, TVector<TVector<uint8>>& OutPackets, int32& OutCount)
    {
        OutCount = 0;
        TVector<uint8> Body;
        int32 Cursor = 0;
        int32 GuidIndex = 0;
        const int32 End = Delivery.Pending.Num();
        while (Cursor + 2 <= End)
        {
            FNetPropertyPacket& Packet = Delivery.InFlight.EmplaceGetRef();
            Packet.Seq    = Delivery.NextSeq++;
            Packet.SentAt = Now;

            Body.Reset();
            FNetArchive Writer(Body);
            uint8 Type = static_cast<uint8>(ENetMessage::PropertyPacket);
            Writer << Type;
            Writer << Packet.Seq;

            // A message larger than the budget still goes, alone in its own packet.
            while (Cursor + 2 <= End)
            {
                const int32 Framed = 2 + (static_cast<int32>(Delivery.Pending[Cursor]) | (static_cast<int32>(Delivery.Pending[Cursor + 1]) << 8));
                if (Body.Num() > kHeaderBytes && Body.Num() + Framed > kPacketBudgetBytes)
                {
                    break;
                }
                Body.Append(Delivery.Pending.GetData() + Cursor, Framed);
                Packet.Guids.Add(Delivery.PendingGuids[GuidIndex++]);
                Cursor += Framed;
            }

            if (OutCount == OutPackets.Num())
            {
                OutPackets.EmplaceGetRef();
            }
            TVector<uint8>& Out = OutPackets[OutCount++];
            Out.Reset();
            Net::AppendFramedMessage(Out, Body.GetData(), static_cast<SIZE_T>(Body.Num()));
        }
        Delivery.Pending.Reset();
        Delivery.PendingGuids.Reset();
    }

    SIZE_T ReadPacketHeader(const uint8* Msg, SIZE_T Size, uint32& OutSeq)
    {
        FNetArchive Reader(Msg, Size);
        uint8 Type = 0;
        Reader << Type;
        Reader << OutSeq;
        return Reader.HasError() ? 0 : static_cast<SIZE_T>(kHeaderBytes);
    }

    uint32 ApplyAck(FNetPropertyDelivery& Delivery, uint32 Latest, uint32 Bits)
    {
        uint32 Lost = 0;
        Delivery.InFlight.RemoveAll([&](const FNetPropertyPacket& Packet)
        {
            if (Packet.Seq > Latest)
            {
                return false;
            }
            const uint32 Behind = Latest - Packet.Seq;
            const bool bArrived = Behind == 0 || (Behind <= 32 && (Bits & (1u << (Behind - 1))) != 0);
            if (!bArrived)
            {
                GiveUp(Delivery, Packet);
                ++Lost;
            }
            return true;
        });
        return Lost;
    }

    uint32 Expire(FNetPropertyDelivery& Delivery, double Now, double Timeout)
    {
        uint32 Lost = 0;
        Delivery.InFlight.RemoveAll([&](const FNetPropertyPacket& Packet)
        {
            if (Now - Packet.SentAt < Timeout)
            {
                return false;
            }
            GiveUp(Delivery, Packet);
            ++Lost;
            return true;
        });
        return Lost;
    }
}
