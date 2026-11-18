#pragma once

#include "Containers/Vector.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // Property changes go host to client in numbered unreliable packets on their own channel, so a lost one never holds up the rest.
    inline constexpr uint8 NetPropertyChannel = 2;
    inline constexpr uint8 NetChannelCount    = 3;

    // Client, the property packets that arrived, reported to the host as the newest and a bitfield of the 32 before it.
    struct FNetPropertyAcks
    {
        uint32 Latest = 0;
        uint32 Bits   = 0;

        // Packets older than this were sent before a spawn this client already holds, so applying them could undo it.
        uint32 Floor  = 0;

        // Each ack goes out on two ticks, so one lost ack rarely costs a resend.
        uint8  Repeats = 0;
    };

    struct FNetPropertyPacket
    {
        uint32          Seq    = 0;
        double          SentAt = 0.0;
        TVector<uint32> Guids;
    };

    // Host, one client's property delivery. A lost packet's entities go again as their current state, never as the old bytes.
    struct FNetPropertyDelivery
    {
        // Framed PropertyUpdate messages held until the client's next service, each with the entity it is about.
        TVector<uint8>  Pending;
        TVector<uint32> PendingGuids;

        TVector<FNetPropertyPacket> InFlight;
        TVector<uint32>             Resends;
        uint32                      NextSeq = 1;
    };

    namespace NetProperties
    {
        // Keeps packets under a datagram, so a property packet is never split into fragments that all have to arrive.
        inline constexpr int32 kPacketBudgetBytes = 1100;

        NETWORKINGRUNTIME_API void NoteReceived(FNetPropertyAcks& Acks, uint32 Seq);

        // Writes a framed PropertyAck while the last change still has repeats left, false when there is nothing to send.
        NETWORKINGRUNTIME_API bool WriteAck(FNetPropertyAcks& Acks, TVector<uint8>& OutBatch);
        NETWORKINGRUNTIME_API bool ReadAck(const uint8* Msg, SIZE_T Size, uint32& OutLatest, uint32& OutBits);

        NETWORKINGRUNTIME_API void Queue(FNetPropertyDelivery& Delivery, uint32 Guid, const uint8* FramedMessage, SIZE_T FramedSize);

        // Packs the held messages into framed PropertyPacket batches, one per datagram, and remembers what each carried.
        NETWORKINGRUNTIME_API void Pack(FNetPropertyDelivery& Delivery, double Now, TVector<TVector<uint8>>& OutPackets, int32& OutCount);

        // Reads a PropertyPacket's sequence and returns where its framed messages start, or 0 when it is malformed.
        NETWORKINGRUNTIME_API SIZE_T ReadPacketHeader(const uint8* Msg, SIZE_T Size, uint32& OutSeq);

        // A sequenced channel never delivers an older packet after a newer one, so any gap below the newest ack is lost. Returns how many were.
        NETWORKINGRUNTIME_API uint32 ApplyAck(FNetPropertyDelivery& Delivery, uint32 Latest, uint32 Bits);

        // Catches a lost packet that no later one reveals. Returns how many were given up on.
        NETWORKINGRUNTIME_API uint32 Expire(FNetPropertyDelivery& Delivery, double Now, double Timeout);
    }
}
